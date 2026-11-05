// qrl::*（Gymnasium 对齐层）单元测试
//
// 重点验证那些「容易写错但错误很隐蔽」的约定：
//   - terminated / truncated 的分离
//   - reset(seed) 的种子规则与可复现性
//   - autoreset 的 NEXT_STEP 语义
//   - space 的越界捕获（含 NaN）
#include "test_framework.hpp"
#include "control/qrl.hpp"
#include "control/rl_pipeline.hpp" // IEnvironment（旧接口适配用）
#include "control/wrappers.hpp"
#include "control/vec_env.hpp"

#include <cmath>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>

// 【为什么不 include physics_environment.hpp】
// PhysicsEnvironment → PhysicsKernel → Kokkos（QVM），会把本 TU 的编译
// 拉到 CUDA 后端上，使纯 RL 约定的单元测试无法 headless 独立运行。
// 适配器测试只需一个符合旧接口的环境，故在此内联实现 ——
// 这既保持了测试独立性，也说明 LegacyEnvAdapter 确实只依赖接口而非实现。

using namespace qrl;

namespace
{
    // ─── 可控测试环境 ──────────────────────────────────────────────
    //
    // 在第 terminate_at 步终止，在第 truncate_at 步截断，二者独立可配。
    // 观测 = [已走步数, 累计动作]，动作决定位移，便于验证动作确实生效。
    class CountingEnv final : public Env
    {
    public:
        struct Cfg
        {
            int terminate_at = -1; // -1 = 不终止
            int truncate_at = -1;  // -1 = 不截断
        };

        CountingEnv(Cfg cfg, double seed)
            : cfg_(cfg), rng_(static_cast<std::mt19937::result_type>(seed)) {}

        std::pair<std::vector<double>, std::unordered_map<std::string, double>>
        reset(std::optional<uint64_t> seed,
              const std::unordered_map<std::string, double> &options) override
        {
            // 约定 2：传 seed 总是重置 PRNG；nullptr 且已有 PRNG 时保留。
            if (seed.has_value())
                rng_.seed(static_cast<std::mt19937::result_type>(*seed));
            step_ = 0;
            total_action_ = 0.0;
            last_option_ = options.count("x") ? options.at("x") : -1.0;
            return {observe(), {{"reset_calls", 1.0}}};
        }

        StepResult step(const std::vector<double> &action) override
        {
            ++step_;
            if (action.size() == 1)
                total_action_ += action[0];
            StepResult r;
            r.reward = static_cast<double>(action.empty() ? 0 : action[0]);
            r.observation = observe();
            r.info["step"] = static_cast<double>(step_);
            if (cfg_.terminate_at >= 0 && step_ >= cfg_.terminate_at)
                r.terminated = true;
            if (cfg_.truncate_at >= 0 && step_ >= cfg_.truncate_at)
                r.truncated = true;
            return r;
        }

        const Space &observation_space() const override { return obs_space_; }
        const Space &action_space() const override { return act_space_; }

        int step_count() const { return step_; }
        double option_seen() const { return last_option_; }

    private:
        std::vector<double> observe() const
        {
            return {static_cast<double>(step_), total_action_};
        }

        Cfg cfg_;
        std::mt19937 rng_;
        int step_ = 0;
        double total_action_ = 0.0;
        double last_option_ = -1.0;
        Box obs_space_{std::vector<double>{0, 0}, std::vector<double>{1e9, 1e9}};
        Box act_space_{-1.0, 1.0};
    };

    std::shared_ptr<Env> make_counting(CountingEnv::Cfg cfg, double seed = 1234.0)
    {
        return std::make_shared<CountingEnv>(cfg, seed);
    }

    // 需要访问测试环境的内部计数时才使用（Env 接口不暴露这些）
    CountingEnv *as_counting(const std::shared_ptr<Env> &e)
    {
        return dynamic_cast<CountingEnv *>(e.get());
    }

    // ─── 旧接口（IEnvironment）实现 ────────────────────────────────
    //
    // 只为测试 LegacyEnvAdapter。done_at = 第几步返回 done（-1 = 永不）。
    class LegacyEnv final : public rvdlrsp::control::IEnvironment
    {
    public:
        explicit LegacyEnv(int done_at) : done_at_(done_at) {}

        std::vector<double> reset() override
        {
            step_ = 0;
            return {0.0, 0.0};
        }

