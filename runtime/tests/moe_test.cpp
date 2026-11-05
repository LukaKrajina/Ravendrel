// ============================================================================
// moe_test.cpp —— MoE 专家结构自动化推导（曲率分箱 × 乘积键 × 因子化底座）
//
// 断言的是「性质」，不是「跑一遍不崩」：
//   · 拓扑由 pB 自动推导，不再硬编码 num_experts；
//   · savings = G×K/(G+K) ≥ 1，规模越大越省（「比 G×K 更高效」的量化）；
//   · 曲率门控 σ(−g) 单调递减（g 大 → 切对偶）；
//   · 乘积键路由在可分解门控下 == 全枚举 top-1，但物化 O(G+K)；
//   · 因子化底座：additive 权重重建误差小，存储 G+K 块而非 G×K 块。
// ============================================================================

#include "numqk/moe.hpp"

#include <cmath>
#include <cstdio>
#include <vector>

namespace
{
    int g_fail = 0;
    void check(bool ok, const char *what)
    {
        std::printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
        if (!ok)
            ++g_fail;
    }
    void check_near(double a, double b, double tol, const char *what)
    {
        const bool ok = std::fabs(a - b) <= tol;
        std::printf("  %s %s (%.4g vs %.4g)\n", ok ? "ok  " : "FAIL", what, a, b);
        if (!ok)
            ++g_fail;
    }
    double rel_err(const std::vector<double> &a, const std::vector<double> &b)
    {
        double n = 0.0, d = 0.0;
        for (size_t i = 0; i < a.size(); ++i)
        {
            n += (a[i] - b[i]) * (a[i] - b[i]);
            d += a[i] * a[i];
        }
        return d > 0.0 ? std::sqrt(n / d) : 0.0;
    }
} // namespace

