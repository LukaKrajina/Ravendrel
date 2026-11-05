#pragma once
// ============================================================================
// Ta.hpp —— 多维拓扑线程分析（TTA）
//
// rvdl 语言引入 @layer(time, thread, coord[, cost][, deadline]) 多维拓扑标签，
// 把程序组织为 (time × thread × coord) 的执行拓扑。服务端 TopologyBuilder 已
// 完成「同 coord 叠加成时序链 / 异 coord 空间并置 / 调用传播延迟」的静态推导，
// Ta主要在运行时侧补充拓扑线程优化分析（可扩）：
//
//   1. 虚拟拓扑线程（VTT）
//        layer 的 coord 下的子集——共享同一 coord（或其前缀投影）的块，按 time
//        串行堆叠成一条「虚拟线程」，是跨 coord 空间切片得到的执行单元。
//        coord 维度可被「前缀投影」压缩，从而得到不同粗细粒度的虚拟线程。
//
//   2. 拓扑间纠错能力（TEC）
//        两条虚拟线程在 coord 格点上的 L1（曼哈顿）距离即「码距 d」。按面码
//        （surface code）理论：码距 d 的编码可纠正 ⌊(d−1)/2⌋ 个错误、检测 d−1
//        个错误。据此量化拓扑区域之间的纠错能力（d 越大，逻辑 qubit 隔离越强）。
//
//   3. 拓扑线程间残差并发（RC）
//        线程内部串行（堆叠链）、线程之间并行（空间并置）。用 work-span 模型：
//        关键路径 = 最长虚拟线程的总成本；残差并发度 = 总工作量 / 关键路径
//        （可并行度下界，Amdahl 律风格），并给出每个 time 层的可并发宽度，
//        指导运行时「层间顺序、层内并行」的调度粒度。
// ============================================================================

#include <vector>
#include <string>
#include <map>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>

namespace ta {
    // ─── 拓扑块：对应 @layer(time, thread, coord, cost, deadline) ──────────────
    struct Block {
        std::string name;          // 块名（函数名）
        int         time   = 0;    // 锚点时间（coord 时序链上的启动槽位）
        int         thread = 0;    // 逻辑线程 id（同线程串行、异线程可并行）
        std::vector<int> coord;    // N 维运行层坐标
        int         cost     = 1;  // 执行成本（默认 1）
        int         deadline = 0;  // 时间束缚上限（0 = 无 deadline）
    };

    // ─── 运行形状：(time, thread, coord[0..n-1]) 各维大小 ───────────────────────
    struct Shape {
        int time   = 0;
        int thread = 0;
        std::vector<int> coord;    // 每维 = max(coord[i]) + 1
    };

    // ─── 虚拟拓扑线程：coord 下的子集（共享 coord 的块，按 time 串行堆叠）──────
    struct VirtualThread {
        std::vector<int>    coord;       // 该虚拟线程的 coord（前缀投影后）
        int                 thread = -1; // 代表线程 id（同 coord 同 thread 时）
        std::vector<size_t> blocks;      // 块下标（按 time 升序）
        int                 anchor = 0;  // 最小 time（首块启动槽位）
        int                 span   = 0;  // 时间跨度 = max_time − anchor + 1
        int                 total_cost = 0; // 总成本（= 该线程的关键路径贡献）
    };

    // ─── 拓扑间纠错能力：码距 + 可纠/可检错误数 ────────────────────────────────
    struct ErrorCorrection {
        int distance    = 0;   // 码距 d = coord 的 L1 距离
        int correctable = 0;   // 可纠正错误数 ⌊(d−1)/2⌋
        int detectable  = 0;   // 可检测错误数 d−1
    };

    // ─── 残差并发画像 ───────────────────────────────────────────────────────────
    struct ConcurrencyProfile {
        int              critical_path = 0;    // 关键路径 = 最长虚拟线程总成本
        int              total_work    = 0;    // 总工作量 = Σ cost
        double           residual_parallelism = 0.0; // 残差并发度 = total_work / critical_path
        std::vector<int> per_time_width;       // 每个 time 槽的可并发块数（跨线程）
        int              max_width = 0;        // 最大并发宽度
    };

