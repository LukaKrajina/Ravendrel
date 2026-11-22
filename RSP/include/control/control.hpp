#pragma once
#include <vector>
#include <string>
#include <unordered_map>
#include <cmath>
#include <cctype>
#include <iostream>
#include "hardware/observability.hpp"

#include "qpc/math.hpp"
#include "rl_agent.hpp"
#include "rl_pipeline.hpp"

namespace rvdlrsp::control
{

    struct IkSolution
    {
        std::vector<double> joint_angles;
        bool converged = true;
    };

    class IkSolver
    {
    public:
        struct Config
        {
            double drive_scale = 3.0;
            size_t joint_count = 6;
        };

        IkSolver() = default;
        explicit IkSolver(Config cfg) : cfg_(cfg) {}

        IkSolution solve(const qpc::Vec3 &target) const
        {
            IkSolution sol;
            sol.joint_angles.assign(cfg_.joint_count, 0.0);
            sol.joint_angles[0] = std::atan2(target.x, cfg_.drive_scale);
            if (cfg_.joint_count > 3)
                sol.joint_angles[3] = std::atan2(target.z, cfg_.drive_scale);
            return sol;
        }

        IkSolution solve(const std::vector<double> &target) const
        {
            qpc::Vec3 v;
            if (!target.empty())
                v.x = target[0];
            if (target.size() > 2)
                v.z = target[2];
            return solve(v);
        }

        const Config &config() const { return cfg_; }

    private:
        Config cfg_;
    };

    class ActionMapper
    {
    public:
        ActionMapper() { build_table(); }

        std::vector<double> map(const std::string &action) const
        {
            auto it = table_.find(normalize(action));
            if (it != table_.end())
            {
                RVDLRSP_INFO("control") << "Mapping action '" << action << "'.";
                return it->second;
            }
            RVDLRSP_WARN("control") << "Unknown action '" << action
                                     << "', falling back to idle.";
            return table_.at("idle");
        }

        bool knows(const std::string &action) const
        {
            return table_.count(normalize(action)) != 0;
        }

    private:
        std::unordered_map<std::string, std::vector<double>> table_;

        static std::string normalize(std::string s)
        {
            for (auto &c : s)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return s;
        }

        void build_table()
        {
            const size_t N = 6;
            auto pose = [N](double pelvis, double head)
            {
                std::vector<double> v(N, 0.0);
                v[0] = pelvis;
                v[3] = head;
                return v;
            };
            const double tilt = 0.35;

            table_["idle"] = pose(0.0, 0.0);
            table_["stop"] = pose(0.0, 0.0);
            table_["forward"] = pose(tilt, 0.0);
            table_["backward"] = pose(-tilt, 0.0);
            table_["left"] = pose(0.0, tilt);
            table_["right"] = pose(0.0, -tilt);
            table_["grab"] = pose(0.0, 0.0);
            table_["release"] = pose(0.0, 0.0);
        }
    };

    class RLInterface
    {
    public:
        RLInterface(IEnvironment &env, double lr = 0.01)
            : env_(&env), agent_(env.observation_dim(), env.action_dim(), lr)
        {
            last_obs_ = env_->reset();
        }

        std::vector<double> observe() const { return last_obs_; }

        std::vector<double> act() { return agent_.act_quantum(last_obs_); }

        double step(const std::vector<double> &action)
        {
            StepResult res = env_->step(action);
            agent_.store(last_obs_, action, res.reward);
            total_reward_ += res.reward;
            ++steps_;
            last_obs_ = res.observation;
            return res.reward;
        }

        void train(int epochs = 1) { agent_.train(epochs); }
        void set_backend(qhal::IQuantumBackend *be) { agent_.set_backend(be); }

        double total_reward() const { return total_reward_; }
        int steps() const { return steps_; }
        const QuantumRLAgent &agent() const { return agent_; }
        QuantumRLAgent &agent() { return agent_; }

    private:
        IEnvironment *env_ = nullptr;
        QuantumRLAgent agent_;
        std::vector<double> last_obs_;
        double total_reward_ = 0.0;
        int steps_ = 0;
    };
}