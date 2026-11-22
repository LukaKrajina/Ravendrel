#include "test_framework.hpp"
#include "control/brain_bridge.hpp"

using namespace rvdlrsp::control;
using namespace rvdlrsp::qcdrc;

QTEST(brain_bridge_apply_pd) {
    TeleopDriver::Config base;
    auto mod = BrainConsciousnessBridge::modulate({1, 1, 1, 1}, base);
    QCHECK(mod.pd_config.kp > base.kp);
    QCHECK(mod.pd_config.kd < base.kd);
}

QTEST(brain_bridge_apply_pd_low) {
    TeleopDriver::Config base;
    auto mod = BrainConsciousnessBridge::modulate({0, 0, 0, 0}, base);
    QCHECK(mod.pd_config.kp < base.kp);
    QCHECK(mod.pd_config.kd > base.kd);
}

QTEST(brain_bridge_target_offset) {
    auto off = BrainConsciousnessBridge::target_offset({0, 1, 0, 1});
    QCHECK_NEAR(off.x, 0.0, 1e-9);
    QCHECK_NEAR(off.z, 0.0, 1e-9);

    auto off2 = BrainConsciousnessBridge::target_offset({1, 1, 1, 1});
    QCHECK(off2.x > 0.0);
}

QTEST(brain_bridge_modulate_empty) {
    TeleopDriver::Config base;
    auto mod = BrainConsciousnessBridge::modulate({}, base);
    QCHECK_NEAR(mod.pd_config.kp, base.kp, 1e-9);
    QCHECK_NEAR(mod.pd_config.kd, base.kd, 1e-9);
}