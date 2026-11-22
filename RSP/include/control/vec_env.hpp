#pragma once
#include "control/qrl.hpp"

#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace qrl
{

    enum class AutoresetMode
    {
        NextStep = 0,
        SameStep = 1,
        Disabled = 2,
    };

    struct VectorStepResult
    {
        std::vector<std::vector<double>> observations;
        std::vector<double> rewards;
        std::vector<bool> terminated;
        std::vector<bool> truncated;
        std::unordered_map<std::string, std::vector<double>> info;

        size_t size() const { return rewards.size(); }
        bool any_done() const
        {
            for (size_t i = 0; i < terminated.size(); ++i)
                if (terminated[i] || truncated[i])
                    return true;
            return false;
        }
    };

    class VectorEnv
    {
    public:
        virtual ~VectorEnv() = default;

        virtual size_t num_envs() const = 0;

        virtual std::pair<std::vector<std::vector<double>>,
                          std::unordered_map<std::string, std::vector<double>>>
        reset(std::optional<std::vector<std::optional<uint64_t>>> seeds) = 0;

        virtual VectorStepResult step(const std::vector<std::vector<double>> &actions) = 0;

        virtual const Space &single_action_space() const = 0;
        virtual const Space &single_observation_space() const = 0;

        AutoresetMode autoreset_mode() const { return autoreset_mode_; }

        virtual void close() {}

        virtual double attr(size_t, const std::string &) const = 0;
        virtual void set_attr(size_t, const std::string &, double) = 0;

    protected:
        AutoresetMode autoreset_mode_ = AutoresetMode::NextStep;
    };

    class SyncVectorEnv final : public VectorEnv
    {
    public:
        explicit SyncVectorEnv(std::vector<std::shared_ptr<Env>> envs,
                                AutoresetMode mode = AutoresetMode::NextStep)
            : envs_(std::move(envs))
        {
            autoreset_mode_ = mode;
            if (envs_.empty())
                throw std::invalid_argument("[qrl.SyncVectorEnv] no sub-environments");
            needs_reset_.assign(envs_.size(), false);
            out_of_range_.assign(envs_.size(), false);
            last_reset_info_.assign(envs_.size(), {});
            for (size_t i = 1; i < envs_.size(); ++i)
            {
                if (envs_[i]->observation_space().dim() != envs_[0]->observation_space().dim() ||
                    envs_[i]->action_space().dim() != envs_[0]->action_space().dim())
                    throw std::invalid_argument(
                        "[qrl.SyncVectorEnv] sub-environment spaces differ at index " +
                        std::to_string(i));
            }
        }

        size_t num_envs() const override { return envs_.size(); }

        const Space &single_action_space() const override { return envs_[0]->action_space(); }
        const Space &single_observation_space() const override
        {
            return envs_[0]->observation_space();
        }

        std::pair<std::vector<std::vector<double>>,
                  std::unordered_map<std::string, std::vector<double>>>
        reset(std::optional<std::vector<std::optional<uint64_t>>> seeds) override
        {
            const size_t n = envs_.size();
            std::vector<std::vector<double>> obs(n);
            std::unordered_map<std::string, std::vector<double>> info;

            std::vector<std::optional<uint64_t>> per_env(n, std::nullopt);
            if (seeds.has_value())
            {
                if (seeds->size() == 1)
                {
                    for (size_t i = 0; i < n; ++i)
                        if ((*seeds)[0].has_value())
                            per_env[i] = *(*seeds)[0] + i;
                }
                else if (seeds->size() == n)
                {
                    per_env = *seeds;
                }
                else
                {
                    throw std::invalid_argument(
                        "[qrl.SyncVectorEnv.reset] seeds must be nullopt, {seed}, or length num_envs");
                }
            }

            std::unordered_map<std::string, std::vector<double>> sub_info;
            for (size_t i = 0; i < n; ++i)
            {
                auto r = envs_[i]->reset(per_env[i], {});
                obs[i] = std::move(r.first);
                for (const auto &kv : r.second)
                    sub_info[kv.first].push_back(kv.second);
                needs_reset_[i] = false;
            }
            for (auto &kv : sub_info)
            {
                if (kv.second.size() < n)
                    kv.second.resize(n, 0.0);
                info.emplace(std::move(kv));
            }
            return {std::move(obs), std::move(info)};
        }

        VectorStepResult step(const std::vector<std::vector<double>> &actions) override
        {
            const size_t n = envs_.size();
            if (actions.size() != n)
                throw std::invalid_argument(
                    "[qrl.SyncVectorEnv.step] actions.size() != num_envs");

            VectorStepResult out;
            out.observations.resize(n);
            out.rewards.assign(n, 0.0);
            out.terminated.assign(n, false);
            out.truncated.assign(n, false);
            std::unordered_map<std::string, std::vector<double>> sub_info;

            for (size_t i = 0; i < n; ++i)
            {
                if (autoreset_mode_ == AutoresetMode::Disabled && needs_reset_[i])
                    throw std::runtime_error(
                        "[qrl.SyncVectorEnv.step] sub-environment " + std::to_string(i) +
                        " requires an explicit reset (AutoresetMode::Disabled)");

                if (!single_action_space().contains(actions[i]))
                    out_of_range_[i] = true;

                if (autoreset_mode_ == AutoresetMode::NextStep && needs_reset_[i])
                {
                    auto rr = envs_[i]->reset(std::nullopt, {});
                    needs_reset_[i] = false;
                    last_reset_info_[i] = rr.second;
                }

                StepResult r = envs_[i]->step(actions[i]);

                if (r.terminated || r.truncated)
                {
                    switch (autoreset_mode_)
                    {
                    case AutoresetMode::NextStep:
                        needs_reset_[i] = true;
                        break;
                    case AutoresetMode::SameStep:
                    {
                        auto rr = envs_[i]->reset(std::nullopt, {});
                        r.observation = std::move(rr.first);
                        for (const auto &kv : rr.second)
                            r.info[kv.first + ".post_reset"] = kv.second;
                        needs_reset_[i] = false;
                        break;
                    }
                    case AutoresetMode::Disabled:
                        needs_reset_[i] = true;
                        break;
                    }
                }
                else
                {
                    needs_reset_[i] = false;
                }

                out.observations[i] = std::move(r.observation);
                out.rewards[i] = r.reward;
                out.terminated[i] = r.terminated;
                out.truncated[i] = r.truncated;
                for (const auto &kv : r.info)
                    sub_info[kv.first].push_back(kv.second);
            }

            for (auto &kv : sub_info)
                if (kv.second.size() < n)
                    kv.second.resize(n, 0.0);
            out.info = std::move(sub_info);
            return out;
        }

        bool action_out_of_range(size_t i) const
        {
            return i < out_of_range_.size() && out_of_range_[i];
        }
        bool any_action_out_of_range() const
        {
            for (size_t i = 0; i < out_of_range_.size(); ++i)
                if (out_of_range_[i])
                    return true;
            return false;
        }
        void clear_out_of_range()
        {
            for (size_t i = 0; i < out_of_range_.size(); ++i)
                out_of_range_[i] = false;
        }

        double attr(size_t i, const std::string &key) const override
        {
            if (i >= envs_.size())
                throw std::out_of_range("[qrl.SyncVectorEnv.attr] index out of range");
            if (key == "num_envs")
                return static_cast<double>(envs_.size());
            if (key == "render_fps")
                return static_cast<double>(envs_[i]->render_fps());
            if (key == "closed")
                return envs_[i]->closed() ? 1.0 : 0.0;
            throw std::invalid_argument("[qrl.SyncVectorEnv.attr] unsupported key: " + key);
        }

        void set_attr(size_t, const std::string &, double) override
        {
            throw std::logic_error(
                "[qrl.SyncVectorEnv.set_attr] sub-environments expose no mutable attributes");
        }

        void close() override
        {
            for (auto &e : envs_)
                if (e)
                    e->close();
        }

    private:
        std::vector<std::shared_ptr<Env>> envs_;
        std::vector<bool> needs_reset_;
        std::vector<bool> out_of_range_;
        std::vector<std::unordered_map<std::string, double>> last_reset_info_;
    };

    inline std::unique_ptr<SyncVectorEnv>
    make_vector_env(size_t n, const std::function<std::shared_ptr<Env>()> &factory,
                    AutoresetMode mode = AutoresetMode::NextStep)
    {
        if (n == 0)
            throw std::invalid_argument("[qrl.make_vector_env] n must be positive");
        std::vector<std::shared_ptr<Env>> envs;
        envs.reserve(n);
        for (size_t i = 0; i < n; ++i)
        {
            auto e = factory();
            if (!e)
                throw std::runtime_error("[qrl.make_vector_env] factory returned null");
            envs.push_back(std::move(e));
        }
        return std::make_unique<SyncVectorEnv>(std::move(envs), mode);
    }

}