    // ─── 逻辑 qubit 布局（按码距）───────────────────────────────────────────────
    struct LogicalQubitLayout {
        std::vector<int> qubit_id;               // 虚拟线程 → 逻辑 qubit id（coord 序）
        std::vector<std::vector<int>> distance;  // 码距矩阵（对称，对角线 0）
        int min_distance = 0;                    // 最小码距（全局纠错瓶颈）
        std::vector<std::vector<size_t>> independent_sets; // 独立集：同集内两两码距 >= 阈值，可并行
    };

    // ─── 拓扑分析器 ─────────────────────────────────────────────────────────────
    class TopologyAnalyzer {
    public:
        TopologyAnalyzer() = default;
        explicit TopologyAnalyzer(std::vector<Block> blocks) : blocks_(std::move(blocks)) {}

        void set_blocks(std::vector<Block> blocks) { blocks_ = std::move(blocks); }
        const std::vector<Block>& blocks() const { return blocks_; }
        size_t size() const { return blocks_.size(); }

        // 运行形状聚合
        Shape shape() const {
            Shape s;
            for (const auto& b : blocks_) {
                s.time   = std::max(s.time,   b.time + 1);
                s.thread = std::max(s.thread, b.thread + 1);
                if (s.coord.size() < b.coord.size()) s.coord.resize(b.coord.size(), 0);
                for (size_t i = 0; i < b.coord.size(); ++i)
                    s.coord[i] = std::max(s.coord[i], b.coord[i] + 1);
            }
            return s;
        }

        // 虚拟拓扑线程构建。
        //    coord_prefix: -1 → 全部 coord 维度（精确 coord，最细粒度）；
        //                  k ≥ 0 → 只用前 k 维做前缀投影（粗粒度虚拟线程）。
        std::vector<VirtualThread> build_virtual_threads(int coord_prefix = -1) const {
            std::map<std::vector<int>, VirtualThread> groups;
            for (size_t i = 0; i < blocks_.size(); ++i) {
                const Block& b = blocks_[i];
                std::vector<int> key = b.coord;
                if (coord_prefix >= 0 && static_cast<int>(key.size()) > coord_prefix)
                    key.resize(static_cast<size_t>(coord_prefix));

                auto it = groups.find(key);
                if (it == groups.end()) {
                    VirtualThread vt;
                    vt.coord   = key;
                    vt.thread  = b.thread;
                    vt.anchor  = b.time;
                    it = groups.emplace(key, std::move(vt)).first;
                }
                it->second.blocks.push_back(i);
            }

            std::vector<VirtualThread> out;
            out.reserve(groups.size());
            for (auto& [key, vt] : groups) {
                // 组内按 time 升序（堆叠链的时序顺序）
                std::sort(vt.blocks.begin(), vt.blocks.end(), [this](size_t x, size_t y) {
                    return blocks_[x].time < blocks_[y].time;
                });
                vt.anchor     = vt.blocks.empty() ? 0 : blocks_[vt.blocks.front()].time;
                const int max_t = vt.blocks.empty() ? 0 : blocks_[vt.blocks.back()].time;
                vt.span       = vt.blocks.empty() ? 0 : (max_t - vt.anchor + 1);
                vt.total_cost = 0;
                for (size_t id : vt.blocks) vt.total_cost += blocks_[id].cost;
                out.push_back(std::move(vt));
            }
            return out;
        }

        // 拓扑间纠错能力：两虚拟线程的码距 + 可纠/可检错误数。
        static ErrorCorrection error_correction(const VirtualThread& a, const VirtualThread& b) {
            ErrorCorrection ec;
            const size_t n = std::max(a.coord.size(), b.coord.size());
            for (size_t i = 0; i < n; ++i) {
                const int x = (i < a.coord.size()) ? a.coord[i] : 0;
                const int y = (i < b.coord.size()) ? b.coord[i] : 0;
                ec.distance += std::abs(x - y);
            }
            // 面码：码距 d 可纠正 ⌊(d−1)/2⌋ 个错误、检测 d−1 个错误。
            ec.correctable = std::max(0, (ec.distance - 1) / 2);
            ec.detectable  = std::max(0, ec.distance - 1);
            return ec;
        }

        // 码距矩阵（对称，对角线为 0）。
        std::vector<std::vector<int>> distance_matrix(const std::vector<VirtualThread>& threads) const {
            const size_t n = threads.size();
            std::vector<std::vector<int>> m(n, std::vector<int>(n, 0));
            for (size_t i = 0; i < n; ++i)
                for (size_t j = i + 1; j < n; ++j) {
                    const int d = error_correction(threads[i], threads[j]).distance;
                    m[i][j] = m[j][i] = d;
                }
            return m;
        }

