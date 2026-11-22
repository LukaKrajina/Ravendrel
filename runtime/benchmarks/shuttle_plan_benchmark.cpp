#include <cstdio>
#include <cmath>
#include <limits>
#include <set>
#include <string>
#include <vector>
#include <algorithm>
#include <type_traits>

#include "utils/Ta.hpp"

using namespace ta;

namespace
{
    int g_pass = 0, g_fail = 0;
    void check(bool ok, const char* what)
    {
        std::printf("  %s %s\n", ok ? "[PASS]" : "[FAIL]", what);
        ok ? ++g_pass : ++g_fail;
    }

    ShuttleLink make_link(size_t n, ShuttleStrategy s = ShuttleStrategy::Auto)
    {
        ShuttleLink link;
        link.from     = "src";
        link.lambda   = 1.0;
        link.phase    = ShuttlePhase::Both;
        link.rounds   = 0;
        link.strategy = s;
        for (size_t i = 0; i < n; ++i) {
            ShuttlePeer p;
            p.name     = "peer" + std::to_string(i + 1);
            p.coord    = { static_cast<int>(i + 1) };
            p.distance = static_cast<int>(i + 1);
            p.logWeight = -static_cast<double>(p.distance);
            p.weight    = std::exp(p.logWeight);
            p.via       = "parallel";
            link.peers.push_back(std::move(p));
        }
        return link;
    }

    int edge_count(const ShuttlePlan& p)
    {
        int total = 0;
        for (const auto& r : p.rounds) total += static_cast<int>(r.edges.size());
        return total;
    }

    void bench_strategies()
    {
        std::printf("\n[1] 五策略代价表（λ=1，对端 coord=(1..N)）\n");
        std::printf("  %6s | %-10s %-10s | %-10s %-10s | %-10s %-10s | %-10s %-10s | %-10s %-10s\n",
                    "N", "蝶式深度", "蝶式边数", "环式深度", "环式边数",
                    "树式深度", "树式边数", "Grover深度", "Grover边数", "c001深度", "c001边数");

        const int ancilla = 32;
        bool butterfly_ok = true, ring_ok = true, tree_ok = true, grover_ok = true;
        bool c001_ok = true;
        bool no_peer_lost = true;

        for (size_t n : {size_t(4), size_t(8), size_t(16), size_t(32),
                         size_t(64), size_t(128), size_t(256), size_t(512), size_t(1024)})
        {
            const long long span = static_cast<long long>(n) + 1;
            auto bf = ShuttlePlanner::plan(make_link(n, ShuttleStrategy::Butterfly),
                                           PayloadKind::Classical, false, ancilla, span);
            auto rg = ShuttlePlanner::plan(make_link(n, ShuttleStrategy::Ring),
                                           PayloadKind::Classical, false, ancilla, span);
            auto tr = ShuttlePlanner::plan(make_link(n, ShuttleStrategy::Tree),
                                           PayloadKind::Classical, false, ancilla, span);
            auto gv = ShuttlePlanner::plan(make_link(n, ShuttleStrategy::Grover),
                                           PayloadKind::Classical, false, ancilla, span);
            auto cp = ShuttlePlanner::plan(make_link(n, ShuttleStrategy::C001Point),
                                           PayloadKind::Classical, false, ancilla, span);

            std::printf("  %6zu | %-10d %-10d | %-10d %-10d | %-10d %-10d | %-10d %-10d | %-10d %-10d\n",
                        n, bf.depth, edge_count(bf), rg.depth, edge_count(rg),
                        tr.depth, edge_count(tr), gv.depth, edge_count(gv),
                        cp.depth, edge_count(cp));

            const int want_bf = static_cast<int>(std::ceil(std::log2(static_cast<double>(span))));
            const int want_rg = static_cast<int>(n);
            const int want_tr = static_cast<int>(std::ceil(std::log2(static_cast<double>(n) + 1.0)));
            butterfly_ok &= (bf.depth == want_bf);
            ring_ok      &= (rg.depth == want_rg);
            tree_ok      &= (tr.depth == want_tr);
            grover_ok    &= (gv.depth == 1);
            c001_ok      &= (cp.depth == 1);

            for (const auto& pl : {bf, rg, tr, gv, cp}) {
                std::set<std::string> reached;
                for (const auto& r : pl.rounds)
                    for (const auto& e : r.edges) reached.insert(e.peer);
                if (reached.size() != n) no_peer_lost = false;
            }

            if (bf.degraded || rg.degraded || tr.degraded || gv.degraded || cp.degraded)
                std::printf("       [WARN] N=%zu 出现降级（ancilla=%d 应充足）\n", n, ancilla);
        }

        check(butterfly_ok, "蝶式深度 == ⌈log₂ span⌉（O(log span)）");
        check(ring_ok,      "环式深度 == N（O(N)）");
        check(tree_ok,      "树式深度 == ⌈log₂(N+1)⌉（O(log N)）");
        check(grover_ok,    "Grover 深度 == 1（O(1)，经典基前向传播无法达到）");
        check(c001_ok,      "c001 蝶点深度恒 == 1（O(1)，与 span/对端数无关，零附加资源）");
        check(no_peer_lost, "每种策略都触及每个对端（无人因代价被丢弃）");

        std::printf("  · 总门数：环式 == N、树式 == N（O(N)），蝶式 == N·⌈log₂span⌉（O(N log N)）。\n"
                    "    三者**每跳带宽相同** —— 故大消息 / 层级集群下环式的 O(N) 门数优势\n"
                    "    会压过它的 O(N) 深度劣势。这正是 shuttle_strategy 必须是可调旋钮的理由。\n"
                    "    c001 蝶点与 Grover 同为 O(1) 深度，但资源模型相反：Grover 需 2ⁿ 工作\n"
                    "    寄存器（不足则 W-TRK001 降级），c001 零附加资源（docs §3.7）。\n");
    }

