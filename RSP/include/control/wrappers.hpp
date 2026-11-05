#pragma once
//
// Gymnasium 对齐层 —— 包装器与向量环境
//
// 拆成两个头文件以便按需包含：
//   wrappers.hpp  标量环境包装器（观测/动作/奖励变换、时间上限、统计）
//   vec_env.hpp   向量环境（批量 reset/step，autoreset 语义）
//
// 与既有 RL 组件的衔接：本目录的 wrappers.hpp 同时提供
// LegacyEnvAdapter，把旧的 IEnvironment（reset→obs、step→StepResult{done}）
// 适配为 qrl::Env，从而**不破坏**现有 PhysicsEnvironment /
// TrackingEnvironment / rl_demo 的可用性。
//
#include "control/qrl.hpp"

#include <algorithm>
#include <cmath>
#include <deque>
#include <memory>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "control/rl_pipeline.hpp" // IEnvironment（适配器需要）

namespace qrl
{

    // ─── 包装器基类 ────────────────────────────────────────────────
    //
    // 对齐 Gymnasium 的 Wrapper 语义：持有 env，透传空间与 reset/step，
    // 并把自身纳入 wrapper 链（unwrapped 给出最内层环境）。
    class Wrapper : public Env
    {
    public:
        explicit Wrapper(std::shared_ptr<Env> env) : env_(std::move(env)) {}

        // 默认转发：未重写 reset 的包装器（ClipAction 等）由此保持可用。
        std::pair<std::vector<double>, std::unordered_map<std::string, double>>
        reset(std::optional<uint64_t> seed = std::nullopt,
              const std::unordered_map<std::string, double> &options = {}) override
        {
            return env_->reset(seed, options);
        }

        const Env &unwrapped() const override { return env_->unwrapped(); }

        const Space &observation_space() const override
        {
            return env_->observation_space();
        }
        const Space &action_space() const override { return env_->action_space(); }
        int render_fps() const override { return env_->render_fps(); }

        std::shared_ptr<Env> env() const { return env_; }

    protected:
        std::shared_ptr<Env> env_;
    };

    // ─── TimeLimit：施加 episode 步数上限 ──────────────────────────
    //
    // Gymnasium 关键设计：超时产生 **truncated**（而非 terminated），
    // 因为它不是 MDP 的终止态。这直接影响 value target 的 bootstrap 项 ——
    // 写成 terminated 会让截断处的 V(s') 被错误丢弃。
    class TimeLimit : public Wrapper
    {
    public:
        TimeLimit(std::shared_ptr<Env> env, int max_episode_steps)
            : Wrapper(std::move(env)), max_steps_(max_episode_steps)
        {
            if (max_steps_ <= 0)
                throw std::invalid_argument("[qrl.TimeLimit] max_episode_steps must be positive");
        }

        std::pair<std::vector<double>, std::unordered_map<std::string, double>>
        reset(std::optional<uint64_t> seed,
              const std::unordered_map<std::string, double> &options) override
        {
            elapsed_ = 0;
            return env_->reset(seed, options);
        }

        StepResult step(const std::vector<double> &action) override
        {
            StepResult r = env_->step(action);
            ++elapsed_;
            if (elapsed_ >= max_steps_ && !r.done())
            {
                // 明确只置 truncated，不动 terminated
                r.truncated = true;
                r.info["TimeLimit.truncated"] = 1.0;
            }
            return r;
        }

        int max_episode_steps() const { return max_steps_; }
        int elapsed() const { return elapsed_; }

    private:
        int max_steps_;
        int elapsed_ = 0;
    };

    // ─── ClipAction：把动作钳制进 action_space ─────────────────────
    //
    // 越界动作应被**捕获**，但生产环境通常选择钳制而非抛异常 ——
    // 抛异常会让一个坏动作终止整次训练。这是显式的策略选择。
    class ClipAction : public Wrapper
    {
    public:
        using Wrapper::Wrapper;

        StepResult step(const std::vector<double> &action) override
        {
            const Box *box = dynamic_cast<const Box *>(&env_->action_space());
            if (!box)
                return env_->step(action);
            std::vector<double> clipped(action.size());
            for (size_t i = 0; i < action.size() && i < box->low().size(); ++i)
            {
                const double lo = box->low()[i], hi = box->high()[i];
                clipped[i] = std::clamp(std::isnan(action[i]) ? 0.0 : action[i], lo, hi);
            }
            return env_->step(clipped);
        }
    };