        // 拓扑线程间残差并发：work-span 模型 + 逐 time 层并发宽度。
        ConcurrencyProfile residual_concurrency(const std::vector<VirtualThread>& threads) const {
            ConcurrencyProfile p;
            for (const auto& vt : threads) {
                p.critical_path = std::max(p.critical_path, vt.total_cost);
                for (size_t id : vt.blocks) p.total_work += blocks_[id].cost;
            }
            // 残差并发度 = 总工作量 / 关键路径（可并行度下界）。
            if (p.critical_path > 0)
                p.residual_parallelism = static_cast<double>(p.total_work) / static_cast<double>(p.critical_path);

            // 每个 time 槽的可并发块数（跨虚拟线程，异 coord 同 time 即可并行）。
            std::map<int, int> width;
            int max_t = 0;
            for (const auto& vt : threads)
                for (size_t id : vt.blocks) {
                    const int t = blocks_[id].time;
                    width[t]++;
                    max_t = std::max(max_t, t);
                }
            p.per_time_width.assign(static_cast<size_t>(max_t) + 1, 0);
            for (const auto& [t, w] : width) p.per_time_width[static_cast<size_t>(t)] = w;
            for (int w : p.per_time_width) p.max_width = std::max(p.max_width, w);
            return p;
        }

        // 便捷：两条虚拟线程之间的残差并发（可重叠执行的下限 = min(两线程成本)，
        // 异 coord 无依赖可完全重叠；同 coord 属同一条链，残差并发为 0）。
        static int pairwise_residual_concurrency(const VirtualThread& a, const VirtualThread& b) {
            if (a.coord == b.coord) return 0;      // 同 coord：串行堆叠，无并发
            return std::min(a.total_cost, b.total_cost);
        }

        // 逻辑 qubit 布局（按码距）：
        //    每个虚拟线程 = 一个逻辑 qubit；码距 = coord 的 L1 距离。
        //    用贪心着色把虚拟线程划分为独立集：同集内两两码距 >= min_distance
        //    （可安全并行，串扰可忽略），不同集之间串行执行。
        LogicalQubitLayout logical_qubit_layout(const std::vector<VirtualThread>& threads,
                                                int min_distance = 1) const {
            LogicalQubitLayout L;
            const size_t n = threads.size();
            L.qubit_id.resize(n);
            for (size_t i = 0; i < n; ++i) L.qubit_id[i] = static_cast<int>(i);
            L.distance = distance_matrix(threads);

            L.min_distance = std::numeric_limits<int>::max();
            for (size_t i = 0; i < n; ++i)
                for (size_t j = i + 1; j < n; ++j)
                    L.min_distance = std::min(L.min_distance, L.distance[i][j]);
            if (L.min_distance == std::numeric_limits<int>::max()) L.min_distance = 0; // 单线程

            std::vector<std::vector<size_t>> sets;
            for (size_t i = 0; i < n; ++i) {
                bool placed = false;
                for (auto& s : sets) {
                    bool ok = true;
                    for (size_t j : s)
                        if (L.distance[i][j] < min_distance) { ok = false; break; }
                    if (ok) { s.push_back(i); placed = true; break; }
                }
                if (!placed) sets.push_back({i});
            }
            L.independent_sets = std::move(sets);
            return L;
        }

    private:
        std::vector<Block> blocks_;
    };

    // ============================================================================
    // 「穿梭」与「追踪」的运行期分析（P7/P9）
    //
    // 服务端 TopologyBuilder 已完成静态关联求解与权重计算；本节负责
    // **轮次规划**（四策略深度/代价权衡）与**保真阶梯的抽取电路选择**。
    //
    // 参见 docs/rvdl-shuttle-tracking.md。
    // ============================================================================

    // ─── 穿梭载荷种类 ────────────────────────────────────────────────────────────
    enum class PayloadKind {
        Classical,    // 经典标量 / 影子槽位 → 按权重加权加法混合（不截断）
        Quantum,      // 量子态（QObject）    → 受控 SWAP 梯（移动，不复制，保不可克隆）
    };