    void bench_auto()
    {
        std::printf("\n[2] auto 启发式（docs §3.5）\n");

        auto l1 = make_link(1);
        auto p1 = ShuttlePlanner::plan(l1, PayloadKind::Classical, false, 32, 2);
        check(p1.strategy == ShuttleStrategy::Butterfly, "n ≤ 2 → 蝶式（对数深度无额外收益）");

        auto p2 = ShuttlePlanner::plan(make_link(8), PayloadKind::Classical, true, 32, 9);
        check(p2.strategy == ShuttleStrategy::Ring, "载荷大 → 环式（带宽项与蝶式相同，互连更省）");

        const long long span = 64;
        auto p3 = ShuttlePlanner::plan(make_link(16), PayloadKind::Quantum, false, 7, span);
        check(p3.strategy == ShuttleStrategy::Grover, "Quantum + ancilla 充足 → Grover（O(1) 深度）");

        auto p4 = ShuttlePlanner::plan(make_link(16), PayloadKind::Quantum, false, 5, span);
        check(p4.strategy != ShuttleStrategy::Grover, "ancilla 不足 → 不选 Grover");

        auto p5 = ShuttlePlanner::plan(make_link(16), PayloadKind::Quantum, false, -1, span);
        check(p5.strategy != ShuttleStrategy::Grover, "ancilla 未知 → 不选 Grover（不假装知道）");

        auto p6 = ShuttlePlanner::plan(make_link(16, ShuttleStrategy::Ring),
                                       PayloadKind::Quantum, false, 32, span);
        check(p6.strategy == ShuttleStrategy::Ring, "显式策略优先于启发式（显式即显式）");

        auto p7 = ShuttlePlanner::plan(make_link(16, ShuttleStrategy::Grover),
                                       PayloadKind::Quantum, false, 2, span);
        check(p7.strategy == ShuttleStrategy::Grover && !p7.degraded,
              "规划器不擅自降级（显式策略原样展开，降级由调度器负责并记录）");
        check(!ShuttlePlanner::grover_affordable(span, 2) &&
               ShuttlePlanner::grover_affordable(span, 7),
              "可供性判定本身是纯函数：span=64 需 ⌈log₂64⌉+1 = 7 位");

        auto p8 = ShuttlePlanner::plan(make_link(8), PayloadKind::Classical, false, 32, 9);
        check(p8.strategy != ShuttleStrategy::Auto,
              "auto 必须解析为具体策略（决策写入诊断，不静默）");

        auto p9 = ShuttlePlanner::plan(make_link(16), PayloadKind::Classical, false, 32, 64);
        check(p9.strategy == ShuttleStrategy::C001Point,
              "span ≥ 16 且 n ≥ 4 → c001 蝶点（远端对端不再随 ⌈log₂span⌉ 加深）");
        auto p10 = ShuttlePlanner::plan(make_link(4), PayloadKind::Classical, false, 32, 16);
        check(p10.strategy == ShuttleStrategy::C001Point,
              "span = 16、n = 4 边界 → c001 蝶点（含边界值）");
        auto p11 = ShuttlePlanner::plan(make_link(3), PayloadKind::Classical, false, 32, 64);
        check(p11.strategy == ShuttleStrategy::Butterfly,
              "n = 3（< 4）→ 蝶式（小规模蝶点无收益）");
        auto p12 = ShuttlePlanner::plan(make_link(16), PayloadKind::Classical, false, 32, 8);
        check(p12.strategy == ShuttleStrategy::Butterfly,
              "span = 8（< 16）→ 蝶式（格点跨度小，蝶式已足够浅）");
        auto p13 = ShuttlePlanner::plan(make_link(16), PayloadKind::Classical, true, 32, 64);
        check(p13.strategy == ShuttleStrategy::Ring,
              "载荷大 → 环式优先于 c001（带宽项仍是大消息的主矛盾）");
    }

