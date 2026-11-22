#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "utils/Ta.hpp"

namespace
{
    struct LayerInfo
    {
        std::string name;
        int time = 0;
        int thread = 0;
        std::vector<int> coord;
        int cost = 1;
        bool has_deadline = false;
        int deadline = 0;
        bool dual = false;
        bool has_shuttle = false;
        std::vector<std::vector<int>> shuttle_targets;
        double shuttle_lambda = 1.0;
        int shuttle_phase = 2;
        int shuttle_rounds = 0;
        int shuttle_strategy = -1;
        bool has_track = false;
        std::vector<std::string> track_observables;
        int track_mode = 0;
        int track_slots = 1;
    };

    std::string coordKey(const std::vector<int> &c)
    {
        std::string k;
        for (size_t i = 0; i < c.size(); ++i)
        {
            if (i) k += ',';
            k += std::to_string(c[i]);
        }
        return k;
    }

    std::string pairKey(const std::string &a, const std::string &b)
    {
        return a < b ? a + '\x1f' + b : b + '\x1f' + a;
    }

    int l1Distance(const std::vector<int> &a, const std::vector<int> &b)
    {
        size_t n = std::max(a.size(), b.size());
        int d = 0;
        for (size_t i = 0; i < n; ++i)
            d += std::abs((i < a.size() ? a[i] : 0) - (i < b.size() ? b[i] : 0));
        return d;
    }

    std::map<std::string, std::string> buildAdjacency(const std::vector<LayerInfo> &layers)
    {
        std::map<std::string, std::string> adj;
        for (size_t i = 0; i < layers.size(); ++i)
        {
            for (size_t j = i + 1; j < layers.size(); ++j)
            {
                const auto &A = layers[i];
                const auto &B = layers[j];
                std::string kind;
                if (coordKey(A.coord) != coordKey(B.coord))
                    kind = (A.coord.size() != B.coord.size()) ? "projection" : "parallel";
                else
                {
                    if (A.dual == B.dual && A.thread == B.thread) kind = "stack";
                    if (A.dual != B.dual) kind = "inversion";
                    else if (A.thread != B.thread) kind = "crossing";
                }
                if (!kind.empty()) adj.emplace(pairKey(A.name, B.name), kind);
            }
        }
        return adj;
    }

    int computeAutoRounds(const std::vector<int> &coordMax, const std::vector<int> &coordMin)
    {
        long long span = 1;
        for (size_t i = 0; i < coordMax.size(); ++i)
        {
            const int lo = std::min(0, i < coordMin.size() ? coordMin[i] : 0);
            span *= (long long)(std::max(1, coordMax[i] - lo + 1));
        }
        return span <= 1 ? 0 : (int)std::ceil(std::log2((double)span));
    }

    struct Peer { std::string name; double logWeight; double weight; int distance; std::string via; };

    std::vector<Peer> resolvePeers(const LayerInfo &src, const std::vector<LayerInfo> &layers,
                                   const std::map<std::string, std::string> &adj)
    {
        std::vector<Peer> peers;
        std::set<std::string> seen;
        for (const auto &target : src.shuttle_targets)
        {
            for (const auto &m : layers)
            {
                if (m.name == src.name) continue;
                if (coordKey(m.coord) != coordKey(target)) continue;
                if (seen.count(m.name)) continue;
                auto ait = adj.find(pairKey(src.name, m.name));
                if (ait == adj.end()) continue;
                if (coordKey(src.coord) == coordKey(m.coord) && src.time == m.time) continue;
                seen.insert(m.name);
                int d = l1Distance(src.coord, m.coord);
                double logW = src.shuttle_lambda > 0.0 ? -(double)d / src.shuttle_lambda : 0.0;
                double w = src.shuttle_lambda > 0.0 ? std::exp(logW) : 0.0;
                peers.push_back({m.name, logW, w, d, ait->second});
            }
        }
        std::sort(peers.begin(), peers.end(),
                  [](const Peer &x, const Peer &y) { return x.name < y.name; });
        return peers;
    }