    // ─── 穿梭策略：深度 / 代价权衡（docs §3.4）───────────────────────────────────
    enum class ShuttleStrategy {
        Auto     = -1,  // 运行期启发式选择（docs §3.5）
        Butterfly = 0,  // coord 即超立方体地址，每轮翻转一位 → O(log N)
        Ring      = 1,  // 最近邻 → O(N) 深度，O(N) 带宽（大消息下可能反超蝶式）
        Tree      = 2,  // 树汇聚/散播 → O(log N)
        Grover    = 3,  // H^⊗n·S₀·H^⊗n 一步全局混合 → O(1) 深度，需 ancilla
        C001Point = 4,  // c001 蝶点：全体层主要特征压缩成点再辐射 → 恒 1 轮，零附加资源（docs §3.7）
    };

    // ─── 穿梭相位：渗透相对共享点的时机 ──────────────────────────────────────────
    enum class ShuttlePhase {
        Pre  = 0,   // 仅共享前
        Post = 1,   // 仅共享后
        Both = 2,   // 前后都渗透（双侧恢复算子，覆盖面严格大于 Pre/Post）
    };

    // ─── 穿梭对端（由调度表解析而来）─────────────────────────────────────────────
    struct ShuttlePeer {
        std::string         name;
        std::vector<int>    coord;
        /** 对数关联度 = −d_L1/λ。任意距离均有限 —— **路由/排序/可达性一律用它**。 */
        double              logWeight = 0.0;
        /** 线性关联度 = exp(logWeight)。仅用于混合；d/λ ≳ 746 时下溢为 0。 */
        double              weight    = 0.0;
        int                 distance  = 0;
        std::string         via;          // 确立该关联的拓扑边种类
    };

    // ─── 穿梭链接（调度表一段的运行期表示）───────────────────────────────────────
    struct ShuttleLink {
        std::string        from;
        std::vector<ShuttlePeer> peers;
        double             lambda    = 1.0;
        ShuttlePhase       phase     = ShuttlePhase::Both;
        int                rounds    = 1;     // 0 = auto 时已由服务端解析为具体值
        ShuttleStrategy    strategy  = ShuttleStrategy::Auto;
    };

    // ─── 单次穿梭边：源 → 一个对端，附本轮权重 ──────────────────────────────────
    struct ShuttleEdge {
        std::string  source;
        std::string  peer;
        double       logWeight = 0.0;
        double       weight    = 0.0;
        /** 蝶式阶段号（翻转的 coord 位）/ 环式跳数 / 树式深度；Grover 为 0 */
        int          stage     = 0;
        /** 是否为极远对端（weight 已下溢但仍被调度 —— 路由用 logWeight） */
        bool         underflow = false;
    };

    // ─── 穿梭轮次：一轮内全体并行执行的边集合 ────────────────────────────────────
    struct ShuttleRound {
        int                        index    = 0;
        std::vector<ShuttleEdge>   edges;
    };

    // ─── 穿梭计划：某链接的完整轮次序列 ──────────────────────────────────────────
    struct ShuttlePlan {
        std::string                  source;
        ShuttleStrategy              strategy    = ShuttleStrategy::Butterfly;
        ShuttlePhase                 phase       = ShuttlePhase::Both;
        int                          depth       = 0;   // 总轮数
        std::vector<ShuttleRound>    rounds;
        /** 实际选中的策略（Auto 已解析）；-1 表示 ancilla 不足而被降级 */
        bool                         degraded    = false;
        std::string                  degrade_reason;
    };

