#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <iostream>
#include <map>
#include <mutex>
#include <ostream>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "Ta.hpp"
#include "../qhal/json.hpp"

namespace rvdl
{
    struct ShadowEntry
    {
        double value = 0.0;
        bool   valid = false;
    };

    static_assert(std::is_trivially_copyable<ShadowEntry>::value &&
                    std::is_standard_layout<ShadowEntry>::value &&
                    !std::is_polymorphic<ShadowEntry>::value,
                "影子槽必须只承载经典标量 —— 引入 QObject 即刻违反 E-Q001/E-Q002");

    using ShadowSlots = std::vector<ShadowEntry>;

    struct ShuttleRuntimeStats
    {
        size_t shuttles = 0;
        size_t scopes = 0;
        size_t underflow_peers = 0;
        size_t degraded = 0;
        size_t c001_points = 0;
        size_t w_shu001 = 0;
        size_t captures = 0;
        size_t deliveries = 0;
        size_t no_target_shadow = 0;
        size_t no_payload = 0;
        size_t returns = 0;
        size_t invalid_scopes = 0;
        size_t rounds_executed  = 0;
    };

    class ShuttleRuntime
    {
    public:
        explicit ShuttleRuntime(std::ostream *log = &std::cout) : log_(log) {}

        void parse(const qhal::json::Value &doc)
        {
            parse_shuttles(doc);
            parse_scopes(doc);
            plans_.clear();
            link_state_.clear();
            stats_.shuttles = shuttles_.size();
            stats_.scopes   = scopes_.size();
        }

        void plan_all(int ancilla_bits, long long coord_span)
        {
            plans_.clear();
            link_state_.clear();
            for (const auto &link : shuttles_)
            {
                const bool has_track = scope_of_.count(link.from) != 0;

                ta::ShuttlePlan p = ta::ShuttlePlanner::plan(link, ta::PayloadKind::Classical,
                                                             false,
                                                            ancilla_bits, coord_span);

                if (p.strategy == ta::ShuttleStrategy::Grover &&
                    !ta::ShuttlePlanner::grover_affordable(coord_span, ancilla_bits))
                {
                    ta::ShuttleLink fallen = link;
                    fallen.strategy = ta::ShuttleStrategy::Butterfly;
                    fallen.rounds = 0;
                    ta::ShuttlePlan q = ta::ShuttlePlanner::plan(
                        fallen, ta::PayloadKind::Classical, false,
                        ancilla_bits, coord_span);
                    ta::ShuttlePlanner::degrade(q, ta::ShuttleStrategy::Butterfly,
                                                "ancilla 工作寄存器不足（需 ⌈log2 span⌉+1 位）");
                    p = std::move(q);
                    ++stats_.degraded;
                    if (log_)
                        *log_ << "[Shuttle] W-TRK001: strategy=3 缺少 ancilla → 降级为策略 0"
                            << " (from=" << link.from << ", span=" << coord_span
                            << ", ancilla_bits=" << ancilla_bits
                            << ", 重规划为 " << p.depth << " 轮)" << std::endl;
                }
                if (!has_track)
                {
                    ++stats_.w_shu001;
                    if (log_)
                        *log_ << "[Shuttle] W-SHU001: 请求相干穿梭但无 @track → "
                            << "退化为经典影子混合 (from=" << link.from << ")" << std::endl;
                }

                const bool was_auto = (link.strategy == ta::ShuttleStrategy::Auto);
                if (log_)
                {
                    *log_ << "[Shuttle] from=" << p.source
                        << " strategy=" << static_cast<int>(p.strategy)
                        << (was_auto ? " (auto)" : " (显式)")
                        << " phase=" << static_cast<int>(p.phase)
                        << " rounds=" << p.depth
                        << " peers=" << link.peers.size()
                        << " lambda=" << link.lambda
                        << " span=" << coord_span << std::endl;
                }

                for (const auto &peer : link.peers)
                {
                    const bool uf = is_underflow(peer);
                    if (uf)
                        ++stats_.underflow_peers;
                    if (log_)
                        *log_ << "[Shuttle]   peer=" << peer.name
                            << " coord=(" << join_ints(peer.coord) << ")"
                            << " logWeight=" << peer.logWeight
                            << " weight=" << peer.weight
                            << " distance=" << peer.distance
                            << " via=" << peer.via
                            << (uf ? " [线性权重下溢：仍按 logWeight 调度，不丢弃]" : "")
                            << std::endl;
                }

                plans_.push_back({std::move(p), &link});
                link_state_.emplace_back();
            }
        }

