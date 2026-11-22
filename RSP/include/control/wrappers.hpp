#pragma once
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

#include "control/rl_pipeline.hpp"

namespace qrl
{

    class Wrapper : public Env
    {
    public:
        explicit Wrapper(std::shared_ptr<Env> env) : env_(std::move(env)) {}

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

    class TransformReward : public Wrapper
    {
    public:
        TransformReward(std::shared_ptr<Env> env, double scale, double shift = 0.0)
            : Wrapper(std::move(env)), scale_(scale), shift_(shift) {}

        StepResult step(const std::vector<double> &action) override
        {
            StepResult r = env_->step(action);
            r.info["reward_raw"] = r.reward;
            r.reward = scale_ * r.reward + shift_;
            return r;
        }

    private:
        double scale_, shift_;
    };

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

    class LegacyEnvAdapter : public Env
    {
    public:
        struct AdapterSpec
        {
            int max_episode_steps;
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
            std::vector<double> obs = legacy_->reset();
            return {obs, {{"adapter", 1.0}, {"seed_applied", seeded_ ? 1.0 : 0.0}}};
        }

        StepResult step(const std::vector<double> &action) override
        {
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

}