    // ─── NormalizeObservation：按运行均值/方差缩放观测 ─────────────
    //
    // Gymnasium 的 NormalizeWrapper 思路。用 Welford 在线统计，
    // 故不需预热期，且**不依赖**全局状态（可复现）。
    //
    // 注意：缩放基于累计统计，故同一 seed 的轨迹**不保证逐位相同** ——
    // 这是该包装器的固有性质，任何依赖精确可复现的训练脚本都不应使用它。
    class NormalizeObservation : public Wrapper
    {
    public:
        using Wrapper::Wrapper;

        std::pair<std::vector<double>, std::unordered_map<std::string, double>>
        reset(std::optional<uint64_t> seed,
              const std::unordered_map<std::string, double> &options) override
        {
            auto r = env_->reset(seed, options);
            auto out = normalize(r.first, false);
            return {out, r.second};
        }

        StepResult step(const std::vector<double> &action) override
        {
            StepResult r = env_->step(action);
            r.observation = normalize(r.observation, true);
            return r;
        }

    private:
        // Welford 在线均值/方差
        std::vector<double> mean_, m2_;
        uint64_t count_ = 0;
        double eps_ = 1e-8;

        std::vector<double> normalize(const std::vector<double> &obs, bool update)
        {
            if (obs.empty())
                return obs;
            if (update)
            {
                if (mean_.size() != obs.size())
                {
                    mean_.assign(obs.size(), 0.0);
                    m2_.assign(obs.size(), 0.0);
                    count_ = 0;
                }
                ++count_;
                const double n = static_cast<double>(count_);
                for (size_t i = 0; i < obs.size(); ++i)
                {
                    const double d = obs[i] - mean_[i];
                    mean_[i] += d / n;
                    m2_[i] += d * (obs[i] - mean_[i]);
                }
            }
            if (count_ < 2)
                return obs;
            std::vector<double> out(obs.size());
            for (size_t i = 0; i < obs.size(); ++i)
            {
                const double var = m2_[i] / static_cast<double>(count_ - 1);
                out[i] = (obs[i] - mean_[i]) / std::sqrt(var + eps_);
            }
            return out;
        }
    };

    // ─── TransformReward：奖励仿射变换 ─────────────────────────────
    //
    // γ 折进奖励是常见的数值技巧（reward = r + γ·V(s')，bootstrap 项
    // 置 0）。本包装器只提供通用仿射变换，把 bootstrap 留给算法层 ——
    // 算法层才知道 s'，包装器无从判断。
    class TransformReward : public Wrapper
    {
    public:
        TransformReward(std::shared_ptr<Env> env, double scale, double shift = 0.0)
            : Wrapper(std::move(env)), scale_(scale), shift_(shift) {}

        StepResult step(const std::vector<double> &action) override
        {
            StepResult r = env_->step(action);
            // 先记录原值，便于诊断奖励塑形的效果
            r.info["reward_raw"] = r.reward;
            r.reward = scale_ * r.reward + shift_;
            return r;
        }

    private:
        double scale_, shift_;
    };

    // ─── RecordEpisodeStatistics：逐回合统计 ───────────────────────
    //
    // Gymnasium 的 RecordEpisodeStatistics 语义：回合结束时把统计写进
    // info，使记录器无需任何环境特定代码（Gymnasium 约定 4 的兑现）。
    class RecordEpisodeStatistics : public Wrapper
    {
    public:
        using Wrapper::Wrapper;

        std::pair<std::vector<double>, std::unordered_map<std::string, double>>
        reset(std::optional<uint64_t> seed,
              const std::unordered_map<std::string, double> &options) override
        {
            auto r = env_->reset(seed, options);
            episode_return_ = 0.0;
            episode_len_ = 0;
            return r;
        }

        StepResult step(const std::vector<double> &action) override
        {
            StepResult r = env_->step(action);
            episode_return_ += r.reward;
            ++episode_len_;
            if (r.done())
            {
                r.info["episode_return"] = episode_return_;
                r.info["episode_length"] = static_cast<double>(episode_len_);
                r.info["episode"] = static_cast<double>(++completed_);
                // 保留最近若干回合的回报，供画学习曲线
                returns_.push_back(episode_return_);
                if (returns_.size() > kWindow)
                    returns_.pop_front();
            }
            return r;
        }

        const std::deque<double> &recent_returns() const { return returns_; }

    private:
        static constexpr size_t kWindow = 100;
        double episode_return_ = 0.0;
        int episode_len_ = 0;
        long long completed_ = 0;
        std::deque<double> returns_;
    };

