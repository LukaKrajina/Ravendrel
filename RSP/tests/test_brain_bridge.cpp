// 脑意识桥接核心层单元测试
#include "test_framework.hpp"
#include "control/brain_bridge.hpp"

using namespace rvdlrsp::control;
using namespace rvdlrsp::qcdrc;

QTEST(brain_bridge_apply_pd) {
    TeleopDriver::Config base;   // kp=12, kd=2.5
    // 高兴奋度（全 1）→ 增益放大、阻尼降低
    auto mod = BrainConsciousnessBridge::modulate({1, 1, 1, 1}, base);
    QCHECK(mod.pd_config.kp > base.kp);
    QCHECK(mod.pd_config.kd < base.kd);
}

QTEST(brain_bridge_apply_pd_low) {
    TeleopDriver::Config base;
    // 低兴奋度（全 0）→ 增益降低、阻尼提高
    auto mod = BrainConsciousnessBridge::modulate({0, 0, 0, 0}, base);
    QCHECK(mod.pd_config.kp < base.kp);
    QCHECK(mod.pd_config.kd > base.kd);
}

QTEST(brain_bridge_target_offset) {
    // 中性兴奋度 → 无偏移
    auto off = BrainConsciousnessBridge::target_offset({0, 1, 0, 1});
    QCHECK_NEAR(off.x, 0.0, 1e-9);
    QCHECK_NEAR(off.z, 0.0, 1e-9);

    // 高兴奋度 → 正偏移
    auto off2 = BrainConsciousnessBridge::target_offset({1, 1, 1, 1});
    QCHECK(off2.x > 0.0);
}

QTEST(brain_bridge_modulate_empty) {
    TeleopDriver::Config base;
    auto mod = BrainConsciousnessBridge::modulate({}, base);
    QCHECK_NEAR(mod.pd_config.kp, base.kp, 1e-9);
    QCHECK_NEAR(mod.pd_config.kd, base.kd, 1e-9);
}
