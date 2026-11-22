#include "test_framework.hpp"
#include "qcdrc/camera.hpp"
#include "qcdrc/mocap.hpp"
#include "qcdrc/teleop.hpp"
#include "qcdrc/teleop_driver.hpp"
#include "qcdrc/cloning.hpp"

using namespace rvdlrsp;
using namespace rvdlrsp::qcdrc;

QTEST(camera_capture) {
    RgbCamera cam;
    RgbFrame f = cam.capture();
    QCHECK(f.width == 640);
    QCHECK(f.height == 480);
    QCHECK(f.pixels.size() == static_cast<size_t>(640) * 480 * 3);
}

QTEST(mocap_skeleton) {
    MotionCapture mc;
    const Skeleton &s = mc.skeleton();
    QCHECK(s.joints.size() >= 13);
    QCHECK(s.joints[0].name == "pelvis");
}

QTEST(teleop_step) {
    Teleop t;
    std::vector<double> angles = t.teleop_step();
    QCHECK(!angles.empty());
    QCHECK(t.joint_targets().size() == angles.size());
}

QTEST(teleop_driver_mapping) {
    TeleopDriver::Config cfg;
    qpc::Vec3 target = TeleopDriver::joint_to_target({0.0, 0.0, 0.0, 0.0}, cfg);
    QCHECK_NEAR(target.x, 0.0, 1e-9);
    QCHECK_NEAR(target.z, 0.0, 1e-9);
    QCHECK_NEAR(target.y, 0.5, 1e-9);
}

QTEST(teleop_driver_force) {
    qpc::RigidBody body;
    body.set_mass(1.0);
    body.position = {0, 0, 0};
    body.linear_velocity = {0, 0, 0};
    TeleopDriver::Config cfg;
    qpc::Vec3 f = TeleopDriver::compute_force(body, {1.0, 0, 0}, cfg);
    QCHECK(f.x > 0.0);
}

QTEST(behavior_cloning) {
    BehaviorCloner cloner;
    cloner.collect({1, 2, 3}, {0.1, 0.2, 0.3});
    cloner.collect({4, 5, 6}, {0.2, 0.3, 0.4});
    QCHECK(cloner.dataset_size() == 2);
    cloner.train(5);
    std::vector<double> pred = cloner.predict({0, 0, 0});
    QCHECK(pred.size() == 3);
    QCHECK_NEAR(pred[0], 0.122022701, 1e-6);
    QCHECK_NEAR(pred[1], 0.222022701, 1e-6);
    QCHECK_NEAR(pred[2], 0.322022701, 1e-6);
}


QTEST(ik_joint_angle_right) {
    Joint3D a{"", -1, 0, 0}, b{"", 0, 0, 0}, c{"", 0, 1, 0};
    QCHECK_NEAR(joint_angle(a, b, c), kPi * 0.5, 1e-9);
}

QTEST(ik_elbow_flexion_straight) {
    Joint3D shoulder{"", 0, 0, 0}, elbow{"", 1, 0, 0}, wrist{"", 2, 0, 0};
    QCHECK_NEAR(elbow_flexion(shoulder, elbow, wrist), 0.0, 1e-9);
}

QTEST(ik_elbow_flexion_right_angle) {
    Joint3D shoulder{"", 0, 0, 0}, elbow{"", 1, 0, 0}, wrist{"", 1, 1, 0};
    QCHECK_NEAR(elbow_flexion(shoulder, elbow, wrist), kPi * 0.5, 1e-9);
}

QTEST(ik_planar_2r_reach) {
    auto r = PlanarIKSolver::solve({1.0, 1.0}, 1.0, 1.0);
    QCHECK(r.converged);
    QCHECK(r.residual < 1e-4);
    double theta = 0.0, ex = 0.0, ey = 0.0;
    for (size_t i = 0; i < r.angles.size(); ++i) {
        theta += r.angles[i];
        ex += 1.0 * std::cos(theta);
        ey += 1.0 * std::sin(theta);
    }
    QCHECK_NEAR(ex, 1.0, 1e-4);
    QCHECK_NEAR(ey, 1.0, 1e-4);
}

QTEST(ik_planar_unreachable_reports) {
    auto r = PlanarIKSolver::solve({1.0, 1.0}, 3.0, 0.0);
    QCHECK(!r.converged);
    QCHECK(r.residual > 0.5);
}

QTEST(teleop_solve_arm_ik) {
    auto q = Teleop::solve_arm_ik(1.0, 1.0, 1.0, 1.0);
    QCHECK(q.size() == 2);
    double shoulder = q[0];
    double elbow_rel = q[1];
    double elbow_abs = shoulder + elbow_rel;
    double ex = std::cos(shoulder) + std::cos(elbow_abs);
    double ey = std::sin(shoulder) + std::sin(elbow_abs);
    QCHECK_NEAR(ex, 1.0, 1e-4);
    QCHECK_NEAR(ey, 1.0, 1e-4);
}

QTEST(teleop_map_motion_extracts_elbow) {
    Teleop t;
    MotionCapture mc;
    t.map_motion(mc.skeleton());
    const auto &targets = t.joint_targets();
    QCHECK(targets.size() == 5);
    bool found = false;
    for (const auto &tt : targets) {
        if (tt.name == "elbow") {
            QCHECK_NEAR(tt.angle_rad, 0.0, 1e-6);
            found = true;
        }
    }
    QCHECK(found);
}