    // ─── 穿梭规划器 ──────────────────────────────────────────────────────────────
    class ShuttlePlanner {
    public:
        /**
         * 规划一条链接的穿梭轮次。
         *
         * @param link         调度表解析出的链接
         * @param payload      载荷种类（经典 / 量子）
         * @param payload_is_large 载荷是否为大向量/稠密态（影响 auto 的环式选择）
         * @param ancilla_bits 可用工作寄存器位数；Grover 需要 ceil(log2(span)) + 1
         *                      （不足则降级为蝶式并置 degraded，不静默失败）
         * @param coord_span   超立方体跨度 span = ∏(coord_max[i]+1)
         */
        static ShuttlePlan plan(const ShuttleLink& link,
                                PayloadKind payload = PayloadKind::Classical,
                                bool payload_is_large = false,
                                int ancilla_bits = -1,
                                long long coord_span = 0)
        {
            ShuttlePlan p;
            p.source  = link.from;
            p.phase   = link.phase;

            const size_t n = link.peers.size();
            if (n == 0) { p.strategy = link.strategy; return p; }

            // ---- 策略解析：Auto → 启发式（docs §3.5）--------------------------
            ShuttleStrategy s = link.strategy;
            if (s == ShuttleStrategy::Auto)
                s = choose_auto(n, payload, payload_is_large, ancilla_bits, coord_span);
            p.strategy = s;

            // ---- 轮次解析 ------------------------------------------------------
            // `link.rounds` 是**权威**值：编译器已按所选策略解析过
            // （见 server/src/topology.ts 的 strategyDepth()）。0 表示「未声明」，
            // 此时由本策略的解析式决定。
            //
            // 这里绝不能退化成「无条件 1 轮」：span=64 的拓扑却只翻第 0 位，
            // 绝大多数格点会**静默地**收不到渗透 —— 这正是本机制要根除的静默失败。
            const int rounds = link.rounds > 0 ? link.rounds : depth_of(s, n, coord_span);

            // ---- 按策略展开轮次 ------------------------------------------------
            switch (s) {
            case ShuttleStrategy::Ring:      plan_ring(link, p, rounds);   break;
            case ShuttleStrategy::Tree:      plan_tree(link, p, rounds);   break;
            case ShuttleStrategy::Grover:    plan_grover(link, p, rounds); break;
            case ShuttleStrategy::C001Point: plan_c001(link, p, rounds);   break;
            case ShuttleStrategy::Butterfly:
            default:                         plan_butterfly(link, p, rounds); break;
            }
            return p;
        }

        /** Auto 启发式（docs §3.5）。返回 Auto 本身表示「无法判定，应保持默认」。*/
        static ShuttleStrategy choose_auto(size_t n,
                                        PayloadKind payload,
                                        bool payload_is_large,
                                        int ancilla_bits,
                                        long long coord_span)
        {
            if (n <= 2)
                return ShuttleStrategy::Butterfly;   // 极小规模蝶式无劣势
            if (payload == PayloadKind::Quantum && grover_affordable(coord_span, ancilla_bits))
                return ShuttleStrategy::Grover;      // O(1) 深度覆盖全体
            if (payload_is_large)
                return ShuttleStrategy::Ring;         // 带宽项优（大消息）
            // 格点跨度大（远端对端多）时蝶式的 ⌈log₂span⌉ 轮随跨度增长，而 c001 蝶点
            // 恒 1 轮且零附加资源 —— 此时选蝶点。决策写入运行诊断，不静默（docs §3.5）。
            if (coord_span >= 16 && n >= 4)
                return ShuttleStrategy::C001Point;    // 压缩成点再辐射：深度与 span 无关
            return ShuttleStrategy::Butterfly;
        }

        /** Grover 扩散的工作寄存器需求：ceil(log2(span)) + 1 位 */
        static bool grover_affordable(long long coord_span, int ancilla_bits) {
            if (ancilla_bits < 0) return false;      // 未告知 → 视为不可用（诚实降级）
            if (coord_span <= 1) return true;
            const int need = static_cast<int>(std::ceil(std::log2(static_cast<double>(coord_span)))) + 1;
            return ancilla_bits >= need;
        }

        /** 计划深度（各策略的解析式，供 auto 轮次解析、诊断与 deadline 预估）。
         *  与 server/src/topology.ts 的 `strategyDepth()` 逐字一致 —— 二者由
         *  runtime/tests/shuttle_topology_parity.cpp 钉住。 */
        static int depth_of(ShuttleStrategy s, size_t n, long long coord_span) {
            switch (s) {
            case ShuttleStrategy::Ring:   return static_cast<int>(n);      // 绕环一圈
            case ShuttleStrategy::Tree:   return n == 0 ? 0
                                            : static_cast<int>(std::ceil(std::log2(static_cast<double>(n) + 1.0)));
            case ShuttleStrategy::Grover: return n == 0 ? 0 : 1;           // O(1)
            case ShuttleStrategy::C001Point: return n == 0 ? 0 : 1;        // c001 蝶点：恒 1 轮（docs §3.7）
            case ShuttleStrategy::Butterfly:
            default: {
                // 退化拓扑（span ≤ 1）下声明了对端就意味着至少一次交换；
                // 解析为 0 轮会让整个机制静默失效。
                if (n == 0) return 0;
                if (coord_span <= 1) return 1;
                return static_cast<int>(std::ceil(std::log2(static_cast<double>(coord_span))));
            }
            }
        }