        void scope_enter(const std::string &block)
        {
            auto it = scope_of_.find(block);
            if (it == scope_of_.end())
                return;
            const ta::TrackingScopeRt &s = *it->second;

            std::lock_guard<std::mutex> lk(mu_);
            auto &slotmap = shadows_[block];
            for (const auto &p : s.plans)
            {
                ShadowSlots &vec = slotmap[p.name];
                vec.assign(static_cast<size_t>(s.slots), ShadowEntry{});
                for (auto &e : vec)
                    e.valid = true;
            }

            if (log_)
            {
                *log_ << "[Track] enter block=" << block
                    << " observables=" << s.observables.size()
                    << " slots=" << s.slots
                    << " window=[" << s.window_begin << "," << s.window_end << ")"
                    << " mode=" << static_cast<int>(s.mode) << std::endl;
                for (const auto &p : s.plans)
                    *log_ << "[Track]   obs=" << p.name
                        << " level=" << static_cast<int>(p.level)
                        << " extract=" << static_cast<int>(p.extract)
                        << " samples=" << p.samples << std::endl;
            }
        }

        void scope_exit(const std::string &block)
        {
            auto it = scope_of_.find(block);
            if (it == scope_of_.end())
                return;
            const ta::TrackingScopeRt &s = *it->second;

            bool ok = true;
            for (const auto &p : s.plans)
            {
                if (p.level == ta::TrackLevel::Shadow && p.samples <= 0)
                    ok = false;
                if (p.level != ta::TrackLevel::Relabel && p.extract == ta::ExtractKind::None)
                    ok = false;
                if (p.level == ta::TrackLevel::Qnd &&
                    ta::auto_level_of(p.name) != ta::TrackLevel::Qnd)
                    ok = false;

                const double threshold = (p.level == ta::TrackLevel::Shadow)
                                            ? ta::TrackingPlanner::shadow_fidelity_bound(p.samples)
                                            : 1.0;
                if (log_)
                    *log_ << "[Track]   settle obs=" << p.name
                        << " level=" << static_cast<int>(p.level)
                        << " required_fidelity>=" << threshold << std::endl;
            }

            {
                std::lock_guard<std::mutex> lk(mu_);
                shadows_.erase(block);
            }

            if (!ok)
            {
                ++stats_.invalid_scopes;
                if (log_)
                    *log_ << "[Track] scope INVALID block=" << block
                        << " —— 保真契约不满足，作用域失效（不伪造成功）" << std::endl;
            }
            else if (log_)
            {
                *log_ << "[Track] exit block=" << block << " (影子槽已释放)" << std::endl;
            }
        }

        void capture_source(const std::string &block)
        {
            std::lock_guard<std::mutex> lk(mu_);
            for (size_t i = 0; i < plans_.size(); ++i)
            {
                const ta::ShuttleLink &link = *plans_[i].link;
                if (link.from != block)
                    continue;
                auto src_it = shadows_.find(block);
                if (src_it == shadows_.end())
                {
                    continue;
                }
                auto &st = link_state_[i];
                for (const auto &kv : src_it->second)
                    st.payload[kv.first] = mean_of(kv.second);
                st.captured = true;
                ++stats_.captures;

                if (plans_[i].plan.strategy == ta::ShuttleStrategy::C001Point && log_)
                {
                    ++stats_.c001_points;
                    *log_ << "[Shuttle] c001 蝶点 digest from=" << block
                        << " observables=" << st.payload.size()
                        << " peers=" << link.peers.size()
                        << " rounds=" << plans_[i].plan.depth;
                    for (const auto &kv : st.payload)
                        *log_ << " " << kv.first << "=" << kv.second;
                    *log_ << " （全体关联层主要特征已压缩于此点，随后按 phase="
                        << static_cast<int>(plans_[i].plan.phase) << " 辐射）" << std::endl;
                }

                if (log_ && plans_[i].plan.strategy != ta::ShuttleStrategy::C001Point)
                    *log_ << "[Shuttle] capture from=" << block
                        << " observables=" << st.payload.size() << std::endl;
            }
        }