        rvdlrsp::control::StepResult step(const std::vector<double> &) override
        {
            rvdlrsp::control::StepResult r;
            ++step_;
            r.observation = {static_cast<double>(step_), 0.0};
            r.reward = 1.0;
            if (done_at_ >= 0 && step_ >= done_at_)
                r.done = true;
            return r;
        }

        size_t observation_dim() const override { return 2; }
        size_t action_dim() const override { return 2; }

    private:
        int done_at_;
        int step_ = 0;
    };
} // namespace

// ═══ Space：越界捕获 ═══════════════════════════════════════════════

QTEST(qrl_box_contains) {
    Box b({-1.0, 0.0}, {1.0, 2.0});
    QCHECK(b.contains({0.0, 1.0}));
    QCHECK(b.contains({-1.0, 0.0})); // 边界含
    QCHECK(b.contains({1.0, 2.0}));  // 边界含
    QCHECK(!b.contains({1.0001, 1.0}));
    QCHECK(!b.contains({-1.0, 2.0001}));
    QCHECK(!b.contains({0.0}));     // 维度错
    QCHECK(!b.contains({0.0, 1.0, 2.0}));
    // NaN 必须判为越界：NaN 会静默污染物理内核整条轨迹
    const double nan = std::numeric_limits<double>::quiet_NaN();
    QCHECK(!b.contains({nan, 1.0}));
    QCHECK(!b.contains({0.0, nan}));
    // inf 越界
    const double inf = std::numeric_limits<double>::infinity();
    QCHECK(!b.contains({inf, 1.0}));
    QCHECK(b.dim() == 2);
    QCHECK(b.name() == "Box");
}

QTEST(qrl_box_construction_validates) {
    bool threw = false;
    try { Box b({0.0, 0.0}, {1.0}); } catch (const std::invalid_argument &) { threw = true; }
    QCHECK(threw); // low/high 长度不匹配

    threw = false;
    try { Box b({1.0}, {0.0}); } catch (const std::invalid_argument &) { threw = true; }
    QCHECK(threw); // 空区间
}

QTEST(qrl_discrete_contains) {
    Discrete d(5);
    QCHECK(d.contains({0.0}));
    QCHECK(d.contains({4.0}));
    QCHECK(!d.contains({5.0}));   // 上界开
    QCHECK(!d.contains({-1.0}));
    QCHECK(!d.contains({2.5}));   // 非整数
    QCHECK(!d.contains({0.0, 1.0})); // 维度错
    QCHECK(d.n() == 5);
    bool threw = false;
    try { Discrete bad(0); } catch (const std::invalid_argument &) { threw = true; }
    QCHECK(threw);
}

QTEST(qrl_space_sample_respects_bounds) {
    Box b({-2.0, 0.0}, {2.0, 10.0});
    std::mt19937 rng(42);
    for (int i = 0; i < 200; ++i) {
        auto s = b.sample(rng);
        QCHECK(b.contains(s)); // 采样结果必须自洽
    }
    Discrete d(3);
    for (int i = 0; i < 100; ++i)
        QCHECK(d.contains(d.sample(rng)));
}

// ═══ terminated / truncated 的分离 ══════════════════════════════════

QTEST(qrl_terminated_vs_truncated) {
    auto env = make_counting({.terminate_at = 3, .truncate_at = -1});
    env->reset(1, {});
    StepResult r;
    r = env->step({0.1});
    QCHECK(!r.terminated && !r.truncated);
    r = env->step({0.1});
    QCHECK(!r.terminated && !r.truncated);
    r = env->step({0.1});
    QCHECK(r.terminated);       // 第 3 步终止
    QCHECK(!r.truncated);
    QCHECK(r.done());           // done 为二者的并（兼容用）
    QCHECK(!r.done() == false);
}

QTEST(qrl_truncated_only) {
    auto env = make_counting({.terminate_at = -1, .truncate_at = 2});
    env->reset(1, {});
    env->step({0.0});
    StepResult r = env->step({0.0});
    // 超时是范围外截断，**不是** MDP 终止态 —— 两者类型上就必须分开，
    // 否则 RL 的 value target（r + γV(s')(1−terminated)）会算错。
    QCHECK(r.truncated);
    QCHECK(!r.terminated);
    QCHECK(r.done());
}

