#pragma once
#include <memory>
#include <vector>
#include <functional>
#include <algorithm>
#include <cmath>
#include <iostream>
#include "../../qhal/IQuantumBackend.hpp"
#include "../../../src/QObject.hpp"

namespace vedaros::algorithm
{

    class EntangledPlanner
    {
    private:
        qhal::IQuantumBackend *backend_;

        static double length_penalty(const std::vector<int> &decisions, size_t T = 80)
        {
            double n = static_cast<double>(decisions.size());
            double clipped = std::max(1.0, std::min(n, static_cast<double>(T)));
            double val = std::log2(clipped / static_cast<double>(T));
            return std::max(-1.0, std::min(0.0, val));
        }

    public:
        explicit EntangledPlanner(qhal::IQuantumBackend *backend) : backend_(backend)
        {
            std::cout << "[vedaRos.plan] Fully-entangled consciousness planner online.\n";
        }

        std::vector<std::shared_ptr<::rvdl::BellState>> entangle_consciousness(size_t num_decision_points)
        {
            std::vector<std::shared_ptr<::rvdl::BellState>> consciousness;
            consciousness.reserve(num_decision_points);
            for (size_t i = 0; i < num_decision_points; ++i)
            {
                consciousness.push_back(std::make_shared<::rvdl::BellState>(backend_));
            }
            return consciousness;
        }

        std::vector<int> plan_motion(
            const std::vector<std::shared_ptr<::rvdl::BellState>> &consciousness)
        {
            std::vector<int> goals;
            goals.reserve(consciousness.size());
            for (auto &bell : consciousness)
            {
                auto bits = bell->measure();
                int decision = (bits.empty() ? 0 : bits[0]);
                goals.push_back(decision);
            }
            std::cout << "[vedaRos.plan] Motion planned from " << goals.size()
                      << " entangled decisions.\n";
            return goals;
        }

        std::vector<int> plan_motion_grpo(
            const std::vector<std::shared_ptr<::rvdl::BellState>> &consciousness,
            size_t K = 8,
            std::function<double(const std::vector<int> &)> reward_fn = nullptr)
        {
            std::vector<std::vector<int>> candidates(K);
            for (auto &bell : consciousness)
            {
                for (size_t k = 0; k < K; ++k)
                {
                    bell->reset_to_ground_state();
                    auto bits = bell->measure();
                    candidates[k].push_back(bits.empty() ? 0 : bits[0]);
                }
            }

            std::vector<double> R(K);
            double mean = 0.0;
            for (size_t k = 0; k < K; ++k)
            {
                double acc = reward_fn ? reward_fn(candidates[k]) : 0.0;
                R[k] = acc + length_penalty(candidates[k]);
                mean += R[k];
            }
            mean /= K;

            size_t best = 0;
            double best_adv = R[0] - mean;
            for (size_t k = 1; k < K; ++k)
            {
                double adv = R[k] - mean;
                if (adv > best_adv)
                {
                    best_adv = adv;
                    best = k;
                }
            }

            std::cout << "[vedaRos.plan] GRPO motion planned from " << K
                      << " resampled trajectories (best advantage " << best_adv << ").\n";
            return candidates[best];
        }
    };
}