        void deliver_target(const std::string &block, bool pre)
        {
            std::lock_guard<std::mutex> lk(mu_);
            for (size_t i = 0; i < plans_.size(); ++i)
            {
                const ta::ShuttleLink &link = *plans_[i].link;
                const ta::ShuttlePlan &plan = plans_[i].plan;

                bool is_target = false;
                for (const auto &peer : link.peers)
                    if (peer.name == block) { is_target = true; break; }
                if (!is_target)
                    continue;

                const bool applies = (link.phase == ta::ShuttlePhase::Both) ||
                                    (pre && link.phase == ta::ShuttlePhase::Pre) ||
                                    (!pre && link.phase == ta::ShuttlePhase::Post);
                if (!applies)
                    continue;

                LinkState &st = link_state_[i];

                if (!pre)
                {
                    auto tgt_it = shadows_.find(block);
                    if (tgt_it != shadows_.end())
                    {
                        for (const auto &kv : tgt_it->second)
                            st.payload[kv.first] = mean_of(kv.second);
                        st.returned = true;
                        ++stats_.returns;
                        if (log_)
                            *log_ << "[Shuttle] return from=" << link.from
                                << " via=" << block
                                << " observables=" << tgt_it->second.size() << std::endl;
                    }
                    continue;
                }

                if (!st.captured)
                {
                    ++stats_.no_payload;
                    if (log_)
                        *log_ << "[Shuttle]   目标层入口 target=" << block
                            << " 载荷尚未捕获（源 " << link.from
                            << " 未产出经典影子）→ 跳过，不伪造成功" << std::endl;
                    continue;
                }

                auto tgt_it = shadows_.find(block);
                if (tgt_it == shadows_.end())
                {
                    stats_.no_target_shadow += plan.rounds.size();
                    if (log_)
                        *log_ << "[Shuttle]   目标 target=" << block
                            << " 无追踪作用域 → " << plan.rounds.size()
                            << " 轮渗透无处落地（docs §5 退化路径）" << std::endl;
                    continue;
                }

                size_t mixed = 0;
                for (const auto &round : plan.rounds)
                {
                    for (const auto &edge : round.edges)
                    {
                        if (edge.peer != block)
                            continue;
                        for (auto &pkv : tgt_it->second)
                        {
                            auto sit = st.payload.find(pkv.first);
                            const double contrib =
                                (sit != st.payload.end() ? sit->second : 0.0) * edge.weight;
                            for (auto &e : pkv.second)
                                if (e.valid)
                                    e.value += contrib;
                        }
                        ++mixed;
                    }
                }
                stats_.rounds_executed += plan.rounds.size();
                stats_.deliveries     += mixed;

                if (log_)
                    *log_ << "[Shuttle]   Pre 投递 target=" << block
                        << " from=" << link.from
                        << " rounds=" << plan.rounds.size()
                        << " 已混合边=" << mixed
                        << " (载荷观测数=" << st.payload.size() << ")" << std::endl;
            }
        }

        void layer_enter(const std::vector<std::string> &names)
        {
            for (const auto &n : names) scope_enter(n);
            for (const auto &n : names) deliver_target(n, true);
        }
        void layer_exit(const std::vector<std::string> &names)
        {
            for (const auto &n : names) deliver_target(n, false);
            for (const auto &n : names) capture_source(n);
            for (const auto &n : names) scope_exit(n);
        }