    int observableLevel(const std::string &o)
    {
        if (o == "expect_z" || o == "expect_x" || o == "expect_y") return 0;
        if (o == "num_qubits" || o == "shannon4" || o == "shannon8" || o == "amplitude") return 2;
        return 1;
    }

    int g_fail = 0;
    void check(bool cond, const char *what)
    {
        if (!cond)
        {
            std::printf("  FAIL: %s\n", what);
            ++g_fail;
        }
        else
        {
            std::printf("  ok:   %s\n", what);
        }
    }
}

int main()
{
    std::vector<LayerInfo> layers;
    {
        LayerInfo a;
        a.name = "relay"; a.time = 0; a.thread = 0; a.coord = {0, 0};
        a.has_shuttle = true; a.shuttle_targets = {{1, 0}}; a.shuttle_lambda = 1.0;
        layers.push_back(a);
    }
    {
        LayerInfo b;
        b.name = "target"; b.time = 1; b.thread = 0; b.coord = {1, 0};
        layers.push_back(b);
    }

    std::printf("[1] 邻接与权重核\n");
    auto adj = buildAdjacency(layers);
    check(adj.count(pairKey("relay", "target")) == 1, "relay↔target 有邻接边");
    check(adj.at(pairKey("relay", "target")) == "parallel", "边种类 = parallel");

    auto peers = resolvePeers(layers[0], layers, adj);
    check(peers.size() == 1, "解析出 1 个对端");
    check(peers[0].name == "target", "对端 = target");
    check(peers[0].distance == 1, "L1 距离 = 1");
    check(std::fabs(peers[0].weight - std::exp(-1.0)) < 1e-15, "权重 = exp(-1) ≈ 0.3679");

    std::printf("[2] 轮次规划\n");
    check(computeAutoRounds({1, 0}, {0, 0}) == 1, "span=2 → ceil(log2 2) = 1 轮");
    check(computeAutoRounds({0, 0}, {0, 0}) == 0, "span=1 → 0 轮");
    check(computeAutoRounds({3, 0}, {0, 0}) == 2, "span=4 → ceil(log2 4) = 2 轮");
    check(computeAutoRounds({3, 3}, {0, 0}) == 4, "span=16 → ceil(log2 16) = 4 轮（蝶式深度 O(log N)）");
    check(computeAutoRounds({-1}, {-2}) == 1, "全负坐标 [-2,-1] 跨度 2 → 1 轮（不静默失效）");
    check(computeAutoRounds({-1}, {-2}) != computeAutoRounds({-1}, {0}),
          "负地址与原点起算的跨度确实不同（该差异是被显式处理的，不是巧合）");

    {
        const auto strategyDepth = [](int strategy, size_t peerCount, long long span) -> int {
            switch (strategy) {
            case 1: return static_cast<int>(peerCount);
            case 2: return peerCount == 0 ? 0
                         : static_cast<int>(std::ceil(std::log2(static_cast<double>(peerCount) + 1.0)));
            case 3: return peerCount == 0 ? 0 : 1;
            case 4: return peerCount == 0 ? 0 : 1;
            default: return peerCount == 0 ? 0 : (span <= 1 ? 1
                         : static_cast<int>(std::ceil(std::log2(static_cast<double>(span)))));
            }
        };

        check(strategyDepth(0, 8, 64) == 6, "蝶式 n=8 span=64 → 6 轮（O(log span)）");
        check(strategyDepth(1, 8, 64) == 8, "环式 n=8 → 8 轮（O(N)，**不是** ⌈log₂span⌉）");
        check(strategyDepth(2, 8, 64) == 4, "树式 n=8 → ⌈log₂9⌉ = 4 轮（2^d−1 ≥ n）");
        check(strategyDepth(3, 8, 64) == 1, "Grover → 1 轮（O(1)）");
        check(strategyDepth(4, 8, 64) == 1, "c001 蝶点 n=8 span=64 → 恒 1 轮（压缩成点再辐射）");
        check(strategyDepth(4, 1, 1024) == 1, "c001 蝶点 span=1024 → 仍 1 轮（与 span 无关）");

        check(strategyDepth(1, 8, 64) != strategyDepth(0, 8, 64),
              "环式与蝶式的轮次不同（否则 shuttle_strategy 对调度无影响）");

        check(strategyDepth(1, 8, 64) >= strategyDepth(0, 8, 64) &&
              strategyDepth(0, 8, 64) >= strategyDepth(3, 8, 64),
              "此点上深度 环式 ≥ 蝶式 ≥ Grover（非普适排序，仅本例）");

        check(strategyDepth(0, 1, 1) == 1, "span=1 且有对端 → 至少 1 轮（0 轮 = 静默失效）");
        check(strategyDepth(1, 1, 1) == 1, "环式 n=1 → 1 跳");
        check(strategyDepth(2, 1, 1) == 1, "树式 n=1 → 1 层");
        check(strategyDepth(0, 0, 0) == 0, "无对端 → 0 轮（无可渗透对象）");
    }

    std::printf("[2b] 对账：解析深度 ↔ 真实展开（Ta.hpp）\n");
    {
        bool analytic_match = true, expansion_match = true, nonzero_ok = true;
        for (long long span : {1LL, 2LL, 4LL, 16LL, 64LL, 256LL, 1024LL}) {
            for (size_t n : {size_t(1), size_t(2), size_t(3), size_t(8), size_t(16), size_t(64)}) {
                ta::ShuttleLink link;
                link.from = "src"; link.lambda = 1.0; link.rounds = 0;
                for (size_t i = 0; i < n; ++i) {
                    ta::ShuttlePeer p;
                    p.name = "p" + std::to_string(i);
                    p.coord = {static_cast<int>(i + 1)};
                    p.distance = static_cast<int>(i + 1);
                    p.logWeight = -static_cast<double>(i + 1);
                    p.weight = std::exp(p.logWeight);
                    link.peers.push_back(std::move(p));
                }
                for (int s : {0, 1, 2, 3, 4}) {
                    ta::ShuttleStrategy st = static_cast<ta::ShuttleStrategy>(s);
                    const int analytic = ta::ShuttlePlanner::depth_of(st, n, span);
                    ta::ShuttleLink l2 = link;
                    l2.strategy = st;
                    const auto plan = ta::ShuttlePlanner::plan(
                        l2, ta::PayloadKind::Classical, false, 64, span);
                    if (plan.depth != analytic) expansion_match = false;
                    if (analytic < 0) analytic_match = false;
                    if (analytic < 1) nonzero_ok = false;
                }
            }
        }
        check(analytic_match, "depth_of 对任意 (策略, 对端数, span) 均非负");
        check(expansion_match, "**真实展开的轮次数 == 解析深度**（二者曾不一致）");
        check(nonzero_ok, "有对端时每种策略的深度都 ≥ 1（不存在静默零轮）");

        {
            ta::ShuttleLink wide;
            wide.from = "w"; wide.lambda = 1.0; wide.rounds = 0;
            for (int i = 0; i < 2; ++i) {
                ta::ShuttlePeer p;
                p.name = "w" + std::to_string(i);
                p.coord = {i * 1000}; p.logWeight = -1.0; p.weight = std::exp(-1.0);
                wide.peers.push_back(std::move(p));
            }
            check(ta::ShuttlePlanner::depth_of(ta::ShuttleStrategy::Butterfly, 2, 1001) >
                  ta::ShuttlePlanner::depth_of(ta::ShuttleStrategy::Ring, 2, 1001),
                  "对端少而相距远 → 蝶式比环式更深（此时环式更省）");

            ta::ShuttleLink dense;
            dense.from = "d"; dense.lambda = 1.0; dense.rounds = 0;
            for (int i = 0; i < 1024; ++i) {
                ta::ShuttlePeer p;
                p.name = "d" + std::to_string(i);
                p.coord = {i % 2}; p.logWeight = -1.0; p.weight = std::exp(-1.0);
                dense.peers.push_back(std::move(p));
            }
            check(ta::ShuttlePlanner::depth_of(ta::ShuttleStrategy::Ring, 1024, 2) >
                  ta::ShuttlePlanner::depth_of(ta::ShuttleStrategy::Butterfly, 1024, 2),
                  "对端多而聚得紧 → 环式比蝶式更深（此时蝶式更省）");
        }

        ta::ShuttleLink l3;
        l3.from = "src"; l3.lambda = 1.0; l3.rounds = 5;
        for (int i = 0; i < 4; ++i) {
            ta::ShuttlePeer p;
            p.name = "p" + std::to_string(i);
            p.coord = {i + 1}; p.logWeight = -1.0; p.weight = std::exp(-1.0);
            l3.peers.push_back(std::move(p));
        }
        for (int s : {0, 1, 2, 3, 4}) {
            ta::ShuttleLink l4 = l3;
            l4.strategy = static_cast<ta::ShuttleStrategy>(s);
            const auto pl = ta::ShuttlePlanner::plan(l4, ta::PayloadKind::Classical, false, 64, 8);
            check(pl.depth == 5, "显式 rounds=5 被原样执行（用户值即权威值）");
        }

        ta::ShuttleLink l5;
        l5.from = "src"; l5.lambda = 1.0; l5.rounds = 1;
        for (int i = 0; i < 9; ++i) {
            ta::ShuttlePeer p;
            p.name = "p" + std::to_string(i);
            p.coord = {i + 1}; p.logWeight = -static_cast<double>(i + 1);
            p.weight = std::exp(p.logWeight);
            l5.peers.push_back(std::move(p));
        }
        for (int s : {0, 1, 2, 3, 4}) {
            ta::ShuttleLink l6 = l5;
            l6.strategy = static_cast<ta::ShuttleStrategy>(s);
            const auto pl = ta::ShuttlePlanner::plan(l6, ta::PayloadKind::Classical, false, 64, 16);
            std::set<std::string> reached;
            for (const auto& r : pl.rounds)
                for (const auto& e : r.edges) reached.insert(e.peer);
            if (s == 0 || s == 3 || s == 4)
                check(reached.size() == l6.peers.size(),
                      "单轮蝶式/Grover/c001 覆盖全体对端（一轮即全体置换/蝶点辐射）");
            else
                check(!reached.empty(), "环式/树式单轮仍有落地（不是空计划）");
            check(pl.rounds.size() == 1, "rounds=1 时确实只做一轮");
        }
    }

    std::printf("[3] 相位 → 槽位映射\n");
    auto slotsOf = [](int phase, int rounds) {
        return phase == 2 ? std::pair<int, int>{rounds, rounds}
             : phase == 1 ? std::pair<int, int>{0, rounds}
                          : std::pair<int, int>{rounds, 0};
    };
    check(slotsOf(2, 1) == std::pair<int, int>{1, 1}, "phase=2 → [1,1]");
    check(slotsOf(1, 1) == std::pair<int, int>{0, 1}, "phase=1 → [0,1]");
    check(slotsOf(0, 1) == std::pair<int, int>{1, 0}, "phase=0 → [1,0]");

    std::printf("[4] 追踪三级阶梯\n");
    check(observableLevel("expect_z") == 0, "expect_z → L0/qnd（Pauli 族，Peierls 零扰动）");
    check(observableLevel("entropy") == 1, "entropy → L1/shadow（需测量，统计）");
    check(observableLevel("num_qubits") == 2, "num_qubits → L2/relabel（纯经典量）");
    check(observableLevel("measure_z") == 1, "measure_z → L1（相位不可恢复，按最弱保证）");

    {
        const auto forcedLevel = [&](const std::string &o, int mode) -> int {
            const int nat = observableLevel(o);
            if (mode == 1) return nat == 0 ? 0 : -1;
            if (mode == 2) return 1;
            if (mode == 3) return 2;
            return nat;
        };
        check(forcedLevel("entropy", 3) == 2, "track_mode=3 覆盖 entropy 的 auto 级别 → L2");
        check(forcedLevel("entropy", 2) == 1, "track_mode=2 → L1");
        check(forcedLevel("expect_z", 2) == 1, "track_mode=2 也把 Pauli 族降为 L1（强制即强制）");
        check(forcedLevel("expect_z", 3) == 2, "track_mode=3 把 Pauli 族升为 L2");
        check(forcedLevel("expect_z", 1) == 0, "track_mode=1 对 Pauli 族合法 → L0");
        check(forcedLevel("entropy", 1) == -1, "track_mode=1 对非 Pauli 族 → E-TRK003（无 QND 读出）");
        check(forcedLevel("entropy", 0) == 1, "track_mode=0（auto）→ 按族 = L1");
        check(forcedLevel("num_qubits", 0) == 2, "auto 对纯经典量 → L2");
        check(forcedLevel("entropy", 3) != forcedLevel("entropy", 0),
              "强制模式必须真的改变级别（否则 track_mode 是哑参数）");
    }

    std::printf("[5] 「哪怕关联度极小也有效」：logWeight 恒有限，对端永不丢失\n");
    {
        LayerInfo far;
        far.name = "far"; far.time = 2; far.thread = 0; far.coord = {999, 0};
        std::vector<LayerInfo> ls = layers;
        ls.push_back(far);
        auto a2 = buildAdjacency(ls);
        LayerInfo src;
        src.name = "relay"; src.coord = {0, 0};
        src.has_shuttle = true; src.shuttle_targets = {{999, 0}}; src.shuttle_lambda = 1.0;
        auto ps = resolvePeers(src, ls, a2);
        check(!ps.empty(), "极远对端(d=999)仍在穿梭计划中");
        check(std::isfinite(ps[0].logWeight), "logWeight = −999 有限");
        check(ps[0].logWeight == -999.0, "logWeight = −d/λ = −999");
        check(ps[0].weight == 0.0, "线性 weight 下溢为 0（数值零，非语义截断）");
        check(ps[0].distance == 999 && !ps[0].name.empty(),
              "对端保留全部元数据，可路由可排序，不被丢弃");
    }
    {
        LayerInfo src;
        src.name = "relay"; src.coord = {0, 0};
        src.has_shuttle = true; src.shuttle_targets = {{1, 0}}; src.shuttle_lambda = 0.5;
        auto ps = resolvePeers(src, layers, buildAdjacency(layers));
        check(ps[0].weight > 0.0, "常规距离下 weight > 0");
        check(std::fabs(ps[0].weight - std::exp(-2.0)) < 1e-15, "weight = exp(−d/λ) = exp(−2)");
    }

    std::printf("[6] 拒绝条件\n");
    {
        std::vector<LayerInfo> ls;
        LayerInfo a; a.name = "a"; a.time = 0; a.coord = {0, 0};
        LayerInfo b; b.name = "b"; b.time = 0; b.coord = {0, 0};
        ls.push_back(a); ls.push_back(b);
        LayerInfo s; s.name = "a"; s.time = 0; s.coord = {0, 0};
        s.has_shuttle = true; s.shuttle_targets = {{0, 0}};
        check(resolvePeers(s, ls, buildAdjacency(ls)).empty(), "同 coord 同 time 被拒绝");
    }
    {
        std::vector<LayerInfo> ls;
        LayerInfo a; a.name = "a"; a.time = 0; a.coord = {0, 0};
        a.has_shuttle = true; a.shuttle_targets = {{0, 0}};
        ls.push_back(a);
        check(resolvePeers(a, ls, buildAdjacency(ls)).empty(), "自我穿梭被拒绝（无对端）");
    }

    std::printf("\n=== 结果：%s ===\n", g_fail == 0 ? "全部通过" : "存在失败");
    return g_fail == 0 ? 0 : 1;
}