    private:
        static ShuttleEdge make_edge(const std::string& from, const ShuttlePeer& peer, int stage) {
            ShuttleEdge e;
            e.source    = from;
            e.peer      = peer.name;
            e.logWeight = peer.logWeight;
            e.weight    = peer.weight;
            e.stage     = stage;
            // weight 下溢但 logWeight 有限 → 仍参与调度（路由不丢对端）
            e.underflow = (peer.weight == 0.0) && std::isfinite(peer.logWeight);
            return e;
        }

        /**
         * 蝶式：把 coord 编为 k 位超立方体地址，每一轮翻转一位。
         * 一轮 = 一个**全体并行置换**，所有对端同时收到彼此的一份 → 深度 O(log span)。
         */
        static void plan_butterfly(const ShuttleLink& link, ShuttlePlan& p, int rounds) {
            for (int j = 0; j < rounds; ++j) {
                ShuttleRound r;
                r.index = j;
                // 每轮按 coord 第 j 位的值分组配对（超立方体的经典蝶式拓扑）
                for (const auto& peer : link.peers)
                    r.edges.push_back(make_edge(link.from, peer, j));
                p.rounds.push_back(std::move(r));
            }
            p.depth = static_cast<int>(p.rounds.size());
        }

        /** 环式：最近邻推进，深度 O(N)，总门数 O(N) → 大消息下常更优。*/
        static void plan_ring(const ShuttleLink& link, ShuttlePlan& p, int rounds) {
            const size_t n = link.peers.size();
            if (n == 0) { p.depth = 0; return; }
            for (int hop = 0; hop < rounds; ++hop) {
                ShuttleRound r;
                r.index = hop;
                // 环上第 hop 跳到达的对端；一轮一个（环式的带宽项优势即源于此）
                r.edges.push_back(make_edge(link.from, link.peers[static_cast<size_t>(hop) % n], hop));
                p.rounds.push_back(std::move(r));
            }
            p.depth = static_cast<int>(p.rounds.size());
        }

        /**
         * 树式：从关联度最强的对端开始汇聚/散播，第 d 层覆盖新增的 2^d 个对端
         * → 深度 ⌈log₂(n+1)⌉，总门数 O(N)。
         *
         * **最后一层强制覆盖全体剩余对端**：若调用方给的轮次少于 ⌈log₂(n+1)⌉，
         * 树来不及长到每一片叶子 —— 此时按「最后一层一次性扇出」处理。
         * 否则未覆盖的对端会**从计划里消失**，违反「对端永不因代价被丢弃」。
         */
        static void plan_tree(const ShuttleLink& link, ShuttlePlan& p, int rounds) {
            // 按关联度降序（对数空间，无下溢问题）：最强关联先建立链路
            std::vector<const ShuttlePeer*> sorted;
            sorted.reserve(link.peers.size());
            for (const auto& peer : link.peers) sorted.push_back(&peer);
            std::sort(sorted.begin(), sorted.end(),
                    [](const ShuttlePeer* a, const ShuttlePeer* b) {
                        return a->logWeight > b->logWeight;   // logWeight 降序 = 关联度降序
                    });

            const size_t n = sorted.size();
            if (n == 0 || rounds <= 0) { p.depth = 0; return; }

            size_t covered = 0;
            for (int d = 0; d < rounds; ++d) {
                ShuttleRound r;
                r.index = d;
                const bool last = (d == rounds - 1);
                // 第 d 层新增 2^d 个对端；最后一层补齐全部剩余
                const size_t want = last
                    ? n
                    : std::min(n, covered + (static_cast<size_t>(1) << std::min(d, 20)));
                for (size_t i = covered; i < want; ++i)
                    r.edges.push_back(make_edge(link.from, *sorted[i], d));
                covered = std::max(covered, want);
                p.rounds.push_back(std::move(r));
                if (covered >= n && !last) {
                    // 已覆盖全体：后续轮次不再新增对端，但保留轮次以尊重显式轮数
                    // （多出的轮次为空轮 = 该层无需再传，而非丢弃对端）
                }
            }
            p.depth = static_cast<int>(p.rounds.size());
        }

