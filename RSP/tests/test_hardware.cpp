// 真实硬件 HAL 抽象层单元测试
#include "test_framework.hpp"
#include "hardware/bio_signal.hpp"
#include "hardware/actuator.hpp"
#include "hardware/safety_controller.hpp"
#include "control/prosthetic.hpp"

using namespace rvdlrsp::hardware;
using namespace rvdlrsp::control;

QTEST(sim_emg_sample) {
    SimEmgSource emg(4);
    QCHECK(emg.channel_count() == 4);
    QCHECK(emg.is_connected());
    auto s = emg.sample();
    QCHECK(s.size() == 4);
    auto bits = emg.sample_bits(0.5);
    QCHECK(bits.size() == 4);
}

QTEST(sim_eeg_sample) {
    SimEegSource eeg(8);
    QCHECK(eeg.channel_count() == 8);
    auto s = eeg.sample();
    QCHECK(s.size() == 8);
    auto bits = eeg.sample_bits(0.5);
    QCHECK(bits.size() == 8);
}

QTEST(external_bio_push) {
    ExternalBioSignalSource ext(2);
    ext.push_signal({0.8, -0.3});
    ext.push_bits({1, 0});
    auto s = ext.sample();
    QCHECK_NEAR(s[0], 0.8, 1e-9);
    auto bits = ext.sample_bits(0.5);
    QCHECK(bits[0] == 1);
    QCHECK(bits[1] == 0);
}

QTEST(sim_actuator_position) {
    SimActuator act("test");
    for (int i = 0; i < 50; ++i)
        act.set_position(1.0);
    QCHECK(std::fabs(act.read_position() - 1.0) < 0.05);
}

QTEST(external_actuator_encoder) {
    ExternalActuator act("ext");
    act.push_encoder(0.75, 1.2);
    QCHECK_NEAR(act.read_position(), 0.75, 1e-9);
    QCHECK_NEAR(act.read_velocity(), 1.2, 1e-9);
}

QTEST(safety_clamp) {
    SafetyController sc;
    QCHECK_NEAR(sc.clamp_position(2.0), 1.5, 1e-9);   // 钳到上限
    QCHECK_NEAR(sc.clamp_position(-2.0), -1.5, 1e-9);  // 钳到下限
    QCHECK_NEAR(sc.clamp_position(0.01), 0.0, 1e-9);   // 死区归零
    sc.emergency_stop();
    QCHECK_NEAR(sc.clamp_position(0.5), 0.0, 1e-9);    // 急停后归零
    QCHECK(sc.is_stopped());
    sc.resume();
    QCHECK(!sc.is_stopped());
}

QTEST(emg_prosthetic_controller) {
    auto joints = make_prosthetic_arm_joints();
    ProstheticLimb limb(joints);
    SimEmgSource emg(4);
    std::vector<std::shared_ptr<IActuator>> acts;
    for (int i = 0; i < 5; ++i)
        acts.push_back(std::make_shared<SimActuator>("joint" + std::to_string(i)));

    EmgProstheticController ctrl(&emg, limb, acts);
    ctrl.step();
    ctrl.step();
    QCHECK(true); // 不崩溃即通过
    ctrl.emergency_stop();
    QCHECK(ctrl.is_stopped());
}

QTEST(eeg_eye_controller) {
    SimEegSource eeg(8);
    BionicEye eye;
    EegEyeController ctrl(&eeg, eye);
    ctrl.step();
    ctrl.step();
    QCHECK(true); // 不崩溃即通过
    ctrl.emergency_stop();
    QCHECK(ctrl.is_stopped());
}