// RL pipeline 端到端单元测试
#include "test_framework.hpp"
#include "control/rl_pipeline.hpp"

using namespace rvdlrsp::control;

QTEST(environment_step) {
    TrackingEnvironment env;
    QCHECK(env.observation_dim() == 1);
    QCHECK(env.action_dim() == 1);

    auto obs = env.reset();
    QCHECK(obs.size() == 1);

    // 施加正向力，位置应靠近目标
    StepResult r = env.step({1.0});
    QCHECK(r.observation.size() == 1);
    QCHECK(r.reward > -1.0); // 未远离目标
}

QTEST(pipeline_train) {
    TrackingEnvironment env;
    QuantumRLAgent agent(env.observation_dim(), env.action_dim(), 0.05);

    RLConfig cfg;
    cfg.episodes = 20;
    cfg.max_steps = 50;
    cfg.train_every = 5;

    double total = RLPipeline::train(env, agent, cfg);
    QCHECK(agent.buffer_size() > 0); // 采集了经验
    (void)total;
}
