#include "test_framework.hpp"
#include "control/consciousness_controller.hpp"

using namespace rvdlrsp::control;

QTEST(consciousness_neutral) {
    auto m = ConsciousnessController::compute({0, 1, 0, 1});
    QCHECK_NEAR(m.arousal, 0.5, 1e-9);
    QCHECK_NEAR(m.gain_scale, 1.0, 1e-9);
    QCHECK_NEAR(m.damping_scale, 1.0, 1e-9);
}

QTEST(consciousness_high_arousal) {
    auto m = ConsciousnessController::compute({1, 1, 1, 1});
    QCHECK_NEAR(m.arousal, 1.0, 1e-9);
    QCHECK(m.gain_scale > 1.0);
    QCHECK(m.damping_scale < 1.0);
}

QTEST(consciousness_low_arousal) {
    auto m = ConsciousnessController::compute({0, 0, 0, 0});
    QCHECK_NEAR(m.arousal, 0.0, 1e-9);
    QCHECK(m.gain_scale < 1.0);
    QCHECK(m.damping_scale > 1.0);
}

QTEST(consciousness_apply) {
    double kp = 10.0, kd = 2.0;
    auto m = ConsciousnessController::compute({1, 1});
    double kp2 = ConsciousnessController::apply_gain(kp, m.gain_scale);
    double kd2 = ConsciousnessController::apply_damping(kd, m.damping_scale);
    QCHECK(kp2 > kp);
    QCHECK(kd2 < kd);
}

QTEST(consciousness_empty) {
    auto m = ConsciousnessController::compute({});
    QCHECK_NEAR(m.arousal, 0.5, 1e-9);
    QCHECK_NEAR(m.gain_scale, 1.0, 1e-9);
}