int main()
{
    using namespace numqk::moe;

    std::printf("[1] 拓扑由 pB 自动推导（不再硬编码 num_experts）\n");
    {
        const double pbs[] = {0.5, 1.0, 3.0, 5.0};
        const size_t expG[] = {2, 2, 3, 4};
        const size_t expK[] = {2, 2, 6, 8};
        size_t prevG = 0, prevK = 0;
        for (size_t i = 0; i < 4; ++i)
        {
            MoETopology t = auto_moe_topology(pbs[i]);
            std::printf("    pB=%.1f → G=%zu K=%zu 虚拟=%zu 物化=%zu 省 %.2fx\n",
                        pbs[i], t.groups, t.experts_per, t.virtual_experts,
                        t.materialized, t.savings);
            check(t.groups == expG[i] && t.experts_per == expK[i], "G/K 符合推导表");
            check(t.virtual_experts == t.groups * t.experts_per, "虚拟专家 = G×K");
            check(t.materialized == t.groups + t.experts_per, "物化块 = G+K");
            check(t.savings >= 1.0, "savings ≥ 1");
            check(t.groups >= prevG && t.experts_per >= prevK, "G/K 随 pB 单调不减");
            prevG = t.groups; prevK = t.experts_per;
        }
        check_near(auto_moe_topology(5.0).savings, 32.0 / 12.0, 1e-12, "5pB 省 2.67×");
        check(auto_moe_topology(5.0).savings > auto_moe_topology(1.0).savings,
              "规模越大越省（高效性随规模增长）");
    }

    std::printf("[2] 曲率门控 σ(−g) 单调递减（g 大 → 切对偶专家）\n");
    {
        check_near(curvature_gate(0.0), 0.5, 1e-12, "g=0 → 0.5（对偶均衡）");
        check(curvature_gate(10.0) < 0.5, "g=10 → 权重 < 0.5");
        check(curvature_gate(-10.0) > 0.5, "g=-10 → 权重 > 0.5");
        bool mono = true;
        for (double g = -8.0; g <= 8.0; g += 0.5)
            if (curvature_gate(g + 0.5) > curvature_gate(g))
                mono = false;
        check(mono, "σ(−g) 单调递减");
    }

    std::printf("[3] 乘积键路由 == 全枚举 top-1（物化 O(G+K)）\n");
    {
        const size_t G = 4, K = 8;
        std::vector<double> gg(G), kg(K);
        for (size_t i = 0; i < G; ++i) gg[i] = std::sin(0.7 * i + 1.0) + 1.1;
        for (size_t j = 0; j < K; ++j) kg[j] = std::cos(0.5 * j + 0.3) + 1.1;

        // 全枚举 top-1
        size_t bg = 0, bk = 0; double bm = -1e300;
        for (size_t i = 0; i < G; ++i)
            for (size_t j = 0; j < K; ++j)
                if (gg[i] * kg[j] > bm) { bm = gg[i] * kg[j]; bg = i; bk = j; }

        ProductKeyRoute r = product_key_route(gg.data(), G, kg.data(), K);
        check(r.group == bg && r.expert == bk, "乘积键 top-1 == 全枚举 top-1");
        check_near(r.weight, bm, 1e-12, "门控权重 == 全枚举最大权重");
    }

    std::printf("[4] 因子化底座：additive 权重重建误差小，存储 G+K 块\n");
    {
        const size_t G = 4, K = 8, d = 256;
        // 构造 additive 权重 W_{g,k} = U_g + V_k，U/V 都是低频平滑信号
        std::vector<std::vector<double>> U0(G, std::vector<double>(d)),
                                         V0(K, std::vector<double>(d));
        for (size_t g = 0; g < G; ++g)
            for (size_t i = 0; i < d; ++i)
                U0[g][i] = 2.0 * std::cos(2.0 * M_PI * i / 64.0 + g);
        for (size_t k = 0; k < K; ++k)
            for (size_t i = 0; i < d; ++i)
                V0[k][i] = 0.5 * std::cos(2.0 * M_PI * i / 32.0 + 0.3 * k);

        std::vector<double> W(G * K * d);
        for (size_t g = 0; g < G; ++g)
            for (size_t k = 0; k < K; ++k)
                for (size_t i = 0; i < d; ++i)
                    W[(g * K + k) * d + i] = U0[g][i] + V0[k][i];

        FactorizedExperts fe = factorize_experts(W, G, K, d, 32, 6, 256);
        check(fe.u_blocks.size() == G && fe.v_blocks.size() == K,
              "物化 G+K 块（不是 G×K 块）");

        double worst = 0.0;
        for (size_t g = 0; g < G; ++g)
            for (size_t k = 0; k < K; ++k)
            {
                const std::vector<double> back = rebuild_expert(fe, g, k);
                const std::vector<double> ref(W.begin() + (g * K + k) * d,
                                              W.begin() + (g * K + k) * d + d);
                const double e = rel_err(ref, back);
                if (e > worst) worst = e;
            }
        check(worst < 0.05, "additive 权重重建误差 < 5%（交互项≈0）");
    }

    std::printf("[5] 交互项越大，additive 分解误差越大（信息损失有界可见）\n");
    {
        const size_t G = 2, K = 2, d = 128;
        double e_eps0 = 0.0, e_eps1 = 0.0;
        for (int t = 0; t < 2; ++t)
        {
            const double eps = (t == 0) ? 0.0 : 2.0;
            std::vector<double> W(G * K * d);
            for (size_t g = 0; g < G; ++g)
                for (size_t k = 0; k < K; ++k)
                    for (size_t i = 0; i < d; ++i)
                        W[(g * K + k) * d + i] =
                            std::cos(2.0 * M_PI * i / 64.0 + g) + 0.5 * std::cos(2.0 * M_PI * i / 32.0 + k)
                            + eps * std::cos(2.0 * M_PI * i / 2.0) * static_cast<double>(g * k); // 高频交互（Nyquist，rank=6 必截断）
            FactorizedExperts fe = factorize_experts(W, G, K, d, 32, 6, 256);
            double worst = 0.0;
            for (size_t g = 0; g < G; ++g)
                for (size_t k = 0; k < K; ++k)
                {
                    const std::vector<double> back = rebuild_expert(fe, g, k);
                    const std::vector<double> ref(W.begin() + (g * K + k) * d,
                                                  W.begin() + (g * K + k) * d + d);
                    const double e = rel_err(ref, back);
                    if (e > worst) worst = e;
                }
            (t == 0 ? e_eps0 : e_eps1) = worst;
        }
        check(e_eps0 < 0.05, "无交互（eps=0）误差小");
        check(e_eps1 > e_eps0, "有交互（eps>0）误差增大");
    }

    std::printf("[6] 对偶前向融合：2 专家对偶泛化为任意两专家（不替换，是增强）\n");
    {
        check_near(inversion(2.0), 0.5, 1e-12, "T-对偶反转：inversion(2)=0.5");
        check_near(inversion(0.5), 2.0, 1e-12, "T-对偶反转：inversion(0.5)=2.0");
        const double pw = polymer_weight(1.0, 0.5, 3.0);
        check_near(pw, std::pow(2.0 * std::cosh(0.5), 3.0), 1e-12,
                   "polymer 权重 = (2·cosh(0.5))^3");

        // 对偶融合的权重行为：g 大 → 切对偶 B；g 小 → 留原空间 A；g=0 → 均衡。
        // 注意：对偶项 inversion·polymer ≈ 11.46 数值上大于原空间项（θ=1 → 1），
        // 所以「切对偶」在这里表现为输出变大，不是变小。
        const double f_neg = dual_fuse(1.0, 1.0, -10.0, 1.0); // g<0 → σ(−g)→1 → 偏 A
        const double f_pos = dual_fuse(1.0, 1.0, +10.0, 1.0); // g>0 → σ(−g)→0 → 偏 B
        const double f_mid = dual_fuse(1.0, 1.0, 0.0, 1.0);   // g=0 → 0.5 对半
        check(f_neg > 0.9 && f_neg < 1.1, "g→−∞ 输出≈纯原空间（θ=1 → ≈1）");
        check(f_pos > 11.0 && f_pos < 12.0, "g→+∞ 输出≈纯对偶（inversion·polymer≈11.46）");
        check(f_mid > f_neg && f_mid < f_pos, "g=0 输出介于原空间与对偶之间（0.5 对半）");

        // 对偶融合 MoE 前向：路由选组 top-1 + 组内 top-2，输出 == 手算 dual_fuse
        const size_t G = 4, K = 8;
        std::vector<double> act(G * K), gg(G), kg(K);
        for (size_t i = 0; i < G; ++i) gg[i] = std::sin(0.7 * i + 1.0) + 1.1;
        for (size_t j = 0; j < K; ++j) kg[j] = std::cos(0.5 * j + 0.3) + 1.1;
        for (size_t i = 0; i < G; ++i)
            for (size_t j = 0; j < K; ++j)
                act[i * K + j] = std::sin(0.3 * i + 0.4 * j);

        // 手算：组 top-1
        size_t bg = 0; double bgv = -1e300;
        for (size_t i = 0; i < G; ++i) if (gg[i] > bgv) { bgv = gg[i]; bg = i; }
        // 组内 top-2
        size_t e1 = 0, e2 = 1;
        if (kg[e2] > kg[e1]) { size_t t = e1; e1 = e2; e2 = t; }
        for (size_t j = 2; j < K; ++j)
            if (kg[j] > kg[e1]) { e2 = e1; e1 = j; }
            else if (kg[j] > kg[e2]) { e2 = j; }
        const double expect = dual_fuse(act[bg * K + e1], act[bg * K + e2], 0.7, 1.3);

        DualRoute dr = dual_moe_forward(act.data(), G, K, gg.data(), kg.data(), 0.7, 1.3);
        check(dr.group == bg, "路由组 == 手算组 top-1");
        check(dr.expert_a == e1 && dr.expert_b == e2, "路由专家 == 手算组内 top-2");
        check_near(dr.value, expect, 1e-12, "对偶融合输出 == 手算 dual_fuse");
    }

    std::printf("\n%s（失败 %d 项）\n", g_fail == 0 ? "MOE: 全部通过" : "MOE: 失败", g_fail);
    return g_fail == 0 ? 0 : 1;
}