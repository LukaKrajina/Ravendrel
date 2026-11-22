#pragma once
#include <vector>
#include <random>
#include <cmath>
#include <limits>
#include <algorithm>
#include <iostream>
#include "hardware/observability.hpp"
#include "qhal/IQuantumBackend.hpp"
#include "numqk/Numqk.hpp"

namespace rvdlrsp::control
{

    struct Experience
    {
        std::vector<double> observation;
        std::vector<double> action;
        double reward = 0.0;
    };

    class QuantumRLAgent
    {
    private:
        qhal::IQuantumBackend *backend_ = nullptr;
        numqk::Tensor<double> policy_weights_;
        std::vector<Experience> replay_buffer_;
        std::mt19937 rng_;
        double learning_rate_ = 0.01;
        size_t obs_dim_ = 0;
        size_t act_dim_ = 0;

    public:
        QuantumRLAgent(size_t obs_dim, size_t act_dim, double lr = 0.01)
            : obs_dim_(obs_dim), act_dim_(act_dim), learning_rate_(lr),
              policy_weights_(numqk::Tensor<double>({obs_dim, act_dim}, false)),
              rng_(std::random_device{}())
        {
            std::uniform_real_distribution<double> dist(-0.1, 0.1);
            for (size_t i = 0; i < policy_weights_.size(); ++i)
                policy_weights_.data()[i] = dist(rng_);
        }

        void set_backend(qhal::IQuantumBackend *be) { backend_ = be; }

        std::vector<double> act(const std::vector<double> &obs)
        {
            std::vector<double> action(act_dim_, 0.0);
            size_t o_lim = std::min(obs.size(), obs_dim_);
            for (size_t a = 0; a < act_dim_; ++a)
                for (size_t o = 0; o < o_lim; ++o)
                    action[a] += policy_weights_.data()[o * act_dim_ + a] * obs[o];
            return action;
        }

        std::vector<double> act_quantum(const std::vector<double> &obs)
        {
            std::vector<double> base = act(obs);
            if (!backend_)
                return base;

            size_t n = std::min(obs.size(), static_cast<size_t>(16));
            if (n == 0)
                return base;

            backend_->allocate_qubits(n);
            for (size_t i = 0; i < n; ++i)
            {
                double angle = std::atan(obs[i]) * 2.0;
                backend_->apply_h(i);
                backend_->apply_rz(i, angle);
            }

            double explore_scale = 0.05;
            std::vector<double> action = base;
            for (size_t a = 0; a < act_dim_ && a < n; ++a)
            {
                int m = backend_->measure(a);
                action[a] += (m == 1) ? explore_scale : -explore_scale;
            }
            return action;
        }

        void store(const std::vector<double> &obs, const std::vector<double> &action, double reward)
        {
            replay_buffer_.push_back({obs, action, reward});
            if (replay_buffer_.size() > 10000)
                replay_buffer_.erase(replay_buffer_.begin());
        }

        size_t buffer_size() const { return replay_buffer_.size(); }

        void train(int epochs)
        {
            std::vector<const Experience *> samples;
            double max_reward = -std::numeric_limits<double>::infinity();
            for (const auto &exp : replay_buffer_)
            {
                if (exp.observation.size() < obs_dim_ || exp.action.size() < act_dim_)
                    continue;
                samples.push_back(&exp);
                max_reward = std::max(max_reward, exp.reward);
            }
            if (samples.empty())
                return;

            RVDLRSP_INFO("rl") << "Training agent (" << epochs << " epochs, "
                                << samples.size() << " samples, max reward=" << max_reward << ")...";
            for (int e = 0; e < epochs; ++e)
            {
                for (const Experience *exp : samples)
                {
                    const double weight = std::exp(exp->reward - max_reward);
                    std::vector<double> pred = act(exp->observation);
                    for (size_t a = 0; a < act_dim_; ++a)
                    {
                        double error = exp->action[a] - pred[a];
                        for (size_t o = 0; o < obs_dim_; ++o)
                        {
                            policy_weights_.data()[o * act_dim_ + a] +=
                                learning_rate_ * weight * error * exp->observation[o];
                        }
                    }
                }
            }
        }
    };
}