    void bench_no_truncation()
    {
        std::printf("\n[3] 无截断阈值：logWeight 恒有限，对端永不丢失（docs §2.3）\n");
        const double lam = 1.0;
        bool all_finite = true, underflows_seen = false, all_scheduled = true;

        for (int d : {1, 10, 100, 500, 700, 746, 750, 1000, 5000, 1000000})
        {
            ShuttlePeer p;
            p.name      = "p";
            p.coord     = {d};
            p.distance  = d;
            p.logWeight = -static_cast<double>(d) / lam;
            p.weight    = std::exp(p.logWeight);
            p.via       = "parallel";

            const bool finite_log   = std::isfinite(p.logWeight);
            const bool underflow    = (p.weight == 0.0);
            if (!finite_log) all_finite = false;
            if (underflow) underflows_seen = true;

            std::printf("  d=%-8d logWeight=%-14.6f weight=%-12.6g%s\n",
                        d, p.logWeight, p.weight,
                        underflow ? "  ← 线性权重下溢（数值零，非语义截断）" : "");

            ShuttleLink link;
            link.from = "src"; link.lambda = lam; link.strategy = ShuttleStrategy::Butterfly;
            link.peers.push_back(p);
            auto plan = ShuttlePlanner::plan(link, PayloadKind::Classical, false, 32, 2);
            if (edge_count(plan) == 0) all_scheduled = false;
        }

        check(all_finite,      "logWeight = −d/λ 对任意有限 d 均有限（可路由/排序/比较）");
        check(underflows_seen, "确实存在线性权重下溢为 0 的距离（否则本节没有测到边界）");
        check(all_scheduled,   "下溢对端**仍被调度**（数值零 ≠ 语义截断）");

        auto crosses_to_zero = [](double x) { return std::exp(-x) == 0.0; };
        double lo = 1.0, hi = 2000.0;
        for (int it = 0; it < 200; ++it)
        {
            const double mid = 0.5 * (lo + hi);
            if (mid <= lo || mid >= hi) break;
            if (crosses_to_zero(mid)) hi = mid; else lo = mid;
        }
        std::printf("  · exp(−x) 的**整零**阈值二分到单个可表示步长：x ∈ (%.9g, %.9g]\n"
                    "    （文档记作 «≈746»；lo 处仍非零、hi 处已整零）\n", lo, hi);
        const double ln_max    = std::log(std::numeric_limits<double>::max());
        const double ln_tiny   = std::log(std::numeric_limits<double>::denorm_min());
        std::printf("    对照：ln(DBL_MAX) = %.3f 是**正规数**下界，此处权重仍非零（%.3g）——\n"
                    "    与整零阈值相差 %.1f，混用会让「下溢」的断言落在错误的 x 上。\n",
                    ln_max, std::exp(-ln_max), -ln_tiny - ln_max);
        check(crosses_to_zero(hi) && !crosses_to_zero(lo),
              "整零阈值被二分夹逼找到（不是把常数硬抄进断言）");
        check(std::fabs(hi - 746.0) < 2.0 && std::fabs(lo - 746.0) < 2.0,
              "整零阈值 ≈ 746，与文档记载一致（且端点本身非零）");
        check(std::exp(-std::log(std::numeric_limits<double>::max())) > 0.0,
              "ln(DBL_MAX) ≈ 709.8 处权重**尚未**整零（故不能拿它当截断阈值）");

        std::vector<std::pair<double, int>> byLog;
        for (int d : {800, 900, 1000})
            byLog.push_back({ -static_cast<double>(d) / lam, d });
        std::sort(byLog.begin(), byLog.end(),
                  [](const std::pair<double, int>& a, const std::pair<double, int>& b) {
                      return a.first > b.first;
                  });

        bool strictly_ordered = true;
        for (size_t i = 1; i < byLog.size(); ++i)
            if (!(byLog[i - 1].first > byLog[i].first)) strictly_ordered = false;

        check(strictly_ordered,
              "全下溢区间内 logWeight 两两**严格**可分辨（weight 已全部为 0）");
        check(byLog[0].second == 800 && byLog[1].second == 900 && byLog[2].second == 1000,
              "按 logWeight 排序精确还原距离序（近 → 远）");

        bool weight_all_tied = true;
        for (int d : {800, 900, 1000})
            if (std::exp(-static_cast<double>(d) / lam) != 0.0) weight_all_tied = false;
        check(weight_all_tied,
              "同一区间内 weight 全为 0 —— 拿它当排序键会得到任意顺序（故路由禁用 weight）");
    }

