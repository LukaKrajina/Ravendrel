#include "test_framework.hpp"
#include "control/prosthetic_driver.hpp"
#include "core/robot.hpp"

using namespace rvdlrsp;
using namespace rvdlrsp::control;
using namespace rvdlrsp::core;

QTEST(prosthetic_robot_driver_build) {
    Robot robot = make_prosthetic_arm();
    qpc::PhysicsKernel kernel(false, 1.0 / 60.0);
    robot.build(kernel);
    QCHECK(kernel.body_count() == 6);
    QCHECK(kernel.joints().size() == 5);
}

QTEST(prosthetic_robot_driver_drive) {
    Robot robot = make_prosthetic_arm();
    qpc::PhysicsKernel kernel(false, 1.0 / 60.0);
    robot.build(kernel);

    auto joints = make_prosthetic_arm_joints();
    ProstheticLimb limb(joints);
    ProstheticRobotDriver driver(limb, robot, kernel, make_prosthetic_arm_bones());

    auto *elbow_bone = robot.skeleton().find("elbow");
    QCHECK(elbow_bone != nullptr);
    qpc::Quat init_orient = kernel.body(elbow_bone->body_index).orientation;

    limb.set_target({0.0, -1.5, 0.0, 0.0, 0.0});
    for (int i = 0; i < 50; ++i)
        driver.drive();

    qpc::Quat cur_orient = kernel.body(elbow_bone->body_index).orientation;
    bool changed = std::fabs(cur_orient.x - init_orient.x) > 1e-6 ||
                   std::fabs(cur_orient.w - init_orient.w) > 1e-6;
    QCHECK(changed);
}

QTEST(prosthetic_robot_driver_bones_match) {
    auto bones = make_prosthetic_arm_bones();
    auto joints = make_prosthetic_arm_joints();
    QCHECK(bones.size() == joints.size());
    QCHECK(bones.size() == 5);
    QCHECK(bones[0] == "upper_arm");
    QCHECK(bones[1] == "elbow");
}

QTEST(prosthetic_kokkos_integration) {
    Robot robot = make_prosthetic_arm();
    qpc::PhysicsKernel kernel(false, 1.0 / 60.0);
    robot.build(kernel);

    QCHECK(!kernel.kokkos_integration_enabled());
    kernel.enable_kokkos_integration(true);
    QCHECK(kernel.kokkos_integration_enabled());

    for (int i = 0; i < 10; ++i)
        kernel.step();
    QCHECK(true);

    kernel.enable_kokkos_integration(false);
    QCHECK(!kernel.kokkos_integration_enabled());
    for (int i = 0; i < 10; ++i)
        kernel.step();
    QCHECK(true);
}