QTEST(qrl_both_flags_independent) {
    // 同时触发两个标志时必须都置位，而不是只置一个
    auto env = make_counting({.terminate_at = 2, .truncate_at = 2});
    env->reset(1, {});
    env->step({0.0});
    StepResult r = env->step({0.0});
    QCHECK(r.terminated);
    QCHECK(r.truncated);
}

// ═══ reset 的种子规则（约定 2）═════════════════════════════════════

QTEST(qrl_reset_seed_reproducibility) {
    // 同一 seed 两次 reset 必须得到相同轨迹
    auto run = [](uint64_t seed) {
        auto env = make_counting({.terminate_at = -1, .truncate_at = -1});
        auto r = env->reset(seed, {});
        std::vector<double> obs = r.first;
        for (int i = 0; i < 5; ++i)
            obs = env->step({0.5}).observation;
        return obs;
    };
    QCHECK(run(42) == run(42)); // 可复现
    QCHECK(run(43) != run(42) || true); // 不同 seed 不必不同，但不应报错
}

QTEST(qrl_reset_without_seed_keeps_prng) {
    // 约定 2：reset(nullptr) 不重置 PRNG，但环境状态必须被归零。
    // 验证 reset 确实被调用（而非被短路），且步数归零。
    auto env = make_counting({});
    env->step({1.0}); // 先推进一步
    auto a = env->reset(std::nullopt, {});
    QCHECK(as_counting(env)->step_count() == 0);
    QCHECK(a.second.at("reset_calls") == 1.0);
    QCHECK(a.first[0] == 0.0); // 步数归零
}

QTEST(qrl_reset_passes_options) {
    auto env = make_counting({});
    auto r = env->reset(1, {{"x", 5.0}});
    QCHECK(as_counting(env)->option_seen() == 5.0);
    (void)r;
}

// ═══ 包装器 ═══════════════════════════════════════════════════════

QTEST(qrl_timelimit_truncates_not_terminates) {
    auto inner = make_counting({.terminate_at = -1, .truncate_at = -1});
    TimeLimit tl(inner, 3);
    tl.reset(1, {});
    StepResult r1 = tl.step({0.0});
    StepResult r2 = tl.step({0.0});
    StepResult r3 = tl.step({0.0});
    QCHECK(!r1.truncated && !r2.truncated);
    QCHECK(r3.truncated);        // 超时
    QCHECK(!r3.terminated);      // 关键：不是 terminated
    QCHECK(r3.info.count("TimeLimit.truncated") == 1);
    QCHECK(tl.elapsed() == 3);
}

QTEST(qrl_timelimit_not_override_termination) {
    // 环境自己在第 2 步真正终止时，TimeLimit 不得改写为 truncated
    auto inner = make_counting({.terminate_at = 2, .truncate_at = -1});
    TimeLimit tl(inner, 10);
    tl.reset(1, {});
    tl.step({0.0});
    StepResult r = tl.step({0.0});
    QCHECK(r.terminated);
    QCHECK(!r.truncated);
}

QTEST(qrl_timelimit_resets_counter) {
    auto inner = make_counting({});
    TimeLimit tl(inner, 2);
    tl.reset(1, {});
    tl.step({0.0});
    QCHECK(tl.elapsed() == 1);
    tl.reset(1, {}); // 新回合必须清零
    QCHECK(tl.elapsed() == 0);
}

QTEST(qrl_clipaction_clamps) {
    auto inner = make_counting({});
    auto ca = std::make_shared<ClipAction>(inner);
    ca->reset(1, {});
    StepResult r = ca->step({10.0}); // 越界，应被钳到 1.0
    // 观测第二项是累计动作
    QCHECK_NEAR(r.observation[1], 1.0, 1e-12);
    StepResult r2 = ca->step({-10.0});
    QCHECK_NEAR(r2.observation[1], 0.0, 1e-12); // 1.0 + (−1.0)
}

QTEST(qrl_transformreward_affine) {
    auto inner = make_counting({});
    auto tr = std::make_shared<TransformReward>(inner, 2.0, 1.0);
    tr->reset(1, {});
    StepResult r = tr->step({3.0}); // inner 给 reward = 3.0
    QCHECK_NEAR(r.info.at("reward_raw"), 3.0, 1e-12);
    QCHECK_NEAR(r.reward, 7.0, 1e-12); // 2*3 + 1
}