    // ─── 旧接口适配器：IEnvironment → qrl::Env ─────────────────────
    //
    // 让既有 PhysicsEnvironment / TrackingEnvironment 无需修改即可接入
    // 新管线。这是「不破坏既有用户代码」的具体实现。
    //
    // terminated / truncated 的映射规则（旧接口只有一个 `done`）：
    //   只能依据 max_steps 推断。旧 step() 的 done 通常由「到达步数上限
    //   或到达目标」合并而成，无法区分二者 —— 故此处按 spec.max_episode_steps
    //   把「纯超时」判为 truncated，其余判为 terminated，并在文档中
    //   明确说明这是**近似**：真正区分需要环境侧改造。
    class LegacyEnvAdapter : public Env
    {
    public:
        /// 旧接口适配器的行为开关。
        ///
        /// ⚠ 这里用**显式默认构造**而不是默认成员初始化器（NSDMI）：
        ///   嵌套类的 NSDMI 要到**外层类结束**才生效，而下面那个构造函数的
        ///   默认实参 `AdapterSpec spec = {}` 正在外层类**内部**求值 ——
        ///   于是编译器报：
        ///     default member initializer for 'AdapterSpec::max_episode_steps'
        ///     required before the end of its enclosing class
        ///   连带 `= {}` 也无法转换：
        ///     could not convert '<brace-enclosed initializer list>()' to
        ///     'qrl::LegacyEnvAdapter::AdapterSpec'
        ///   （整个 RSP 子项目因此编不出来。）
        ///
        ///   改成显式默认构造后，`{}` 走值初始化 → 调用它 → 语义不变。
        struct AdapterSpec
        {
            int max_episode_steps;       // >0 时把纯超时判为 truncated
            bool success_is_terminated;

            AdapterSpec() : max_episode_steps(-1), success_is_terminated(true) {}
        };

        LegacyEnvAdapter(std::unique_ptr<rvdlrsp::control::IEnvironment> legacy,
                         const Box &obs_space,
                         const Box &act_space,
                         AdapterSpec spec = {})
            : legacy_(std::move(legacy)),
              obs_space_(obs_space),
              act_space_(act_space),
              spec_(spec)
        {
            if (!legacy_)
                throw std::invalid_argument("[qrl.LegacyEnvAdapter] null IEnvironment");
        }

        std::pair<std::vector<double>, std::unordered_map<std::string, double>>
        reset(std::optional<uint64_t> seed,
              const std::unordered_map<std::string, double> &options) override
        {
            elapsed_ = 0;
            if (seed.has_value())
            {
                legacy_rng_.seed(static_cast<std::mt19937::result_type>(*seed));
                seeded_ = true;
            }
            (void)options;
            // 旧 reset() 无种子入口：只能重置环境自有状态。若需要种子控制，
            // 应改用原生 qrl::Env 实现。此处如实记录该限制。
            std::vector<double> obs = legacy_->reset();
            return {obs, {{"adapter", 1.0}, {"seed_applied", seeded_ ? 1.0 : 0.0}}};
        }

        StepResult step(const std::vector<double> &action) override
        {
            // 越界动作显式捕获：交给包装器钳制，但直接调用时必须报错，
            // 否则物理内核会静默饱和并让训练悄悄退化。
            if (!act_space_.contains(action))
                throw std::invalid_argument(
                    "[qrl.LegacyEnvAdapter] action outside action_space");

            auto raw = legacy_->step(action);
            StepResult r;
            r.observation = std::move(raw.observation);
            r.reward = raw.reward;
            ++elapsed_;

            if (raw.done)
            {
                // 见上方说明：无法从单一 done 恢复二者，按超时启发式切分。
                const bool timed_out = spec_.max_episode_steps > 0 &&
                                       elapsed_ >= spec_.max_episode_steps;
                r.truncated = timed_out;
                r.terminated = !timed_out;
            }
            r.info["elapsed_steps"] = static_cast<double>(elapsed_);
            return r;
        }

        const Space &observation_space() const override { return obs_space_; }
        const Space &action_space() const override { return act_space_; }

        rvdlrsp::control::IEnvironment &legacy() { return *legacy_; }

    private:
        std::unique_ptr<rvdlrsp::control::IEnvironment> legacy_;
        Box obs_space_, act_space_;
        AdapterSpec spec_;
        int elapsed_ = 0;
        std::mt19937 legacy_rng_{std::random_device{}()};
        bool seeded_ = false;
    };

} // namespace qrl
