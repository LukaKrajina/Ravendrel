#pragma once
//
// moe.hpp —— Chimera MoE 专家结构的自动化推导（曲率分箱 × 乘积键 × 因子化底座
//           × 对偶前向融合）
//
// 像Jarvis 的 MoE 版 config 曾硬编码 num_experts=12，但真实的 chimera_moe
// 只是「2 专家对偶」——硬编码既不准、也无意义。所以我便把专家结构改成**由 pB
// 自动推导**，并用「乘积键路由 + 因子化底座 + 对偶融合」让物化成本 O(G+K)
// 而非 O(G×K)，这样会好很多。
//
// 四个关键概念：
//
//   1. 曲率分箱（组 G）：Chimera 的路由信号是度规（曲率）g，σ(−g) 在曲率符号
//      翻转处切专家。组 = 曲率 regime 的分箱，pB 越大可分辨越细。
//        G = clamp(2 + floor(log2(pB)), 2, 4)
//        0.5pB→2（对偶双区）  1pB→2  3pB→3  5pB→4（强/弱 × 正/负曲率）
//
//   2. 乘积键路由：G×K 个虚拟专家 = {1..G} × {1..K} 的笛卡尔积。门控
//      softmax_g ⊗ softmax_k 只存 (G+K) 个标量，而不是 (G×K) 个。这是
//      Google PEER / DeepSeekMoE 都用的「乘积键」思想：路由参数 O(G+K)。
//
//   3. 因子化底座：专家权重 W_{g,k} ≈ U_g + V_k（two-way 主效应分解），
//      物化只存 G 个组底座块 + K 个全局调制块（每块用 pb.hpp 的点参数压缩），
//      不物化 G×K 个独立权重。
//
//   4. 对偶前向融合（dual_fuse）：**不替换** chimera_moe 的 2 专家对偶，而是
//      把「专家 A（原空间）× 专家 B（T-对偶反转 × polymer 铁磁）」的曲率玻尔
//      兹曼混合，泛化成「任意两个专家的对偶融合算子」。G×K 里路由出的 top-2
//      专家两两做对偶前向融合 —— 表达力从 G×K 个独立专家升级为 C(G×K,2) 个
//      对偶融合对（组合数增长），而路由仍 O(G+K)、融合是 O(1) 算子。
//
// 「比 G×K 更高效」的量化口径：savings = (G×K) / (G+K)。
//   0.5pB → 4/4 = 1.0×（小规模退化为普通 MoE，无害）
//   5pB   → 32/12 = 2.67×（路由 + 底座都省，点参数又叠一层 pb 压缩）
//
// 依赖：仅 STL + pb.hpp，无 QVM / Kokkos，可独立单测。

#include "numqk/pb.hpp"

#include <cmath>
#include <cstddef>
#include <vector>

namespace numqk
{
namespace moe
{
    /// MoE 拓扑（由 pB 自动推导，不再硬编码）。
    struct MoETopology
    {
        double pB = 0.0;             ///< 模型点参数量（1 pB ≡ 1000 B 有效容量）
        size_t groups = 0;           ///< G：曲率分箱数
        size_t experts_per = 0;      ///< K：每组专家数
        size_t virtual_experts = 0;  ///< G×K：等效专家数（表达力上限）
        size_t materialized = 0;     ///< G+K：实际物化块数（路由 + 底座成本）
        double savings = 1.0;        ///< (G×K)/(G+K)：比朴素 G×K 更高效的倍数
    };

    /// 自动推导 MoE 拓扑。
    inline MoETopology auto_moe_topology(double pB)
    {
        MoETopology t;
        t.pB = pB;
        if (pB <= 0.0)
            pB = 0.5;

        // 组数 G（曲率分箱）：度规符号 → 对偶双区；|g| 量级 → 细分 regime
        double g = 2.0 + std::floor(std::log2(pB));
        if (g < 2.0) g = 2.0;
        if (g > 4.0) g = 4.0;
        t.groups = static_cast<size_t>(g);

        // 每组专家 K（组内调制）：点参数是流形坐标，随 pB 线性可辨的子专家数
        double k = std::round(2.0 * pB);
        if (k < 2.0) k = 2.0;
        if (k > 8.0) k = 8.0;
        t.experts_per = static_cast<size_t>(k);

        t.virtual_experts = t.groups * t.experts_per;
        t.materialized = t.groups + t.experts_per;
        t.savings = static_cast<double>(t.virtual_experts) /
                    static_cast<double>(t.materialized);
        return t;
    }