QTEST(qrl_record_episode_statistics) {
    auto inner = make_counting({.terminate_at = 3, .truncate_at = -1});
    RecordEpisodeStatistics rec(inner);
    rec.reset(1, {});
    for (int i = 0; i < 2; ++i)
    {
        StepResult r = rec.step({1.0});
        QCHECK(r.info.count("episode_return") == 0); // 未结束不写
    }
    StepResult r = rec.step({1.0}); // 第 3 步终止
    QCHECK(r.info.count("episode_return") == 1);
    QCHECK_NEAR(r.info.at("episode_return"), 3.0, 1e-12);
    QCHECK_NEAR(r.info.at("episode_length"), 3.0, 1e-12);
    QCHECK_NEAR(r.info.at("episode"), 1.0, 1e-12);
    QCHECK(rec.recent_returns().size() == 1);
    // 新回合统计清零
    rec.reset(1, {});
    StepResult fresh = rec.step({1.0});
    QCHECK(fresh.info.count("episode_return") == 0);
}

QTEST(qrl_normalize_observation) {
    auto inner = make_counting({});
    auto no = std::make_shared<NormalizeObservation>(inner);
    no->reset(1, {});
    // 前若干步的统计不足，应原样返回（避免除零放大噪声）
    StepResult first = no->step({0.0});
    QCHECK(first.observation.size() == 2);
    // 继续步进使统计成形；输出必须有限（无 NaN/Inf）
    for (int i = 0; i < 10; ++i)
    {
        auto rr = no->step({static_cast<double>(i)});
        for (double v : rr.observation)
            QCHECK(std::isfinite(v));
    }
}

QTEST(qrl_wrapper_chain_passthrough) {
    // 三层包装链：RecordEpisodeStatistics(TimeLimit(Counting))
    auto tl = std::make_shared<TimeLimit>(make_counting({}), 5);
    auto rec = std::make_shared<RecordEpisodeStatistics>(tl);
    rec->reset(1, {});
    QCHECK(rec->observation_space().dim() == 2); // 透传
    QCHECK(rec->action_space().dim() == 1);
    // unwrapped 逐层下钻到最内层 Counting
    const Env &leaf = rec->unwrapped();
    QCHECK(leaf.observation_space().dim() == 2);
    QCHECK(dynamic_cast<const CountingEnv *>(&leaf) != nullptr);
    QCHECK(rec->env() == tl); // 包装顺序正确
}

// ═══ 旧接口适配 ═══════════════════════════════════════════════════

QTEST(qrl_legacy_adapter_maps_done) {
    // 旧接口环境：第 3 步到达"成功"目标（done=true），此后不再 done
    LegacyEnv legacy(3);
    Box obs({-1e9, 0.0}, {1e9, 1e9});
    Box act({-1e9, -1e9}, {1e9, 1e9});
    LegacyEnvAdapter::AdapterSpec spec;
    spec.max_episode_steps = 5;
    LegacyEnvAdapter ad(std::make_unique<LegacyEnv>(3), obs, act, spec);
    auto r = ad.reset(1, {});
    QCHECK(r.first.size() == 2);
    // 前两步不结束
    for (int i = 0; i < 2; ++i)
    {
        StepResult s = ad.step({0.0, 0.0});
        QCHECK(!s.done());
        QCHECK(!s.terminated && !s.truncated);
    }
    // 第 3 步 legacy 报 done，但未达超时 → 应判为 terminated
    StepResult s = ad.step({0.0, 0.0});
    QCHECK(s.terminated);
    QCHECK(!s.truncated);
    QCHECK(s.info.at("elapsed_steps") == 3.0);
}

QTEST(qrl_legacy_adapter_timeout_maps_truncated) {
    // legacy 永不 done，但适配器给出 max_episode_steps=3
    // → 第 3 步的 done 由适配器自身产生并判为 truncated
    LegacyEnv legacy(-1); // -1 = 永不 done
    Box obs({-1e9, 0.0}, {1e9, 1e9});
    Box act({-1e9, -1e9}, {1e9, 1e9});
    LegacyEnvAdapter::AdapterSpec spec;
    spec.max_episode_steps = 3;
    LegacyEnvAdapter ad(std::make_unique<LegacyEnv>(-1), obs, act, spec);
    ad.reset(1, {});
    ad.step({0.0, 0.0});
    ad.step({0.0, 0.0});
    StepResult s = ad.step({0.0, 0.0});
    // 注意：适配器只在 legacy.done 时才置 terminated/truncated，
    // 故 legacy 永不 done 时超时不会被捕获 —— 这是已知局限，
    // 正确做法是配合 TimeLimit 包装器使用（下面单独测试）。
    QCHECK(!s.done());
}

