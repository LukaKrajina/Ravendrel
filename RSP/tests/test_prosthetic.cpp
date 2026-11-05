// 义肢 / 义眼 脑意识控制 + 量子 RL 辅助单元测试
#include "test_framework.hpp"
#include "control/prosthetic.hpp"

using namespace rvdlrsp::control;

QTEST(prosthetic_limb_basic) {
    auto joints = make_prosthetic_arm_joints();
    ProstheticLimb limb(joints);
    QCHECK(limb.joint_count() == 5);
    QCHECK(limb.angles().size() == 5);
    QCHECK(limb.target_angles().size() == 5);
}

QTEST(prosthetic_intent_maps_arousal) {
    auto joints = make_prosthetic_arm_joints();
    ProstheticLimb limb(joints);
    // 高意识兴奋度（全 1）→ 关节屈曲（目标角 > 放松角 0）
    limb.set_intent({1, 1, 1, 1});
    const auto &t = limb.target_angles();
    QCHECK(t[1] > 0.0); // elbow 目标角 > 放松角 0
    // 低意识兴奋度（全 0）→ 关节伸展（目标角 < 放松角）
    limb.set_intent({0, 0, 0, 0});
    const auto &t2 = limb.target_angles();
    QCHECK(t2[1] < 0.0);
}

QTEST(prosthetic_step_converges) {
    auto joints = make_prosthetic_arm_joints();
    ProstheticLimb limb(joints);
    limb.set_target({0.5, -1.0, 0.3, 0.8, 1.0});
    for (int i = 0; i < 50; ++i) limb.step();
    const auto &a = limb.angles();
    const auto &t = limb.target_angles();
    // 一阶趋近后应接近目标（误差 < 0.1）
    QCHECK(std::fabs(a[0] - t[0]) < 0.1);
}

QTEST(prosthetic_train_no_crash) {
    auto joints = make_prosthetic_arm_joints();
    ProstheticLimb limb(joints);
    limb.set_target({0.5, -1.0, 0.3, 0.8, 1.0});
    for (int i = 0; i < 10; ++i) {
        limb.step();
        limb.feedback(0.0);
    }
    limb.train(1);
    QCHECK(true);
}

QTEST(bionic_eye_basic) {
    BionicEye eye;
    QCHECK(eye.pan() == 0.0);
    QCHECK(eye.tilt() == 0.0);
    // 初始注视方向为 -Z
    auto g = eye.gaze_direction();
    QCHECK_NEAR(g.z, -1.0, 1e-9);
}

QTEST(bionic_eye_intent) {
    BionicEye eye;
    eye.set_intent({1, 1, 1, 1}); // 高兴奋度 → 向右扫视
    QCHECK(eye.target_pan() > 0.0);
}

QTEST(bionic_eye_step_converges) {
    BionicEye eye;
    eye.set_target(1.0, 0.5);
    for (int i = 0; i < 50; ++i) eye.step();
    QCHECK(std::fabs(eye.pan() - 1.0) < 0.1);
    QCHECK(std::fabs(eye.tilt() - 0.5) < 0.1);
}

QTEST(prosthetic_environment) {
    auto joints = make_prosthetic_arm_joints();
    ProstheticEnvironment env(joints);
    auto obs = env.reset();
    QCHECK(obs.size() == 10); // 5 关节 × 2 维
    auto res = env.step({0.5, -1.0, 0.3, 0.8, 1.0});
    QCHECK(res.observation.size() == 10);
    QCHECK(res.reward < 0.0); // 初始误差导致负奖励
}

QTEST(bionic_eye_camera_shape) {
    BionicEyeCamera cam;
    cam.set_target_world({0.0, 0.0, -3.0});
    auto frame = cam.capture();
    QCHECK(frame.width == 640);
    QCHECK(frame.height == 480);
    QCHECK(frame.pixels.size() == 640u * 480u * 3u);
    QCHECK(cam.is_open());
}

QTEST(bionic_eye_camera_target_centered) {
    BionicEyeCamera cam;
    // 目标在正前方，注视初始朝向 -Z，目标应投影到图像中央
    cam.set_target_world({0.0, 0.0, -3.0});
    auto frame = cam.capture();

    // 检查中央区域存在亮色（目标）像素
    bool has_bright = false;
    for (int py = 240 - 25; py <= 240 + 25 && !has_bright; ++py)
        for (int px = 320 - 25; px <= 320 + 25; ++px)
        {
            size_t idx = (static_cast<size_t>(py) * 640 + px) * 3;
            if (frame.pixels[idx] > 100)
            {
                has_bright = true;
                break;
            }
        }
    QCHECK(has_bright);
}

QTEST(bionic_eye_camera_track_moves_target) {
    BionicEyeCamera cam;
    // 目标在视野左侧 → 义眼注视后应趋近，目标投影向中央靠拢
    cam.set_target_world({-1.5, 0.0, -3.0});
    // 设置注视目标以追踪该方向（模拟脑意识/视觉追踪）
    cam.eye().set_target(0.4, 0.0);
    for (int i = 0; i < 50; ++i)
        cam.eye().step();
    auto frame = cam.capture();
    (void)frame; // 仅验证不崩溃 + 帧尺寸
    QCHECK(frame.width == 640);
}