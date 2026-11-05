#include "control/physics_environment.hpp"
#include "control/rl_pipeline.hpp"
#include "control/rl_agent.hpp"
#include <iostream>

int main() {
    using namespace rvdlrsp::control;

    // 真实物理环境（QM后端）
    // QVM虚拟物理环境
    PhysicsEnvironment env;

    // 量子 RL Agent（可注入 qhal 后端做量子估计）
    QuantumRLAgent agent(env.observation_dim(), env.action_dim(), 0.02);
    agent.set_backend(env.backend());

    RLConfig cfg;
    cfg.episodes = 50;
    cfg.max_steps = 120;
    cfg.learning_rate = 0.02;
    cfg.train_every = 5;

    std::cout << "[rvdlRSP.rl] End-to-end RL training on physics environment...\n";
    double total = RLPipeline::train(env, agent, cfg);

    std::cout << "[rvdlRSP.rl] Training complete. Total reward = " << total << "\n";
    return 0;
}