    void bench_tracking()
    {
        std::printf("\n[4] 三级保真阶梯与强制模式（docs §4.2）\n");
        std::printf("  %-12s %-8s %-8s %-8s %-8s\n", "观测量",
                    "auto级", "auto采样", "强制3级", "强制2级");

        struct Row { const char* obs; TrackLevel want_auto; TrackLevel want_m3; TrackLevel want_m2; };
        const Row rows[] = {
            {"expect_z",   TrackLevel::Qnd,     TrackLevel::Relabel, TrackLevel::Shadow},
            {"expect_x",   TrackLevel::Qnd,     TrackLevel::Relabel, TrackLevel::Shadow},
            {"num_qubits", TrackLevel::Relabel, TrackLevel::Relabel, TrackLevel::Shadow},
            {"shannon4",   TrackLevel::Relabel, TrackLevel::Relabel, TrackLevel::Shadow},
            {"entropy",    TrackLevel::Shadow,  TrackLevel::Relabel, TrackLevel::Shadow},
            {"fidelity",   TrackLevel::Shadow,  TrackLevel::Relabel, TrackLevel::Shadow},
            {"attention",  TrackLevel::Shadow,  TrackLevel::Relabel, TrackLevel::Shadow},
            {"measure_z",  TrackLevel::Shadow,  TrackLevel::Relabel, TrackLevel::Shadow},
        };

        bool auto_ok = true, force3_ok = true, force2_ok = true, samples_ok = true;
        for (const auto& r : rows)
        {
            auto a = TrackingPlanner::plan("b", {r.obs}, TrackMode::Auto,    1, 0, 1)[0];
            auto m3 = TrackingPlanner::plan("b", {r.obs}, TrackMode::Relabel, 1, 0, 1)[0];
            auto m2 = TrackingPlanner::plan("b", {r.obs}, TrackMode::Shadow,  1, 0, 1)[0];

            std::printf("  %-12s L%-7d %-8d L%-7d L%-7d\n", r.obs,
                        static_cast<int>(a.plans[0].level), a.plans[0].samples,
                        static_cast<int>(m3.plans[0].level),
                        static_cast<int>(m2.plans[0].level));

            auto_ok   &= (a.plans[0].level == r.want_auto);
            force3_ok &= (m3.plans[0].level == r.want_m3);
            force2_ok &= (m2.plans[0].level == r.want_m2);

            const int s = a.plans[0].samples;
            if (a.plans[0].level == TrackLevel::Qnd)     samples_ok &= (s == 1);
            if (a.plans[0].level == TrackLevel::Relabel) samples_ok &= (s == 0);
            if (a.plans[0].level == TrackLevel::Shadow)  samples_ok &= (s > 1);
        }

        check(auto_ok,   "auto 选级与族归属表一致（Pauli→L0 / 经典→L2 / 其余→L1）");
        check(force3_ok, "track_mode=3 把**所有**观测量强制为 L2（强制即强制）");
        check(force2_ok, "track_mode=2 把**所有**观测量强制为 L1（含本是 Pauli 族的）");
        check(samples_ok,"采样代价随级正确：L0=1 / L1=3^k 保守下界 / L2=0");

        check(TrackingPlanner::fidelity_contract(TrackLevel::Qnd, 1.0) &&
              !TrackingPlanner::fidelity_contract(TrackLevel::Qnd, 0.99),
              "L0 保真契约要求 = 1（非破坏读出是精确的，不是近似的）");
        check(TrackingPlanner::fidelity_contract(TrackLevel::Relabel, 1.0),
              "L2 保真契约要求 = 1（态从不被触碰）");
        check(TrackingPlanner::fidelity_contract(TrackLevel::Shadow, 0.0) &&
              TrackingPlanner::fidelity_contract(TrackLevel::Shadow, 1.0),
              "L1 保真契约是统计级（不承诺单次精确 —— 单次软克隆物理上不可能）");

        const double b1  = TrackingPlanner::shadow_fidelity_bound(1);
        const double b21 = TrackingPlanner::shadow_fidelity_bound(21);
        const double b1k = TrackingPlanner::shadow_fidelity_bound(1000);
        std::printf("  · L1 采样收敛下界 1 − 1/√N：N=1 → %.4f，N=21 → %.4f，N=1000 → %.4f\n",
                    b1, b21, b1k);
        check(b1 < b21 && b21 < b1k && b1k < 1.0,
              "L1 下界随采样单调上升且恒 < 1（统计级，不伪装成精确）");

        auto sc = TrackingPlanner::plan("blk", {"expect_z", "entropy"}, TrackMode::Auto, 3, 7, 10)[0];
        check(sc.window_begin == 7 && sc.window_end == 10,
              "作用域绑定功能周期 [time, time+cost)（窗口退出即释放）");
        check(sc.slots == 3, "track_slots 原样落到作用域（槽数由用户约束）");
        check(sc.plans.size() == 2 && sc.level.size() == 2,
              "多个观测量各自成计划（不同族可共存于同一作用域）");

        static_assert(std::is_standard_layout<ShadowSlot>::value,
                      "影子槽必须是标准布局");
        static_assert(!std::is_polymorphic<ShadowSlot>::value,
                      "影子槽不得多态 —— 多态是「它其实是个对象」的第一步");
        static_assert(std::is_same<decltype(ShadowSlot::values)::value_type, double>::value,
                      "影子槽只承载经典标量（double）—— 换成 QObject 即刻违反 E-Q001/E-Q002");
        static_assert(std::is_trivially_copyable<double>::value &&
                      std::is_trivially_copyable<bool>::value,
                      "影子槽的标量载体必须平凡可复制（可与 QObject 的线性性区分开）");
        std::printf("  · ShadowSlot = { std::vector<double>, bool }："
                    "元素是经典标量、类型标准布局且非多态 —— "
                    "结构上不可能是 QObject（QLT E-Q001/E-Q002 由此保住）\n");
    }
}

int main()
{
    std::printf("=== 穿梭与追踪规划器验证基准 ===\n");
    bench_strategies();
    bench_auto();
    bench_no_truncation();
    bench_tracking();
    std::printf("\n=== 结果：%d 通过 / %d 失败 ===\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}