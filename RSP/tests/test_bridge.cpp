// VedaROS 桥真实数据流单元测试(去空壳)
#include "test_framework.hpp"
#include "bridge/vedaros_bridge.hpp"
#include <atomic>
#include <thread>
#include <chrono>

using namespace rvdlrsp::bridge;

QTEST(bridge_register_robot) {
    VedaRosBridge bridge;
    auto node = bridge.register_robot("test", "robot1");
    QCHECK(node != nullptr);
    QCHECK(bridge.participant_count() == 1);
    QCHECK(node->endpoint().node_name == "robot1");
    QCHECK(node->endpoint().is_quantum_capable);
}

QTEST(bridge_serialize_roundtrip) {
    std::vector<double> in{1.5, -2.0, 3.25, 0.0};
    auto bytes = VedaRosBridge::serialize_doubles(in);
    QCHECK(bytes.size() == in.size() * sizeof(double));
    auto out = VedaRosBridge::deserialize_doubles(bytes);
    QCHECK(out.size() == in.size());
    for (size_t i = 0; i < in.size(); ++i)
        QCHECK_NEAR(out[i], in[i], 1e-12);
}

QTEST(bridge_publish_joint_state) {
    VedaRosBridge bridge;
    auto node = bridge.register_robot("test", "robot1");
    bridge.publish_joint_state(node, {0.5, -1.0, 0.3});

    auto msgs = bridge.drain_messages();
    QCHECK(msgs.size() == 1);
    QCHECK(msgs[0].topic == "joint_state");
    auto angles = VedaRosBridge::deserialize_doubles(msgs[0].payload);
    QCHECK(angles.size() == 3);
    QCHECK_NEAR(angles[0], 0.5, 1e-9);
    QCHECK_NEAR(angles[1], -1.0, 1e-9);
    QCHECK_NEAR(angles[2], 0.3, 1e-9);
}

QTEST(bridge_subscribe_receives) {
    VedaRosBridge bridge;
    auto pub = bridge.register_robot("t", "pub");
    auto sub = bridge.register_robot("t", "sub");

    std::vector<double> received;
    std::atomic<bool> got{false};
    bridge.subscribe_joint_command(sub, [&](const std::vector<double> &a) {
        received = a;
        got.store(true);
    });

    sub->spin_async();

    // 发布 joint_command(与 subscribe_joint_command 订阅的 topic 一致)
    pub->publish("joint_command", vedaros::TypeId::Custom,
                 VedaRosBridge::serialize_doubles({4.0, 5.0, 6.0}));

    // 轮询等待回调(最多约 1s)
    for (int i = 0; i < 100 && !got.load(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

    QCHECK(got.load());
    QCHECK(received.size() == 3);
    QCHECK_NEAR(received[0], 4.0, 1e-9);
    QCHECK_NEAR(received[2], 6.0, 1e-9);
}
