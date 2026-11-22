#pragma once

#include <vector>
#include <string>
#include <map>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>

namespace ta {
    struct Block {
        std::string name;
        int time = 0;
        int thread = 0;
        std::vector<int> coord;
        int cost = 1;
        int deadline = 0;
    };

    struct Shape {
        int time   = 0;
        int thread = 0;
        std::vector<int> coord;
    };

    struct VirtualThread {
        std::vector<int> coord;
        int thread = -1;
        std::vector<size_t> blocks;
        int anchor = 0;
        int span = 0;
        int total_cost = 0;
    };

    struct ErrorCorrection {
        int distance = 0;
        int correctable = 0;
        int detectable = 0;
    };

    struct ConcurrencyProfile {
        int critical_path = 0;
        int total_work = 0;
        double residual_parallelism = 0.0;
        std::vector<int> per_time_width;
        int max_width = 0;
    };

    struct LogicalQubitLayout {
        std::vector<int> qubit_id;
        std::vector<std::vector<int>> distance;
        int min_distance = 0;
        std::vector<std::vector<size_t>> independent_sets;
    };

    class TopologyAnalyzer {
    public:
        TopologyAnalyzer() = default;
        explicit TopologyAnalyzer(std::vector<Block> blocks) : blocks_(std::move(blocks)) {}

        void set_blocks(std::vector<Block> blocks) { blocks_ = std::move(blocks); }
        const std::vector<Block>& blocks() const { return blocks_; }
        size_t size() const { return blocks_.size(); }

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

        static ErrorCorrection error_correction(const VirtualThread& a, const VirtualThread& b) {
            ErrorCorrection ec;
            const size_t n = std::max(a.coord.size(), b.coord.size());
            for (size_t i = 0; i < n; ++i) {
                const int x = (i < a.coord.size()) ? a.coord[i] : 0;
                const int y = (i < b.coord.size()) ? b.coord[i] : 0;
                ec.distance += std::abs(x - y);
            }
            ec.correctable = std::max(0, (ec.distance - 1) / 2);
            ec.detectable  = std::max(0, ec.distance - 1);
            return ec;
        }

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

