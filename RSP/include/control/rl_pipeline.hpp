#pragma once
#include <vector>
#include <functional>
#include <iostream>
#include "hardware/observability.hpp"
#include "rl_agent.hpp"

namespace rvdlrsp::control {

    // 单步结果：观测 / 奖励 / 是否结束
    struct StepResult {
        std::vector<double> observation;
        double reward = 0.0;
        bool done = false;
    };

    // 环境抽象接口
    // 旧版环境接口（保留以兼容既有 PhysicsEnvironment / TrackingEnvironment
    // 与 rl_demo）。注意其 StepResult::done 无法区分 MDP 终止与范围外截断
    // —— 需要区分的场景请改用 qrl::Env（见 control/qrl.hpp）。
    class IEnvironment {
    public:
        virtual ~IEnvironment() = default;
        virtual std::vector<double> reset() = 0;
        virtual StepResult step(const std::vector<double> &action) = 0;
        virtual size_t observation_dim() const = 0;
        virtual size_t action_dim() const = 0;
    };

    // 训练配置
    struct RLConfig {
        int episodes = 100;
        int max_steps = 200;
        double learning_rate = 0.01;
        int train_every = 10;   // 每 N 步训练一次
    };

    // RL 训练 pipeline
    class RLPipeline {
    public:
        // 训练循环：agent 与环境交互，采集经验并定期更新策略
        static double train(IEnvironment &env, QuantumRLAgent &agent, const RLConfig &cfg = {}) {
            double total_reward = 0.0;
            int step_count = 0;

            RVDLRSP_INFO("rl") << "Pipeline training (" << cfg.episodes
                                << " episodes, lr=" << cfg.learning_rate << ")...";

            for (int ep = 0; ep < cfg.episodes; ++ep) {
                std::vector<double> obs = env.reset();
                double ep_reward = 0.0;

                for (int t = 0; t < cfg.max_steps; ++t) {
                    // 量子估计动作（后端可用时引入量子探索）
                    std::vector<double> action = agent.act_quantum(obs);

                    StepResult res = env.step(action);
                    ep_reward += res.reward;
                    total_reward += res.reward;

                    agent.store(obs, action, res.reward);
                    ++step_count;

                    if (step_count % cfg.train_every == 0)
                        agent.train(1);

                    if (res.done) break;
                    obs = std::move(res.observation);
                }

                if ((ep + 1) % 10 == 0)
                    RVDLRSP_INFO("rl") << "episode " << (ep + 1) << "/" << cfg.episodes
                                        << " reward=" << ep_reward;
            }

            RVDLRSP_INFO("rl") << "Pipeline complete, total reward=" << total_reward;
            return total_reward;
        }
    };

    // ─── 示例环境：目标追踪（观测=位置误差，动作=控制力方向）────────
    // 供端到端联调与测试使用。
    class TrackingEnvironment : public IEnvironment {
    private:
        double position_ = 0.0;
        double target_ = 1.0;
        int step_ = 0;

    public:
        std::vector<double> reset() override {
            position_ = 0.0;
            step_ = 0;
            return observe();
        }

        StepResult step(const std::vector<double> &action) override {
            // 动作[0] 作为控制力，向目标移动
            double force = action.empty() ? 0.0 : action[0];
            position_ += force * 0.1;

            ++step_;
            double error = target_ - position_;
            double reward = -error * error;   // 负二次误差（越大越好）

            StepResult r;
            r.observation = observe();
            r.reward = reward;
            r.done = (step_ >= 100) || (error > -0.01 && error < 0.01);
            return r;
        }

        size_t observation_dim() const override { return 1; }
        size_t action_dim() const override { return 1; }

    private:
        std::vector<double> observe() { return {target_ - position_}; }
    };
}