    /// 曲率门控：σ(−g) = 1/(1+e^g)，与 chimera_moe 的 boltzmann2(1,0,−g) 同语义。
    /// g 越大（曲率越大 / 越靠近无穷远边界）→ 权重越小 → 切到对偶。
    inline double curvature_gate(double g)
    {
        if (g > 100.0) return 0.0;
        if (g < -100.0) return 1.0;
        return 1.0 / (1.0 + std::exp(g));
    }

    /// 乘积键路由的结果：选 1 组 + 组内 1 专家。
    struct ProductKeyRoute
    {
        size_t group = 0;    ///< 选中的曲率组
        size_t expert = 0;   ///< 组内选中的专家
        double weight = 0.0; ///< 门控权重 = gate_group × gate_expert
    };

    /// 乘积键路由：把 G×K 个专家的选择分解为「选组」×「组内选专家」两个独立
    /// top-1。物化成本 O(G+K)；在可分解门控（gate_{g,k}=gate_g·gate_k）下，
    /// 结果等价于全枚举 G×K 的 top-1。
    inline ProductKeyRoute product_key_route(const double *group_gates, size_t G,
                                             const double *intra_gates, size_t K)
    {
        ProductKeyRoute r;
        double gb = -1e300, kb = -1e300;
        for (size_t i = 0; i < G; ++i)
            if (group_gates[i] > gb) { gb = group_gates[i]; r.group = i; }
        for (size_t j = 0; j < K; ++j)
            if (intra_gates[j] > kb) { kb = intra_gates[j]; r.expert = j; }
        r.weight = gb * kb;
        return r;
    }

    /// 因子化专家底座：G×K 个虚拟专家 ≈ U_g + V_k（two-way 主效应分解）。
    /// 物化只存 G 个组底座块 + K 个全局调制块，每块用 pb.hpp 的点参数压缩。
    struct FactorizedExperts
    {
        std::vector<numqk::pb::PointParams> u_blocks; ///< G 个组底座
        std::vector<numqk::pb::PointParams> v_blocks; ///< K 个全局调制
        size_t d = 0; ///< 每个专家的权重维度
    };

    /// additive 因子化：U_g = 组内均值（底座），V_k = 组内残差对 g 的均值（调制）。
    /// 交互项（W_{g,k} − U_g − V_k）是可加假设的残差 —— 对「组底座 + 组内调制」
    /// 可分的情况残差 ≈ 0；不可分时残差就是信息损失（与 pb 的高频截断同源）。
    inline FactorizedExperts factorize_experts(const std::vector<double> &W,
                                               size_t G, size_t K, size_t d,
                                               size_t block, size_t rank, size_t q)
    {
        FactorizedExperts fe;
        fe.d = d;
        for (size_t g = 0; g < G; ++g)
        {
            std::vector<double> u(d, 0.0);
            for (size_t k = 0; k < K; ++k)
                for (size_t i = 0; i < d; ++i)
                    u[i] += W[(g * K + k) * d + i] / static_cast<double>(K);
            fe.u_blocks.push_back(numqk::pb::compress(u, block, rank, q));
        }
        for (size_t k = 0; k < K; ++k)
        {
            std::vector<double> v(d, 0.0);
            for (size_t g = 0; g < G; ++g)
            {
                const std::vector<double> u = numqk::pb::decompress(fe.u_blocks[g]);
                for (size_t i = 0; i < d; ++i)
                    v[i] += (W[(g * K + k) * d + i] - u[i]) / static_cast<double>(G);
            }
            fe.v_blocks.push_back(numqk::pb::compress(v, block, rank, q));
        }
        return fe;
    }

    /// 重建虚拟专家 W_{g,k} = U_g + V_k（按需从物化的 G+K 块重建，不常驻）。
    inline std::vector<double> rebuild_expert(const FactorizedExperts &fe,
                                              size_t g, size_t k)
    {
        std::vector<double> u = numqk::pb::decompress(fe.u_blocks[g]);
        const std::vector<double> v = numqk::pb::decompress(fe.v_blocks[k]);
        for (size_t i = 0; i < fe.d; ++i)
            u[i] += v[i];
        return u;
    }

