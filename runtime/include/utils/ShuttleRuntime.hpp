#pragma once
// ============================================================================
// ShuttleRuntime.hpp —— 「穿梭」与「追踪」的运行期执行体
//
// 服务端 TopologyBuilder 产出调度表的 `shuttleLinks` / `trackingScopes` 两段
// （docs/rvdl-shuttle-tracking.md §8），本节把它们变成**真实的执行效果**。
//
// ── 说明下这段逻辑住在头文件里而不是 runtime_api.cpp 内 ────────────────────
// 它必须能被**独立编译并对编译器真实产出的调度表运行**。否则
// 「权重是否按 logWeight 路由」「下溢对端是否仍被调度」「影子槽是否按周期释放」
// 这三条只能靠人工阅读确认 —— 而它们恰恰是最容易静默出错的地方。
// 参见 runtime/tests/shuttle_tracking_runtime.cpp。
//
// ── 三条纪律 ────────────────────────────────────────────────────────────────
//   ① 路由一律用 logWeight（−d/λ，任意距离有限），weight 仅用于线性混合。
//      d/λ ≳ 746 时线性权重下溢为 0，但对端**仍被调度、仍被记录** ——
//      与「被阈值截断」不可混淆。
//   ② 影子槽只承载经典标量。这是**结构**而非约定：见 ShadowEntry 的 static_assert。
//   ③ 任何降级 / 跳过 / 失效都写入日志，绝不静默伪造成功。
//
// ============================================================================
// 相位语义（docs §3.2）—— 关键：轮次在**目标层**的 time-slot 边界执行
//
// 设链接 a → 𝕊(a)，共享点 = 目标层的 time-slot。则：
//
//   phase=0（仅共享前）目标 b 的层**入口**：把 a 的渗透载荷写入 b 的影子，
//                      b 因此把该关联当作**输入**看到。
//   phase=1（仅共享后）目标 b 的层**出口**：把 b 的影子回灌进该链接的载荷，
//                      形成双侧恢复的返回腿。
//   phase=2（前后都渗透）两者都做 → **每个关联层同时是源与汇**，
//                      覆盖面严格大于单侧。
//
// 为什么需要「渗透载荷」这个中间通道（而不是直接读源的影子）：
// 影子槽的生命周期 = 块自身的 [time, time+cost) 窗口（§4.4），
// 而源与目标的窗口通常**不相交**（如 sense@t0 → relay@t1）。
// 若在目标的边界直接读源的影子，窗口早已关闭 —— 机制要么失效，
// 要么必须让影子越界存活（那就违反了 §4.4 的「不构成持久克隆」）。
// 渗透载荷正是「渗透」二字的载体：它是跨窗口传递的**经典关联标量**
// （不是量子态的副本，因此不触碰不可克隆定理），随链接在运行期存续。
// ============================================================================

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
    // ─── 影子槽：只承载经典标量 ─────────────────────────────────────────────────
    // 影子槽**绝不**是 QObject。这是 borrow.ts 的 E-Q001（no-cloning）/
    // E-Q002（线性值泄漏）在运行期的对应物：「被测量的是影子，不是原态」，
    // 与子系统码「规范量子比特被测量、逻辑量子比特存活」同构。
    struct ShadowEntry
    {
        double value = 0.0;
        bool   valid = false;
    };

    // QLT 不变量断言：影子槽必须是平凡可复制、标准布局、非多态的经典 POD 载体。
    // 若日后有人把 ShadowEntry 改成持有 QObject（非平凡类型），此断言立刻失败 ——
    // 那是 E-Q001/E-Q002 被破坏的第一个信号。
    static_assert(std::is_trivially_copyable<ShadowEntry>::value &&
                    std::is_standard_layout<ShadowEntry>::value &&
                    !std::is_polymorphic<ShadowEntry>::value,
                "影子槽必须只承载经典标量 —— 引入 QObject 即刻违反 E-Q001/E-Q002");

    using ShadowSlots = std::vector<ShadowEntry>;

    struct ShuttleRuntimeStats
    {
        size_t shuttles         = 0;  // 解析出的穿梭链接数
        size_t scopes           = 0;  // 解析出的追踪作用域数
        size_t underflow_peers  = 0;  // 线性权重下溢但仍被调度的对端数
        size_t degraded         = 0;  // Grover → 蝶式的降级次数（W-TRK001）
        size_t c001_points      = 0;  // c001 蝶点显式物化的摘要数（「压缩成点」的落地计数）
        size_t w_shu001         = 0;  // 无 @track 却请求相干穿梭的次数
        size_t captures         = 0;  // 源端载荷捕获次数
        size_t deliveries       = 0;  // 目标端成功投递的边数
        size_t no_target_shadow = 0;  // 目标无影子可接收而跳过的边数
        size_t no_payload       = 0;  // 目标边界上载荷尚未捕获而跳过的次数
        size_t returns          = 0;  // phase=1/2 的返回腿次数
        size_t invalid_scopes   = 0;  // 保真契约不满足的作用域数
        size_t rounds_executed  = 0;
    };

    class ShuttleRuntime
    {
    public:
        explicit ShuttleRuntime(std::ostream *log = &std::cout) : log_(log) {}

        // ── 解析调度表的两段 ────────────────────────────────────────────────────
        // 旧表缺段 → 两个容器保持为空 → 全部方法成为惰性空操作。
        void parse(const qhal::json::Value &doc)
        {
            parse_shuttles(doc);
            parse_scopes(doc);
            plans_.clear();
            link_state_.clear();
            stats_.shuttles = shuttles_.size();
            stats_.scopes   = scopes_.size();
        }

        // ── 轮次规划（auto 决策落盘到诊断，不得静默）───────────────────────────
        // coord_span = ∏(coord_max[i]+1)；ancilla_bits < 0 表示「未声明可用」。
        void plan_all(int ancilla_bits, long long coord_span)
        {
            plans_.clear();
            link_state_.clear();
            for (const auto &link : shuttles_)
            {
                const bool has_track = scope_of_.count(link.from) != 0;

                ta::ShuttlePlan p = ta::ShuttlePlanner::plan(link, ta::PayloadKind::Classical,
                                                            /*payload_is_large=*/false,
                                                            ancilla_bits, coord_span);

                // 显式 strategy=3 但 ancilla 不足 → 降级并记录，不静默失败
                if (p.strategy == ta::ShuttleStrategy::Grover &&
                    !ta::ShuttlePlanner::grover_affordable(coord_span, ancilla_bits))
                {
                    // 降级必须**重规划**，不能只改标签：Grover 的轮次是 1，
                    // 而蝶式需要 ⌈log₂span⌉ 轮。只改标签会留下
                    //「标签写着蝶式、计划里只有 1 轮」的静默不一致 ——
                    // 1 轮蝶式只翻 coord 的第 0 位，绝大多数格点收不到渗透，
                    // 恰是本机制最要命的静默失败。
                    ta::ShuttleLink fallen = link;
                    fallen.strategy = ta::ShuttleStrategy::Butterfly;
                    fallen.rounds   = 0;         // 交给蝶式自己的解析式重新定轮次
                    ta::ShuttlePlan q = ta::ShuttlePlanner::plan(
                        fallen, ta::PayloadKind::Classical, /*payload_is_large=*/false,
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

        // ── 追踪：作用域进入（EXTRACT → COPY）──────────────────────────────────
        // 调度器层没有块内 QObject 的句柄，故 ① EXTRACT 与 ③ RESTORE 由块自身经
        // ABI 完成（编译器已插入抽取/复原电路）；**② COPY 是此处真实执行的部分**：
        // 为每个可观测量分配 track_slots 份经典影子槽。
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

        // ── 追踪：作用域退出（RESTORE 校验 → 释放）─────────────────────────────
        void scope_exit(const std::string &block)
        {
            auto it = scope_of_.find(block);
            if (it == scope_of_.end())
                return;
            const ta::TrackingScopeRt &s = *it->second;

            bool ok = true;
            for (const auto &p : s.plans)
            {
                // L1（shadow）无采样数即无法构成经典阴影 —— 真实可检的违约
                if (p.level == ta::TrackLevel::Shadow && p.samples <= 0)
                    ok = false;
                // 非 L2 却选择「不抽取」→ 拿不到值
                if (p.level != ta::TrackLevel::Relabel && p.extract == ta::ExtractKind::None)
                    ok = false;
                // L0（qnd）只对 Pauli 族存在实现：非 Pauli 族不存在非破坏读出电路。
                // 编译期已由 E-TRK003 拦截；此处是运行期最后一道防线（仿佛数据 / 降级表）。
                if (p.level == ta::TrackLevel::Qnd &&
                    ta::auto_level_of(p.name) != ta::TrackLevel::Qnd)
                    ok = false;

                // RESTORE 后的保真门限：L0/L2 结构上精确；L1 为统计级采样收敛下界。
                // 逐态校验由块经 qstate_fidelity 完成，此处落定**契约门限**。
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
                // 影子槽为块栈帧局部：周期结束即释放 → 不构成持久克隆（docs §4.4）
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

        // ── 穿梭 ①：源端捕获 ───────────────────────────────────────────────────
        // 在源的层出口（其功能周期末尾）把影子快照进该链接的渗透载荷。
        // 载荷是**经典关联标量**，不是量子态副本 —— 不触碰不可克隆定理。
        //
        // c001 蝶点（strategy=4）在此显式物化：载荷即**蝶点摘要**（butterfly-point
        // digest）—— 全体关联层的主要特征在这一点上有序集合，随后由 deliver_target
        // 按相位向全体辐射。这就是「压缩成点（蝶点，取主要特征）」的运行期落地：
        // 蝶式要 ⌈log₂span⌉ 轮逐位翻转才能让远端对端收到数据，蝶点只需这一个点。
        // 摘要写入诊断日志（决策与内容可见，不静默）。
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
                    // 源无影子（未声明 @track / 在作用域外）→ 载荷保持为空，投递时诚实跳过
                    continue;
                }
                auto &st = link_state_[i];
                for (const auto &kv : src_it->second)
                    st.payload[kv.first] = mean_of(kv.second);
                st.captured = true;
                ++stats_.captures;

                // c001 蝶点：显式物化摘要并打印（「压缩成点」的可见性）
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

        // ── 穿梭 ②：目标端投递 / 返回腿 ────────────────────────────────────────
        // pre  = 目标层**入口**：载荷 → 目标影子（目标把关联当输入看到）
        // post = 目标层**出口**：目标影子 → 载荷的返回腿（双侧恢复）
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
                    // 返回腿：目标（作为汇）的影子回灌进载荷 —— 双侧恢复的「另一侧」
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

                // 前向投递
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
                    // 目标无影子可接收（docs §5 退化路径）—— 逐边计数
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
                        // 只投递给名字匹配的边缘（一轮内可能含多个对端）
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

        // ── 便捷编排：整个调度周期 ──────────────────────────────────────────────
        // 调用方按 time 层驱动：
        //   入口 = 开影子 → pre 投递 → 执行块；出口 = post 返回腿 → 捕获源 → 关影子
        void layer_enter(const std::vector<std::string> &names)
        {
            for (const auto &n : names) scope_enter(n);
            for (const auto &n : names) deliver_target(n, /*pre=*/true);
        }
        void layer_exit(const std::vector<std::string> &names)
        {
            for (const auto &n : names) deliver_target(n, /*pre=*/false);
            for (const auto &n : names) capture_source(n);
            for (const auto &n : names) scope_exit(n);
        }

        // ── 观测接口（诊断 / 测试）─────────────────────────────────────────────
        const ta::ShuttlePlan *plan_of(const std::string &from) const
        {
            for (const auto &sp : plans_)
                if (sp.plan.source == from)
                    return &sp.plan;
            return nullptr;
        }
        const ta::ShuttleLink *link_of(const std::string &from) const
        {
            // 查**已解析**的链接（不依赖是否已规划）——链接是调度表的事实，
            // 规划是其派生。若查 plans_，调用方在 plan_all() 之前就拿不到链接。
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
        /** 直接注入影子值（块经 ABI 写入时使用）*/
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
        /** 取某链接渗透载荷的当前值（经典关联标量）*/
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
                        // logWeight 为权威路由量；只发 weight 的旧表由 log 反推
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

        /** 一条链接的渗透载荷（跨窗口传递的经典关联标量）*/
        struct LinkState
        {
            std::map<std::string, double> payload;   // 可观测量 → 关联值
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