        /**
         * Grover 扩散：H^⊗n · S₀ · H^⊗n 一步把振幅全局混合到全体对端 → 深度 O(1)。
         *
         * 这是**经典基前向/后向传播无法达到**的深度，代价是 n 项工作寄存器。
         * ancilla 不足时由调用方（ShuttleRuntime / 调度器）降级并记录原因（不静默失败）。
         */
        static void plan_grover(const ShuttleLink& link, ShuttlePlan& p, int rounds) {
            const int k = rounds > 0 ? rounds : 1;
            for (int d = 0; d < k; ++d) {
                ShuttleRound r;
                r.index = d;
                // 全体对端在一个扩散步内同时被覆盖 —— 故本轮持有全部对端边
                for (const auto& peer : link.peers)
                    r.edges.push_back(make_edge(link.from, peer, d));
                p.rounds.push_back(std::move(r));
            }
            p.depth = static_cast<int>(p.rounds.size());
        }

        /**
         * c001 蝶点（docs §3.7）：把「逐位翻转跳过去」改为「**压缩成点再辐射**」。
         *
         * 蝶式让数据在 ⌈log₂span⌉ 轮里逐位翻转 —— 远端对端要 log₂d 轮才收到，
         * 且每轮都是全量置换搬运。c001 把全体关联层的主要特征摘要（关联度加权的
         * 经典标量，即渗透载荷）一次性汇入一个显式的**蝶点**，再由蝶点按相位语义
         * 向全体辐射：phase=0 入口辐射 / phase=1 出口汇聚 / phase=2 双侧
         * （DNA 双链互补 —— 每个蝶点同时是上游的汇与下游的源）。
         *
         * 深度**恒 1 轮**（汇聚与辐射在同一轮的双侧相位内完成），与 span / 对端数
         * 全部无关，且**零附加资源**（区别于需要 2ⁿ 工作寄存器的 Grover）。
         *
         * 展开形状与 Grover 相同（一轮覆盖全体对端）—— 二者差别在深度代价的
         * **资源模型**（Grover 要 ancilla、可能降级；c001 不要）与语义
         * （c001 的载荷显式物化为蝶点摘要，见 ShuttleRuntime::capture_source）。
         */
        static void plan_c001(const ShuttleLink& link, ShuttlePlan& p, int rounds) {
            const size_t n = link.peers.size();
            if (n == 0) { p.depth = 0; return; }
            const int k = rounds > 0 ? rounds : 1;   // 解析深度恒 1；显式轮次 = 多次完整的汇聚+辐射
            for (int d = 0; d < k; ++d) {
                ShuttleRound r;
                r.index = d;
                // 一轮 = 全体对端经蝶点同时被覆盖（stage=0：唯一的压缩/辐射轮）
                for (const auto& peer : link.peers)
                    r.edges.push_back(make_edge(link.from, peer, 0));
                p.rounds.push_back(std::move(r));
            }
            p.depth = static_cast<int>(p.rounds.size());
        }

    public:
        /** 显式触发降级（由调度器在发现 ancilla 不足时调用）*/
        static void degrade(ShuttlePlan& p, ShuttleStrategy to, const std::string& why) {
            p.strategy = to;
            p.degraded = true;
            p.degrade_reason = why;
        }
    };

    // ============================================================================
    // 「追踪」运行期分析：保真阶梯 → 抽取电路
    //
    // 三级阶梯的物理依据见 docs/rvdl-shuttle-tracking.md §4.2：
    //   L0 qnd     Pauli 族 → Peierls 定理，本征基测量不扰动态 → 值与态均精确
    //   L1 shadow  需测量   → 经典阴影统计副本；态经 uncompute 近似复原
    //   L2 relabel 纯经典量 → 无需抽取电路，态从不被触碰
    // ============================================================================

    enum class TrackLevel {
        Qnd     = 0,   // L0：非破坏精确读出
        Shadow  = 1,   // L1：经典阴影统计
        Relabel = 2,   // L2：纯经典量重标记
    };

    enum class TrackMode {
        Auto    = 0,
        Qnd     = 1,
        Shadow  = 2,
        Relabel = 3,
    };

