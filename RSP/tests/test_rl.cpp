// 量子 RL Agent 单元测试（含 mock 量子后端）
#include "test_framework.hpp"
#include "control/rl_agent.hpp"
#include "qhal/IQuantumBackend.hpp"

using namespace rvdlrsp::control;

// ─── Mock 量子后端（可预测的测量结果）───────────────────
class MockBackend : public qhal::IQuantumBackend {
public:
    size_t alloc_calls = 0, h_calls = 0, rz_calls = 0, measure_calls = 0;
    std::vector<int> measure_results;

    void allocate_qubits(size_t) override { ++alloc_calls; }
    void release_qubit(size_t) override {}
    void lock_hardware_id(size_t) override {}
    void unlock_hardware_id(size_t) override {}
    int measure(size_t q) override {
        ++measure_calls;
        return (q < measure_results.size()) ? measure_results[q] : 0;
    }
    void apply_h(size_t) override { ++h_calls; }
    void apply_x(size_t) override {}
    void apply_rz(size_t, double) override { ++rz_calls; }
    void apply_cnot(size_t, size_t) override {}
    void apply_toffoli(size_t, size_t, size_t) override {}
};

QTEST(rl_agent_act_shape) {
    QuantumRLAgent agent(3, 2);
    std::vector<double> obs = {1.0, 0.5, -0.2};
    std::vector<double> action = agent.act(obs);
    QCHECK(action.size() == 2);
}

QTEST(rl_agent_store_train) {
    QuantumRLAgent agent(2, 1, 0.05);
    agent.store({1.0, 0.0}, {1.0}, 1.0);
    agent.store({1.0, 0.0}, {1.0}, 1.0);
    QCHECK(agent.buffer_size() == 2);
    agent.train(10);
    std::vector<double> pred = agent.act({1.0, 0.0});
    QCHECK(pred.size() == 1);
    QCHECK(pred[0] > 0.0);
}

QTEST(rl_agent_quantum_fallback) {
    QuantumRLAgent agent(2, 2);
    agent.set_backend(nullptr);
    std::vector<double> action = agent.act_quantum({1.0, 1.0});
    QCHECK(action.size() == 2);
}

QTEST(rl_agent_quantum_backend) {
    QuantumRLAgent agent(3, 2);
    MockBackend be;
    be.measure_results = {1, 0};  // 第一个 qubit 测量 1，第二个 0
    agent.set_backend(&be);

    std::vector<double> base = agent.act({0.5, -0.3, 0.8});
    std::vector<double> action = agent.act_quantum({0.5, -0.3, 0.8});

    // 确认量子路径被调用
    QCHECK(be.alloc_calls > 0);
    QCHECK(be.h_calls > 0);
    QCHECK(be.rz_calls > 0);
    QCHECK(be.measure_calls > 0);

    // 量子探索扰动：动作 0 加 +explore（测量=1），动作 1 加 -explore（测量=0）
    QCHECK(action.size() == 2);
    QCHECK(action[0] > base[0]);
    QCHECK(action[1] < base[1]);
}

// 健壮性：观测维度不足不应越界崩溃
QTEST(rl_agent_act_dim_mismatch) {
    QuantumRLAgent agent(3, 2);           // 期望 3 维观测
    auto action = agent.act({1.0, 0.5});  // 只给 2 维
    QCHECK(action.size() == 2);           // 仍输出 2 维动作，不崩溃
}

// 健壮性：经验样本维度不符不应越界崩溃
QTEST(rl_agent_train_dim_mismatch) {
    QuantumRLAgent agent(3, 2, 0.05);
    agent.store({1.0, 0.5}, {1.0}, 1.0);  // 观测/动作维度均不足
    agent.store({1.0, 0.5, -0.2}, {1.0, 0.5}, 1.0); // 正常样本
    agent.train(5);                       // 不应崩溃，维度不符样本被跳过
    QCHECK(true);
}
