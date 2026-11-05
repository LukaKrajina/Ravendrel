#pragma once
// ============================================================================
// TauScale.hpp —— 量子时间缩微调度（QτS，Quantum τ-Scaling）
//
// 何庭波主任《A time scaling theory for multi-layer electronic systems》
// （ChinaXiv:202605.00224v2，韬定律 / τ scaling）。论文主张后摩尔时代的进步
// 指标从"空间（晶体管面积）"换成"时间"：把单一特征时间常数 τ 作为从晶体管
// 到数据中心工作负载的统一优化目标，各层 τ 分层复合，并按代际规则压缩。
// 十分有亮点，很巧妙的思路。本文我将该论文的核心公式进行对我们已有量子 AI 层基础下进行延申（映射关系见 TauAbi.hpp）。
// 区别：
//
//   论文层次            量子 AI 对应层        本文件里的量
//   τ_transistor 开关    态制备（编码/基态）    TauLayers.state
//   τ_circuit 关键路径   线路关键路径深度×门时间 TauLayers.gate
//   τ_chip 访存延迟      测量读出（shots）       TauLayers.measure
//   τ_system 端到端      跨块/跨节点通信         TauLayers.comm
//
// 计划：
//   1. 分层复合   τ = f(τ_t, τ_c, τ_ch, τ_s)   → compose()：加权线性复合，
//      权重 w ≥ 1 表示本层引入的组织/通信开销（论文：每层 τ 由下层加上
//      本层组织开销构成）。
//   2. 代际法则   τ_{n+1} = τ_n / α             → generational()：α 领域相关
//      （论文：移动 1.3、自动驾驶 1.5、AI 高达 10）。
//   3. 关键路径   f ≤ 1/τ（等最慢路径走完）      → critical_path()：DAG 最长
//      依赖链 DP；fold_depth()：LogicFolding（垂直层折叠）把串行深度 L 折进
//      T 个并行层，深度近似除以 T；gear_ratio()：齿轮比（混合键合间距/顶层
//      金属间距的量子化 = 跨层边数/层内边数），论文给的经验阈值为 3。
//   4. 时间预算   AI 的 α→10（吞吐=经济价值）    → budget_route()：预算 B 下
//      选价值最大子集，λ 为"时间的影子价格"（拉格朗日对偶）；route_cost()
//      计算关键路径+跨层通信的总调度代价。
//
// 只依赖标准库，header-only，风格与 numqk/moe.hpp 一致（namespace + inline）。
// rvdl 标量入口见 qml/TauAbi.hpp（rvdl_tau_*）。
// ============================================================================

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <tuple>
#include <vector>

namespace numqk
{
    namespace tau
    {

        // ─── 1. 分层 τ 与复合 ────────────────────────────────────────────────────

        /** 量子 AI 管线的四层特征时间（单位：任意一致的抽象时间） */
        struct TauLayers
        {
            double state   = 0.0; // 态制备时间（输入编码 → |φ(x)⟩）
            double gate    = 0.0; // 门执行时间（关键路径深度 × 单门时间）
            double measure = 0.0; // 测量读出时间（shot 数 × 单次读出）
            double comm    = 0.0; // 跨块/跨节点通信时间（穿梭、跨层边）
        };

        /** 各层的组织开销权重（w ≥ 1；1 表示本层不引入额外开销） */
        struct TauWeights
        {
            double w_state   = 1.0;
            double w_gate    = 1.0;
            double w_measure = 1.0;
            double w_comm    = 1.0;
        };

        /**
         * 论文公式 τ = f(τ_transistor, τ_circuit, τ_chip, τ_system) 的电路层形式：
         *     τ_total = w_state·τ_state + w_gate·τ_gate + w_measure·τ_measure + w_comm·τ_comm
         * f 取加权线性复合：上层 τ 由下层之和加上本层组织开销（权重）构成。
         */
        inline double compose(const TauLayers &t, const TauWeights &w)
        {
            return w.w_state * t.state + w.w_gate * t.gate + w.w_measure * t.measure +
                w.w_comm * t.comm;
        }

        // ─── 2. 代际法则 τ_{n+1} = τ_n / α ───────────────────────────────────────

        /** 领域类别（论文第 2 节的应用特定年度缩放因子） */
        enum class AlphaDomain : std::int32_t
        {
            Mobile     = 0, // 移动设备：功耗/热预算受限，α ≈ 1.3
            Autonomous = 1, // 自动驾驶：安全关键实时响应，α ≈ 1.5
            Ai         = 2, // AI 载荷：吞吐直接换算经济价值，α 可达 10
        };