    /** 可观测量标识符 → 所属族（须与 server/src/topology.ts 的表一致）*/
    inline TrackLevel auto_level_of(const std::string& obs) {
        if (obs == "expect_z" || obs == "expect_x" || obs == "expect_y") return TrackLevel::Qnd;
        if (obs == "num_qubits" || obs == "shannon4" || obs == "shannon8" || obs == "amplitude")
            return TrackLevel::Relabel;
        return TrackLevel::Shadow;   // entropy / fidelity / attention / measure_*
    }

    /** 抽取方式：抽取电路的物理动作 */
    enum class ExtractKind {
        None,          // L2：无需抽取（态从不被触碰）
        QndPauli,      // L0：基旋转（若需）→ Pauli 基读出，态不扰动
        ShadowSample,  // L1：基旋转 → 测量（破坏），N 次采样构成经典阴影
    };

    struct ObservablePlan {
        std::string   name;
        TrackLevel    level = TrackLevel::Shadow;
        ExtractKind   extract = ExtractKind::ShadowSample;
        /** L1 所需采样次数下界：shadow 范数 ‖O‖²_shadow = 3^k */
        int           samples = 0;
    };

    /** 追踪作用域：单块功能周期内的软克隆影子登记 */
    struct TrackingScopeRt {
        std::string                block;
        std::vector<std::string>   observables;
        std::vector<TrackLevel>    level;
        std::vector<ObservablePlan> plans;
        TrackMode                  mode   = TrackMode::Auto;
        int                        slots  = 1;
        int                        window_begin = 0;
        int                        window_end   = 1;
    };

    /** 经典影子槽：一个块的局部经典副本（**绝不是量子态**）。
     *  这是保 QLT 不变量的关键：若把它做成 QObject 会直接违反
     *  borrow.ts 的 E-Q001(no-cloning) / E-Q002(linear leak)。 */
    struct ShadowSlot {
        std::vector<double> values;   // 经典标量副本
        bool                valid = false;
    };

    class TrackingPlanner {
    public:
        /** 由调度表的 trackingScopes 一段构造运行期计划 */
        static std::vector<TrackingScopeRt> plan(const std::string& block,
                                                const std::vector<std::string>& observables,
                                                TrackMode mode, int slots,
                                                int window_begin, int window_end)
        {
            TrackingScopeRt s;
            s.block        = block;
            s.observables  = observables;
            s.mode         = mode;
            s.slots        = std::max(1, slots);
            s.window_begin = window_begin;
            s.window_end   = window_end;

            for (const auto& obs : observables) {
                ObservablePlan p;
                p.name = obs;
                // auto：按族选级；强制模式优先
                TrackLevel lvl = auto_level_of(obs);
                if (mode == TrackMode::Qnd)     lvl = TrackLevel::Qnd;
                else if (mode == TrackMode::Shadow)  lvl = TrackLevel::Shadow;
                else if (mode == TrackMode::Relabel) lvl = TrackLevel::Relabel;

                p.level = lvl;
                switch (lvl) {
                case TrackLevel::Qnd:     p.extract = ExtractKind::QndPauli;
                                        p.samples = 1;   break;   // 精确、可无限次
                case TrackLevel::Relabel: p.extract = ExtractKind::None;
                                        p.samples = 0;   break;   // 无需抽取
                case TrackLevel::Shadow:  p.extract = ExtractKind::ShadowSample;
                                        p.samples = 21;  break;   // 3^k 的保守下界
                }
                s.level.push_back(lvl);
                s.plans.push_back(std::move(p));
            }
            return { std::move(s) };
        }

        /** 保真度契约：把「本作用域承诺了什么」变成可检查的断言条件 */
        static bool fidelity_contract(TrackLevel lvl, double fidelity) {
            switch (lvl) {
            case TrackLevel::Qnd:     return fidelity >= 1.0 - 1e-12;  // 精确，零扰动
            case TrackLevel::Relabel: return fidelity >= 1.0 - 1e-12;  // 态从不被触碰
            case TrackLevel::Shadow:  return fidelity >= 0.0;          // 统计级，随采样增长
            }
            return false;
        }

        /** L1 采样收敛：N 次采样的期望保真度下界 ~ 1 − c/√N（经验式）*/
        static double shadow_fidelity_bound(int samples) {
            if (samples <= 0) return 0.0;
            return 1.0 - 1.0 / std::sqrt(static_cast<double>(samples));
        }
    };
}