        const ta::ShuttlePlan *plan_of(const std::string &from) const
        {
            for (const auto &sp : plans_)
                if (sp.plan.source == from)
                    return &sp.plan;
            return nullptr;
        }
        const ta::ShuttleLink *link_of(const std::string &from) const
        {
            for (const auto &l : shuttles_)
                if (l.from == from)
                    return &l;
            return nullptr;
        }
        bool has_scope(const std::string &block) const { return scope_of_.count(block) != 0; }
        const ta::TrackingScopeRt *scope_of(const std::string &block) const
        {
            auto it = scope_of_.find(block);
            return it == scope_of_.end() ? nullptr : it->second;
        }

        bool shadow_all(const std::string &block, const std::string &obs,
                        std::vector<double> &out) const
        {
            std::lock_guard<std::mutex> lk(mu_);
            auto bit = shadows_.find(block);
            if (bit == shadows_.end())
                return false;
            auto oit = bit->second.find(obs);
            if (oit == bit->second.end())
                return false;
            out.clear();
            for (const auto &e : oit->second)
                out.push_back(e.value);
            return true;
        }
        bool shadow_first(const std::string &block, const std::string &obs, double &out) const
        {
            std::vector<double> v;
            if (!shadow_all(block, obs, v) || v.empty())
                return false;
            out = v.front();
            return true;
        }
        bool shadow_write(const std::string &block, const std::string &obs, double v)
        {
            std::lock_guard<std::mutex> lk(mu_);
            auto bit = shadows_.find(block);
            if (bit == shadows_.end())
                return false;
            auto oit = bit->second.find(obs);
            if (oit == bit->second.end())
                return false;
            for (auto &e : oit->second)
                if (e.valid)
                    e.value = v;
            return true;
        }
        bool shadow_forget(const std::string &block)
        {
            std::lock_guard<std::mutex> lk(mu_);
            return shadows_.erase(block) != 0;
        }
        bool payload_of(const std::string &from, const std::string &obs, double &out) const
        {
            std::lock_guard<std::mutex> lk(mu_);
            for (size_t i = 0; i < plans_.size(); ++i)
            {
                if (plans_[i].link->from != from)
                    continue;
                auto it = link_state_[i].payload.find(obs);
                if (it == link_state_[i].payload.end())
                    return false;
                out = it->second;
                return true;
            }
            return false;
        }
        bool captured_of(const std::string &from) const
        {
            std::lock_guard<std::mutex> lk(mu_);
            for (size_t i = 0; i < plans_.size(); ++i)
                if (plans_[i].link->from == from)
                    return link_state_[i].captured;
            return false;
        }

        size_t count_shuttles() const { return shuttles_.size(); }
        size_t count_scopes() const { return scopes_.size(); }
        const ShuttleRuntimeStats &stats() const { return stats_; }

        static bool is_underflow(const ta::ShuttlePeer &p)
        {
            return p.weight == 0.0 && std::isfinite(p.logWeight);
        }
        static double mean_of(const ShadowSlots &slots)
        {
            double acc = 0.0;
            size_t n = 0;
            for (const auto &e : slots)
                if (e.valid) { acc += e.value; ++n; }
            return n ? acc / static_cast<double>(n) : 0.0;
        }
        static std::string join_ints(const std::vector<int> &v)
        {
            std::string s;
            for (size_t i = 0; i < v.size(); ++i)
                s += (i ? "," : "") + std::to_string(v[i]);
            return s;
        }