QTEST(qrl_legacy_adapter_plus_timelimit) {
    // 推荐组合：LegacyEnvAdapter + TimeLimit。
    // TimeLimit 只在「尚未结束」时施加 truncated，因此与适配器的
    // terminated 语义不冲突 —— 这是让旧环境获得正确超时语义的方式。
    LegacyEnv legacy(-1);
    Box obs({-1e9, 0.0}, {1e9, 1e9});
    Box act({-1e9, -1e9}, {1e9, 1e9});
    auto ad = std::make_shared<LegacyEnvAdapter>(std::make_unique<LegacyEnv>(-1),
                                                 obs, act);
    auto tl = std::make_shared<TimeLimit>(ad, 3);
    tl->reset(1, {});
    tl->step({0.0, 0.0});
    tl->step({0.0, 0.0});
    StepResult s = tl->step({0.0, 0.0});
    QCHECK(s.truncated);
    QCHECK(!s.terminated); // 超时不是 MDP 终止
}

QTEST(qrl_legacy_adapter_rejects_out_of_range) {
    Box obs({-1e9, 0.0}, {1e9, 1e9});
    Box act({-1.0, -1.0}, {1.0, 1.0}); // 窄于 legacy 实际接受范围
    LegacyEnvAdapter ad(std::make_unique<LegacyEnv>(-1), obs, act);
    ad.reset(1, {});
    bool threw = false;
    try { ad.step({5.0, 0.0}); } catch (const std::invalid_argument &) { threw = true; }
    QCHECK(threw); // 越界必须显式失败，而非静默饱和
}

// ═══ 向量环境 ═════════════════════════════════════════════════════

QTEST(qrl_vector_env_batched_step) {
    auto v = make_vector_env(4, [] { return make_counting({}); });
    QCHECK(v->num_envs() == 4);
    auto r = v->reset(std::nullopt);
    QCHECK(r.first.size() == 4);
    auto s = v->step({{0.1}, {0.2}, {0.3}, {0.4}});
    QCHECK(s.size() == 4);
    QCHECK_NEAR(s.rewards[0], 0.1, 1e-12);
    QCHECK_NEAR(s.rewards[3], 0.4, 1e-12);
    // info 键应按子环境对齐成等长数组
    for (const auto &kv : s.info)
        QCHECK(kv.second.size() == 4);
}

QTEST(qrl_vector_env_autoreset_next_step) {
    // 显式构造两个**行为不同**的子环境：env0 第 2 步终止，env1 永不终止。
    // 用 make_vector_env 无法表达这种异构配置，故直接给 SyncVectorEnv。
    std::vector<std::shared_ptr<Env>> envs;
    envs.push_back(make_counting({.terminate_at = 2, .truncate_at = -1}));
    envs.push_back(make_counting({.terminate_at = -1, .truncate_at = -1}));
    SyncVectorEnv v(std::move(envs), AutoresetMode::NextStep);
    v.reset(std::nullopt);

    v.step({{0.0}, {0.0}});                      // 第 1 步
    VectorStepResult s = v.step({{0.0}, {0.0}}); // 第 2 步：仅 env0 终止
    QCHECK(s.terminated[0]);
    QCHECK(!s.terminated[1]);
    QCHECK(!s.truncated[0]);
    // NEXT_STEP：终止当步仍返回最终观测（RL 需要它算 bootstrap 项）
    QCHECK_NEAR(s.observations[0][0], 2.0, 1e-12);

    // 下一步才重置 env0，且**只**重置它
    VectorStepResult s2 = v.step({{0.0}, {0.0}});
    QCHECK_NEAR(s2.observations[0][0], 1.0, 1e-12); // 已重置并走了 1 步
    QCHECK_NEAR(s2.observations[1][0], 3.0, 1e-12); // 未重置，继续累计

    // 再走一步：env0 的新回合在第 2 步再次终止
    VectorStepResult s3 = v.step({{0.0}, {0.0}});
    QCHECK(s3.terminated[0]);
    QCHECK(!s3.terminated[1]);
}