        /**
         * 取领域年度缩放因子 α（论文 2.1~10 的离散取值）。
         * 未知领域返回 1.0（不缩放），不做非法值扩展。
         */
        inline double alpha_of(AlphaDomain d)
        {
            switch (d)
            {
            case AlphaDomain::Mobile:     return 1.3;
            case AlphaDomain::Autonomous: return 1.5;
            case AlphaDomain::Ai:         return 10.0;
            default:                      return 1.0;
            }
        }

        /** 整数预设下标 → α：0=移动 1.3，1=自动驾驶 1.5，2=AI 10.0，其余 1.0 */
        inline double alpha_preset(std::int32_t preset)
        {
            return alpha_of(static_cast<AlphaDomain>(preset < 0 ? -1 : preset > 2 ? -1 : preset));
        }

        /** 一代缩放：τ_{n+1} = τ_n / α（α ≤ 0 时返回 τ，不引入无穷） */
        inline double generational(double tau, double alpha)
        {
            if (!(alpha > 0.0)) return tau;
            return tau / alpha;
        }

        /** 预算退火一步：B ← B/α（与 generational 同一公式，语义是时间预算收缩） */
        inline double anneal(double budget, double alpha)
        {
            return generational(budget, alpha);
        }

        // ─── 3. 关键路径与折叠（LogicFolding 量子化）─────────────────────────────

        /**
         * DAG 关键路径：最长依赖链的代价和（论文：时钟必须等最慢路径走完，f ≤ 1/τ）。
         * cost[i]  = 节点自身时间增量；边 (u, v) 表示 u 必须完成后 v 才能开始。
         * 实现：拓扑序 DP，dp[v] = cost[v] + max(dp[u] for u in pred(v))。
         * 返回最大 dp 值；空图返回 0。
         */
        inline double critical_path(const std::vector<double> &cost,
                                    const std::vector<std::pair<std::int32_t, std::int32_t>> &edges)
        {
            const std::int32_t n = static_cast<std::int32_t>(cost.size());
            if (n <= 0) return 0.0;

            std::vector<std::int32_t> indeg(n, 0);
            std::vector<std::vector<std::int32_t>> succ(n);
            for (const auto &e : edges)
            {
                const std::int32_t u = e.first, v = e.second;
                if (u < 0 || u >= n || v < 0 || v >= n || u == v) continue; // 非法边跳过
                succ[u].push_back(v);
                indeg[v]++;
            }

            std::vector<double> dp(n, 0.0);
            // Kahn 拓扑序：出队顺序即拓扑序，保证 dp[u] 先于 dp[v] 定值。
            // 出边传播：dp[v] = cost[v] + max(dp[u] for u in pred(v))，
            // 等价表述为"先 max 收集前驱值、出队时加上自身代价"。
            std::vector<std::int32_t> queue;
            queue.reserve(static_cast<size_t>(n));
            for (std::int32_t i = 0; i < n; i++) if (indeg[i] == 0) queue.push_back(i);
            for (size_t head = 0; head < queue.size(); head++)
            {
                const std::int32_t u = queue[head];
                dp[u] += cost[u];
                for (const std::int32_t v : succ[u])
                {
                    dp[v] = std::max(dp[v], dp[u]);
                    if (--indeg[v] == 0) queue.push_back(v);
                }
            }

            double result = 0.0;
            for (std::int32_t i = 0; i < n; i++) result = std::max(result, dp[i]);
            return result;
        }

        /**
         * 折叠深度（LogicFolding 的量子化）：把串行深度 L 的链接图折进 T 个并行的
         * 垂直层（tier），每层承担 ⌈L/T⌉ 段，折叠后关键路径 ≈ ⌈L/T⌉。
         * T 是层数（垂直集成）；受限于跨层边密度（见 gear_ratio 与 route_cost）。
         * T ≤ 0 视为未折叠（返回 L）；T > L 时至少 1 层，返回 1。
         */
        inline double fold_depth(double L, double T)
        {
            if (!(T > 0.0)) return L;
            if (T >= L) return 1.0;
            return std::ceil(L / T);
        }

        /**
         * 齿轮比（论文 gear ratio：键合间距/顶层金属间距，经验阈值 3）的量子化：
         *     g = E_cross / E_intra
         * E_cross = 跨 tier 边数（垂直通道），E_intra = tier 内边数（平面布线）。
         * 论文结论：g → 1 时跨层路由开销消失（垂直通道密度追上平面布线）；g > 3
         * 时跨层通信主导，折叠不再划算。
         * 守卫：intra ≤ 0 且 cross ≤ 0 → 0（无通信）；intra ≤ 0 且 cross > 0 → 返回
         * 一个大数（2^53 量级），让调用方在比较时正确看出"全是跨层通信"。
         */
        inline double gear_ratio(double e_cross, double e_intra)
        {
            if (e_intra > 0.0) return e_cross / e_intra;
            if (e_cross > 0.0) return 9.007199254740992e15; // 2^53，跨层通信独占
            return 0.0;
        }