        ConcurrencyProfile residual_concurrency(const std::vector<VirtualThread>& threads) const {
            ConcurrencyProfile p;
            for (const auto& vt : threads) {
                p.critical_path = std::max(p.critical_path, vt.total_cost);
                for (size_t id : vt.blocks) p.total_work += blocks_[id].cost;
            }
            if (p.critical_path > 0)
                p.residual_parallelism = static_cast<double>(p.total_work) / static_cast<double>(p.critical_path);

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

        static int pairwise_residual_concurrency(const VirtualThread& a, const VirtualThread& b) {
            if (a.coord == b.coord) return 0;
            return std::min(a.total_cost, b.total_cost);
        }

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
            if (L.min_distance == std::numeric_limits<int>::max()) L.min_distance = 0;

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


    enum class PayloadKind {
        Classical,
        Quantum,
    };

    enum class ShuttleStrategy {
        Auto = -1,
        Butterfly = 0,
        Ring = 1,
        Tree = 2,
        Grover = 3,
        C001Point = 4,
    };

    enum class ShuttlePhase {
        Pre = 0,
        Post = 1,
        Both = 2,
    };

    struct ShuttlePeer {
        std::string         name;
        std::vector<int>    coord;
        double              logWeight = 0.0;
        double              weight    = 0.0;
        int                 distance  = 0;
        std::string via;
    };

    struct ShuttleLink {
        std::string        from;
        std::vector<ShuttlePeer> peers;
        double             lambda    = 1.0;
        ShuttlePhase       phase     = ShuttlePhase::Both;
        int rounds = 1;
        ShuttleStrategy    strategy  = ShuttleStrategy::Auto;
    };

    struct ShuttleEdge {
        std::string  source;
        std::string  peer;
        double       logWeight = 0.0;
        double       weight    = 0.0;
        int          stage     = 0;
        bool         underflow = false;
    };

    struct ShuttleRound {
        int                        index    = 0;
        std::vector<ShuttleEdge>   edges;
    };

    struct ShuttlePlan {
        std::string                  source;
        ShuttleStrategy              strategy    = ShuttleStrategy::Butterfly;
        ShuttlePhase                 phase       = ShuttlePhase::Both;
        int depth = 0;
        std::vector<ShuttleRound>    rounds;
        bool                         degraded    = false;
        std::string                  degrade_reason;
    };

    class ShuttlePlanner {
    public:









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

            ShuttleStrategy s = link.strategy;
            if (s == ShuttleStrategy::Auto)
                s = choose_auto(n, payload, payload_is_large, ancilla_bits, coord_span);
            p.strategy = s;

            const int rounds = link.rounds > 0 ? link.rounds : depth_of(s, n, coord_span);

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

        static ShuttleStrategy choose_auto(size_t n,
                                        PayloadKind payload,
                                        bool payload_is_large,
                                        int ancilla_bits,
                                        long long coord_span)
        {
            if (n <= 2)
                return ShuttleStrategy::Butterfly;
            if (payload == PayloadKind::Quantum && grover_affordable(coord_span, ancilla_bits))
                return ShuttleStrategy::Grover;
            if (payload_is_large)
                return ShuttleStrategy::Ring;
            if (coord_span >= 16 && n >= 4)
                return ShuttleStrategy::C001Point;
            return ShuttleStrategy::Butterfly;
        }

        static bool grover_affordable(long long coord_span, int ancilla_bits) {
            if (ancilla_bits < 0) return false;
            if (coord_span <= 1) return true;
            const int need = static_cast<int>(std::ceil(std::log2(static_cast<double>(coord_span)))) + 1;
            return ancilla_bits >= need;
        }



        static int depth_of(ShuttleStrategy s, size_t n, long long coord_span) {
            switch (s) {
            case ShuttleStrategy::Ring: return static_cast<int>(n);
            case ShuttleStrategy::Tree:   return n == 0 ? 0
                                            : static_cast<int>(std::ceil(std::log2(static_cast<double>(n) + 1.0)));
            case ShuttleStrategy::Grover: return n == 0 ? 0 : 1;
            case ShuttleStrategy::C001Point: return n == 0 ? 0 : 1;
            case ShuttleStrategy::Butterfly:
            default: {
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
            e.underflow = (peer.weight == 0.0) && std::isfinite(peer.logWeight);
            return e;
        }




        static void plan_butterfly(const ShuttleLink& link, ShuttlePlan& p, int rounds) {
            for (int j = 0; j < rounds; ++j) {
                ShuttleRound r;
                r.index = j;
                for (const auto& peer : link.peers)
                    r.edges.push_back(make_edge(link.from, peer, j));
                p.rounds.push_back(std::move(r));
            }
            p.depth = static_cast<int>(p.rounds.size());
        }

        static void plan_ring(const ShuttleLink& link, ShuttlePlan& p, int rounds) {
            const size_t n = link.peers.size();
            if (n == 0) { p.depth = 0; return; }
            for (int hop = 0; hop < rounds; ++hop) {
                ShuttleRound r;
                r.index = hop;
                r.edges.push_back(make_edge(link.from, link.peers[static_cast<size_t>(hop) % n], hop));
                p.rounds.push_back(std::move(r));
            }
            p.depth = static_cast<int>(p.rounds.size());
        }








        static void plan_tree(const ShuttleLink& link, ShuttlePlan& p, int rounds) {
            std::vector<const ShuttlePeer*> sorted;
            sorted.reserve(link.peers.size());
            for (const auto& peer : link.peers) sorted.push_back(&peer);
            std::sort(sorted.begin(), sorted.end(),
                    [](const ShuttlePeer* a, const ShuttlePeer* b) {
                        return a->logWeight > b->logWeight;
                    });

            const size_t n = sorted.size();
            if (n == 0 || rounds <= 0) { p.depth = 0; return; }

            size_t covered = 0;
            for (int d = 0; d < rounds; ++d) {
                ShuttleRound r;
                r.index = d;
                const bool last = (d == rounds - 1);
                const size_t want = last
                    ? n
                    : std::min(n, covered + (static_cast<size_t>(1) << std::min(d, 20)));
                for (size_t i = covered; i < want; ++i)
                    r.edges.push_back(make_edge(link.from, *sorted[i], d));
                covered = std::max(covered, want);
                p.rounds.push_back(std::move(r));
                if (covered >= n && !last) {
                }
            }
            p.depth = static_cast<int>(p.rounds.size());
        }






        static void plan_grover(const ShuttleLink& link, ShuttlePlan& p, int rounds) {
            const int k = rounds > 0 ? rounds : 1;
            for (int d = 0; d < k; ++d) {
                ShuttleRound r;
                r.index = d;
                for (const auto& peer : link.peers)
                    r.edges.push_back(make_edge(link.from, peer, d));
                p.rounds.push_back(std::move(r));
            }
            p.depth = static_cast<int>(p.rounds.size());
        }
















        static void plan_c001(const ShuttleLink& link, ShuttlePlan& p, int rounds) {
            const size_t n = link.peers.size();
            if (n == 0) { p.depth = 0; return; }
            const int k = rounds > 0 ? rounds : 1;
            for (int d = 0; d < k; ++d) {
                ShuttleRound r;
                r.index = d;
                for (const auto& peer : link.peers)
                    r.edges.push_back(make_edge(link.from, peer, 0));
                p.rounds.push_back(std::move(r));
            }
            p.depth = static_cast<int>(p.rounds.size());
        }

    public:
        static void degrade(ShuttlePlan& p, ShuttleStrategy to, const std::string& why) {
            p.strategy = to;
            p.degraded = true;
            p.degrade_reason = why;
        }
    };


    enum class TrackLevel {
        Qnd = 0,
        Shadow = 1,
        Relabel = 2,
    };

    enum class TrackMode {
        Auto    = 0,
        Qnd     = 1,
        Shadow  = 2,
        Relabel = 3,
    };

    inline TrackLevel auto_level_of(const std::string& obs) {
        if (obs == "expect_z" || obs == "expect_x" || obs == "expect_y") return TrackLevel::Qnd;
        if (obs == "num_qubits" || obs == "shannon4" || obs == "shannon8" || obs == "amplitude")
            return TrackLevel::Relabel;
        return TrackLevel::Shadow;
    }

    enum class ExtractKind {
        None,
        QndPauli,
        ShadowSample,
    };

    struct ObservablePlan {
        std::string   name;
        TrackLevel    level = TrackLevel::Shadow;
        ExtractKind   extract = ExtractKind::ShadowSample;
        int           samples = 0;
    };

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



    struct ShadowSlot {
        std::vector<double> values;
        bool                valid = false;
    };

    class TrackingPlanner {
    public:
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
                TrackLevel lvl = auto_level_of(obs);
                if (mode == TrackMode::Qnd)     lvl = TrackLevel::Qnd;
                else if (mode == TrackMode::Shadow)  lvl = TrackLevel::Shadow;
                else if (mode == TrackMode::Relabel) lvl = TrackLevel::Relabel;

                p.level = lvl;
                switch (lvl) {
                case TrackLevel::Qnd:     p.extract = ExtractKind::QndPauli;
                                        p.samples = 1; break;
                case TrackLevel::Relabel: p.extract = ExtractKind::None;
                                        p.samples = 0; break;
                case TrackLevel::Shadow:  p.extract = ExtractKind::ShadowSample;
                                        p.samples = 21; break;
                }
                s.level.push_back(lvl);
                s.plans.push_back(std::move(p));
            }
            return { std::move(s) };
        }

        static bool fidelity_contract(TrackLevel lvl, double fidelity) {
            switch (lvl) {
            case TrackLevel::Qnd: return fidelity >= 1.0 - 1e-12;
            case TrackLevel::Relabel: return fidelity >= 1.0 - 1e-12;
            case TrackLevel::Shadow: return fidelity >= 0.0;
            }
            return false;
        }

        static double shadow_fidelity_bound(int samples) {
            if (samples <= 0) return 0.0;
            return 1.0 - 1.0 / std::sqrt(static_cast<double>(samples));
        }
    };
}