    private:
        void parse_shuttles(const qhal::json::Value &doc)
        {
            shuttles_.clear();
            if (!doc.has("shuttleLinks") || !doc.at("shuttleLinks").is_array())
                return;
            for (const auto &lo : doc.at("shuttleLinks").array())
            {
                if (!lo.is_object() || !lo.has("from"))
                    continue;
                ta::ShuttleLink link;
                link.from     = lo.at("from").string();
                link.lambda   = lo.has("lambda") ? lo.at("lambda").number() : 1.0;
                link.phase    = lo.has("phase")
                                    ? static_cast<ta::ShuttlePhase>(lo.at("phase").int_value())
                                    : ta::ShuttlePhase::Both;
                link.rounds   = lo.has("rounds") ? lo.at("rounds").int_value() : 0;
                link.strategy = lo.has("strategy")
                                    ? static_cast<ta::ShuttleStrategy>(lo.at("strategy").int_value())
                                    : ta::ShuttleStrategy::Auto;

                if (lo.has("targets") && lo.at("targets").is_array())
                {
                    for (const auto &to : lo.at("targets").array())
                    {
                        if (!to.is_object() || !to.has("name"))
                            continue;
                        ta::ShuttlePeer p;
                        p.name     = to.at("name").string();
                        p.distance = to.has("distance") ? to.at("distance").int_value() : 0;
                        p.via      = to.has("via") ? to.at("via").string() : std::string("parallel");
                        if (to.has("coord") && to.at("coord").is_array())
                            for (const auto &cv : to.at("coord").array())
                                p.coord.push_back(cv.int_value());
                        if (to.has("logWeight"))
                            p.logWeight = to.at("logWeight").number();
                        else if (to.has("weight") && to.at("weight").number() > 0.0)
                            p.logWeight = std::log(to.at("weight").number());
                        p.weight = to.has("weight")
                                    ? to.at("weight").number()
                                    : (std::isfinite(p.logWeight) ? std::exp(p.logWeight) : 0.0);
                        link.peers.push_back(std::move(p));
                    }
                }
                if (!link.peers.empty())
                    shuttles_.push_back(std::move(link));
            }
        }

        void parse_scopes(const qhal::json::Value &doc)
        {
            scopes_.clear();
            scope_of_.clear();
            if (!doc.has("trackingScopes") || !doc.at("trackingScopes").is_array())
                return;
            for (const auto &so : doc.at("trackingScopes").array())
            {
                if (!so.is_object() || !so.has("block"))
                    continue;
                std::vector<std::string> obs;
                if (so.has("observables") && so.at("observables").is_array())
                    for (const auto &ov : so.at("observables").array())
                        obs.push_back(ov.string());
                const ta::TrackMode mode = so.has("mode")
                                            ? static_cast<ta::TrackMode>(so.at("mode").int_value())
                                            : ta::TrackMode::Auto;
                const int slots = so.has("slots") ? so.at("slots").int_value() : 1;
                int wb = 0, we = 1;
                if (so.has("window") && so.at("window").is_array() &&
                    so.at("window").array().size() >= 2)
                {
                    wb = so.at("window").array()[0].int_value();
                    we = so.at("window").array()[1].int_value();
                }
                auto planned = ta::TrackingPlanner::plan(so.at("block").string(), obs, mode,
                                                        slots, wb, we);
                for (auto &s : planned)
                    scopes_.push_back(std::move(s));
            }
            for (const auto &s : scopes_)
                scope_of_[s.block] = &s;
        }

        struct PlanEntry
        {
            ta::ShuttlePlan        plan;
            const ta::ShuttleLink *link = nullptr;
        };

        struct LinkState
        {
            std::map<std::string, double> payload;
            bool captured = false;
            bool returned = false;
        };

        std::ostream                                       *log_ = nullptr;
        std::vector<ta::ShuttleLink>                        shuttles_;
        std::vector<ta::TrackingScopeRt>                    scopes_;
        std::map<std::string, const ta::TrackingScopeRt *>  scope_of_;
        std::vector<PlanEntry>                              plans_;
        std::vector<LinkState>                              link_state_;
        mutable std::mutex                                  mu_;
        std::map<std::string, std::map<std::string, ShadowSlots>> shadows_;
        ShuttleRuntimeStats                                 stats_;
    };
}