    // ── 对偶前向融合（从 chimera_moe 的 2 专家对偶泛化，非替换）────────────────
    //
    // 原 chimera_moe 是「全局单一对偶」：专家 A（原空间线性）× 专家 B（T-对偶
    // 反转 × polymer 铁磁权重），曲率 σ(−g) 玻尔兹曼混合。这里把它从「1 个全局
    // 对偶对」泛化成「任意两个专家的对偶融合算子」，嵌入 G×K 乘积键结构：
    //
    //   · 表达力：G×K 个独立专家 → C(G×K, 2) 个「对偶融合对」（组合数增长）
    //   · 高效性：路由仍乘积键 O(G+K)，对偶融合是 O(1) 算子，不物化组合数
    //   · 语义：对偶结构（原空间 × T-对偶反转 × polymer 铁磁）原样保留，
    //     不是「替换」——单专家前向退化时就是原 2 专家对偶。

    /// T-对偶反转（几何反演）x → 1/x：把「靠近 0 的小值」与「发散的大值」对调，
    /// 是原空间 ↔ 对偶空间的映射（对应 chimera_moe 的 inversion）。
    inline double inversion(double x)
    {
        return 1.0 / x;
    }

    /// polymer（铁磁自旋链）配分函数权重。β=1,h=0.5,L=3 时是常数因子，
    /// 这里用单自旋弱耦合近似 Z ≈ (2·cosh(βh))^L，保留公式以便将来换可学习 β/h。
    inline double polymer_weight(double beta, double h, double len)
    {
        return std::pow(2.0 * std::cosh(beta * h), len);
    }

    /// 对偶前向融合：两个专家经曲率玻尔兹曼软门控混合。
    ///   expert_a：原空间（×θ 线性）
    ///   expert_b：T-对偶（inversion × polymer）
    ///   g 大（曲率大 / 靠近无穷远边界）→ σ(−g)→0 → 输出切到对偶 B。
    inline double dual_fuse(double expert_a, double expert_b, double g, double theta)
    {
        const double wa = curvature_gate(g);      // σ(−g)
        const double wb = 1.0 - wa;
        const double a = expert_a * theta;        // 原空间线性
        const double b = inversion(expert_b + 1e-3) * polymer_weight(1.0, 0.5, 3.0);
        return wa * a + wb * b;
    }

    /// 对偶融合路由结果：乘积键选 top-2（组 top-1 + 组内 top-2），两两对偶融合。
    struct DualRoute
    {
        size_t group = 0;
        size_t expert_a = 0;  ///< 组内 top-1（原空间专家）
        size_t expert_b = 0;  ///< 组内 top-2（对偶伙伴）
        double value = 0.0;   ///< 对偶融合输出
        double weight_a = 0.5;///< σ(−g)
        double weight_b = 0.5;///< 1−σ(−g)
    };

    /// 对偶融合 MoE 前向：从 G×K 个专家激活（标量）里，乘积键路由选 top-2，
    /// 做曲率对偶融合。这是「每 2 个专家对偶前向融合」的集成入口。
    ///
    /// activations[(g*K+k)] = 专家 (g,k) 对当前输入的标量响应。
    inline DualRoute dual_moe_forward(const double *activations, size_t G, size_t K,
                                      const double *group_gates,
                                      const double *intra_gates,
                                      double g, double theta)
    {
        DualRoute dr;
        // 组 top-1（曲率门控决定 regime）
        double gb = -1e300;
        for (size_t i = 0; i < G; ++i)
            if (group_gates[i] > gb) { gb = group_gates[i]; dr.group = i; }
        // 组内 top-2（对偶对：原空间 + 对偶伙伴）
        size_t e1 = 0, e2 = 1;
        if (intra_gates[e2] > intra_gates[e1]) { size_t t = e1; e1 = e2; e2 = t; }
        for (size_t j = 2; j < K; ++j)
        {
            if (intra_gates[j] > intra_gates[e1]) { e2 = e1; e1 = j; }
            else if (intra_gates[j] > intra_gates[e2]) { e2 = j; }
        }
        dr.expert_a = e1;
        dr.expert_b = e2;
        const double a = activations[dr.group * K + e1];
        const double b = activations[dr.group * K + e2];
        dr.weight_a = curvature_gate(g);
        dr.weight_b = 1.0 - dr.weight_a;
        dr.value = dr.weight_a * (a * theta) +
                   dr.weight_b * (inversion(b + 1e-3) * polymer_weight(1.0, 0.5, 3.0));
        return dr;
    }
}
}