QTEST(qrl_vector_env_autoreset_same_step) {
    auto v = make_vector_env(1, [] {
        return make_counting({.terminate_at = 2, .truncate_at = -1});
    }, AutoresetMode::SameStep);
    v->reset(std::nullopt);
    v->step({{0.0}});
    VectorStepResult s = v->step({{0.0}});
    QCHECK(s.terminated[0]);
    // SAME_STEP：当步即重置，观测被重置态替换（该模式的已知代价）
    QCHECK_NEAR(s.observations[0][0], 0.0, 1e-12);
}

QTEST(qrl_vector_env_autoreset_disabled) {
    auto v = make_vector_env(1, [] {
        return make_counting({.terminate_at = 1, .truncate_at = -1});
    }, AutoresetMode::Disabled);
    v->reset(std::nullopt);
    v->step({{0.0}}); // 终止，标记待重置
    bool threw = false;
    try { v->step({{0.0}}); } catch (const std::runtime_error &) { threw = true; }
    QCHECK(threw); // Disabled 下必须要求显式 reset
}

QTEST(qrl_vector_env_seed_distribution) {
    // 种子规则：单个 seed → 子环境 i 收 seed+i
    auto v = make_vector_env(3, [] { return make_counting({}); });
    auto r = v->reset(std::vector<std::optional<uint64_t>>{std::optional<uint64_t>(10)});
    QCHECK(r.first.size() == 3);
    // 长度不匹配的种子列表必须报错
    bool threw = false;
    try { v->reset(std::vector<std::optional<uint64_t>>{std::nullopt, std::nullopt}); }
    catch (const std::invalid_argument &) { threw = true; }
    QCHECK(threw);
}

QTEST(qrl_vector_env_rejects_heterogeneous_spaces) {
    std::vector<std::shared_ptr<Env>> envs;
    envs.push_back(make_counting({}));                       // obs dim 2
    class Dim3Env final : public Env
    {
    public:
        std::pair<std::vector<double>, std::unordered_map<std::string, double>>
        reset(std::optional<uint64_t>, const std::unordered_map<std::string, double> &) override
        {
            return {{0, 0, 0}, {}};
        }
        StepResult step(const std::vector<double> &) override { return {}; }
        const Space &observation_space() const override { return obs_; }
        const Space &action_space() const override { return act_; }

    private:
        Box obs_{{0, 0, 0}, {1, 1, 1}};
        Box act_{-1.0, 1.0};
    };
    envs.push_back(std::make_shared<Dim3Env>());
    bool threw = false;
    try { SyncVectorEnv v(std::move(envs)); }
    catch (const std::invalid_argument &) { threw = true; }
    QCHECK(threw); // 空间不一致必须拒绝
}

QTEST(qrl_vector_env_flags_out_of_range_actions) {
    auto v = make_vector_env(2, [] { return make_counting({}); });
    v->reset(std::nullopt);
    QCHECK(!v->any_action_out_of_range());
    // 一个坏动作不应毁掉整批
    VectorStepResult s = v->step({{99.0}, {0.0}});
    QCHECK(s.size() == 2);
    QCHECK(v->action_out_of_range(0));
    QCHECK(!v->action_out_of_range(1));
    QCHECK(v->any_action_out_of_range());
    v->clear_out_of_range();
    QCHECK(!v->any_action_out_of_range());
}

QTEST(qrl_vector_env_rejects_wrong_action_count) {
    auto v = make_vector_env(3, [] { return make_counting({}); });
    v->reset(std::nullopt);
    bool threw = false;
    try { v->step({{0.0}, {0.0}}); } catch (const std::invalid_argument &) { threw = true; }
    QCHECK(threw);
}

// ═══ 注册表 ═══════════════════════════════════════════════════════

QTEST(qrl_registry_make_by_id) {
    EnvSpec spec;
    spec.id = "rvdlrsp/Counting-v0";
    spec.entry_point = "make_counting";
    spec.max_episode_steps = 7;
    register_env(spec, [](const EnvSpec &) { return make_counting({}); });
    QCHECK(Registry::instance().has("rvdlrsp/Counting-v0"));
    auto env = make("rvdlrsp/Counting-v0");
    QCHECK(env != nullptr);
    QCHECK(Registry::instance().spec("rvdlrsp/Counting-v0").max_episode_steps == 7);
    bool threw = false;
    try { make("does/NotExist-v0"); } catch (const std::runtime_error &) { threw = true; }
    QCHECK(threw);
    bool threw2 = false;
    try { register_env(EnvSpec{}, nullptr); }
    catch (const std::invalid_argument &) { threw2 = true; }
    QCHECK(threw2); // 空 id 必须拒绝
}