        /**
         * 论文"f ≤ 1/τ_critical"的对偶：折叠后的一次任务总调度代价。
         *     τ_route = τ_gate · D + τ_comm · D · g
         * 每一项的语义：
         *   τ_gate · D    关键路径（D 个串行层）乘单层门时间；
         *   τ_comm · D · g 每跨一个层都要付一次跨 tier 通信，通信量正比于齿轮比 g
         *                 （g = 边缘通道数相对平面布线密度的比值）。
         * 层内通信已包含在 τ_gate 里（与论文把互连 RC 计入电路层一致）。
         */
        inline double route_cost(double depth, double gear, double t_gate, double t_comm)
        {
            const double d = std::max(0.0, depth);
            const double g = std::max(0.0, gear);
            return d * t_gate + d * g * t_comm;
        }

        // ─── 4. 时间预算路由（AI 的 α→10：吞吐即价值的调度对偶）─────────────────

        /**
         * λ 影子价格（时间的价格）：把"时间"当货币时的单件净价值。
         *     score = value − λ · cost
         * score > 0 → 值得在当前时间价格下调度；λ 越大时间越贵，越少组件入选。
         * 注：与 numqk::moe 的 curvature_gate 一样是标量门控，模型层直接用
         * if (tau_route_price(v, c, λ) > 0) 做路由判决。
         */
        inline double route_price(double value, double cost, double lambda)
        {
            return value - lambda * cost;
        }

        /**
         * 时间预算路由（C++ 内核版，rvdl 层用标量 route_price 自行组合）：
         * 在 Σ τ_i ≤ B 的前提下选价值最大的子集（0/1 背包的拉格朗日对偶）。
         * 对于二分 λ（时间的影子价格），λ 越大入选越少；取仍满足预算的 λ 作为
         * 求解结果，入选集合 = { i : v_i − λ·τ_i > 0 }（严格正贡献才入选）。
         * 输出（返回 std::tuple<double λ, double value, double cost>，入选集合写
         * chosen）。当预算装得下全部时 λ=0 全选——与"AI 预算宽 → 全激活"一致。
         */
        inline std::tuple<double, double, double> budget_route(const std::vector<double> &values,
                                                            const std::vector<double> &costs,
                                                            double budget,
                                                            std::vector<std::int32_t> &chosen)
        {
            chosen.clear();
            const size_t n = values.size();
            if (n == 0 || costs.size() != n || budget < 0.0) return {0.0, 0.0, 0.0};

            double total_cost = 0.0;
            double max_ratio  = 0.0;
            for (size_t i = 0; i < n; i++)
            {
                total_cost += costs[i];
                if (costs[i] > 0.0) max_ratio = std::max(max_ratio, values[i] / costs[i]);
            }

            // 全选都装得下 → λ=0，全选（预算不是约束）
            if (total_cost <= budget)
            {
                for (size_t i = 0; i < n; i++) chosen.push_back(static_cast<std::int32_t>(i));
                double vsum = 0.0;
                for (double x : values) vsum += x;
                return {0.0, vsum, total_cost};
            }

            // 二分 λ ∈ [0, max_ratio + 1]。cost(λ) 是 λ 的非增阶梯函数：
            // 找满足 cost(λ) ≤ budget 的最小 λ（最贵的可行价格）。
            double lo = 0.0, hi = max_ratio + 1.0;
            std::vector<bool> pick(n, false);
            for (int iter = 0; iter < 80; iter++)
            {
                const double mid = 0.5 * (lo + hi);
                double c = 0.0;
                for (size_t i = 0; i < n; i++)
                {
                    pick[i] = values[i] - mid * costs[i] > 0.0;
                    if (pick[i]) c += costs[i];
                }
                if (c <= budget)
                    hi = mid; // 可行 → 试更贵的时间价格
                else
                    lo = mid; // 超支 → 时间要更贵
            }

            const double lambda = hi;
            double vsum = 0.0, csum = 0.0;
            for (size_t i = 0; i < n; i++)
            {
                pick[i] = values[i] - lambda * costs[i] > 0.0;
                if (pick[i])
                {
                    chosen.push_back(static_cast<std::int32_t>(i));
                    vsum += values[i];
                    csum += costs[i];
                }
            }
            // 数值安全：最后一步重算若仍超支（阶梯跳变），退 λ 到 lo 重取
            if (csum > budget && lo < hi)
            {
                chosen.clear();
                vsum = 0.0; csum = 0.0;
                for (size_t i = 0; i < n; i++)
                {
                    pick[i] = values[i] - lo * costs[i] > 0.0;
                    if (pick[i])
                    {
                        chosen.push_back(static_cast<std::int32_t>(i));
                        vsum += values[i];
                        csum += costs[i];
                    }
                }
            }
            return {csum <= budget ? lambda : lo, vsum, csum};
        }
    }
}