// 关节约束单元测试（球窝 / 距离 / 固定 / 棱柱）
#include "test_framework.hpp"
#include "qpc/joint.hpp"
#include "qpc/rigid_body.hpp"

using namespace rvdlrsp::qpc;

QTEST(ball_socket_aligns_anchor) {
    RigidBody a; a.set_mass(1.0); a.position = {0, 0, 0};
    RigidBody b; b.set_mass(1.0); b.position = {0, 0, 1};
    Joint j; j.type = JointType::BallSocket; j.body_a = 0; j.body_b = 1;
    j.anchor = {0, 0, 0}; j.stiffness = 1.0;
    JointSolver::solve(a, b, j);
    QCHECK(b.position.z < 1.0); // b 朝锚点移动
}

QTEST(distance_joint_holds_distance) {
    RigidBody a; a.set_mass(1.0); a.position = {0, 0, 0};
    RigidBody b; b.set_mass(1.0); b.position = {2, 0, 0};
    Joint j; j.type = JointType::Distance; j.body_a = 0; j.body_b = 1;
    j.rest_distance = 1.0; j.stiffness = 1.0;
    JointSolver::solve(a, b, j);
    double dist = (b.position - a.position).length();
    QCHECK_NEAR(dist, 1.0, 1e-6);
}

QTEST(fixed_joint_converges) {
    RigidBody a; a.set_mass(1.0);
    RigidBody b; b.set_mass(1.0); b.position = {1, 0, 0};
    Joint j; j.type = JointType::Fixed; j.body_a = 0; j.body_b = 1;
    j.anchor = {0, 0, 0}; j.stiffness = 0.5;
    for (int i = 0; i < 10; ++i) JointSolver::solve(a, b, j);
    QCHECK((b.position - a.position).length() < 0.5);
}

QTEST(prismatic_limits_slide) {
    RigidBody a; a.set_mass(1.0);
    RigidBody b; b.set_mass(1.0); b.position = {0, 3, 0};
    Joint j; j.type = JointType::Prismatic; j.body_a = 0; j.body_b = 1;
    j.axis = {0, 1, 0}; j.min_limit = 0.0; j.max_limit = 1.0; j.stiffness = 1.0;
    for (int i = 0; i < 10; ++i) JointSolver::solve(a, b, j);
    double slide = (b.position - a.position).dot({0, 1, 0});
    QCHECK(slide <= 1.0 + 1e-6);
}
