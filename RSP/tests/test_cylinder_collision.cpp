// 圆柱碰撞单元测试（球 / 圆柱 / 分离）
#include "test_framework.hpp"
#include "qpc/collision.hpp"
#include "qpc/rigid_body.hpp"

using namespace rvdlrsp::qpc;

QTEST(cylinder_sphere_collision) {
    RigidBody cyl; cyl.set_mass(1.0); cyl.position = {0, 0, 0};
    RigidBody sph; sph.set_mass(1.0); sph.position = {0, 1.2, 0};
    Collider cc; cc.type = ShapeType::Cylinder; cc.radius = 0.5; cc.capsule_half_height = 1.0; cc.body_index = 0;
    Collider cs; cs.type = ShapeType::Sphere; cs.radius = 0.5; cs.body_index = 1;
    Contact c;
    QCHECK(CollisionDetector::cylinder_sphere(cyl, cc, sph, cs, c));
    QCHECK_NEAR(c.penetration, 0.3, 1e-6); // 球心距顶面 0.2，穿透 = 0.5 - 0.2
}

QTEST(cylinder_cylinder_collision) {
    RigidBody a; a.set_mass(1.0); a.position = {0, 0, 0};
    RigidBody b; b.set_mass(1.0); b.position = {0.8, 0, 0};
    Collider ca; ca.type = ShapeType::Cylinder; ca.radius = 0.5; ca.capsule_half_height = 1.0; ca.body_index = 0;
    Collider cb; cb.type = ShapeType::Cylinder; cb.radius = 0.5; cb.capsule_half_height = 1.0; cb.body_index = 1;
    Contact c;
    QCHECK(CollisionDetector::cylinder_cylinder(a, ca, b, cb, c));
    QCHECK_NEAR(c.penetration, 0.2, 1e-6); // 半径和 1.0 - 中心距 0.8
}

QTEST(cylinder_sphere_no_collision) {
    RigidBody cyl; cyl.set_mass(1.0); cyl.position = {0, 0, 0};
    RigidBody sph; sph.set_mass(1.0); sph.position = {3, 0, 0};
    Collider cc; cc.type = ShapeType::Cylinder; cc.radius = 0.5; cc.capsule_half_height = 1.0; cc.body_index = 0;
    Collider cs; cs.type = ShapeType::Sphere; cs.radius = 0.5; cs.body_index = 1;
    Contact c;
    QCHECK(!CollisionDetector::cylinder_sphere(cyl, cc, sph, cs, c));
}

QTEST(cylinder_aabb_collision) {
    RigidBody cyl; cyl.set_mass(1.0); cyl.position = {0, 0, 0};
    RigidBody box; box.set_static(true); box.position = {0, -1.2, 0};
    Collider cc; cc.type = ShapeType::Cylinder; cc.radius = 0.5; cc.capsule_half_height = 1.0; cc.body_index = 0;
    Collider cb; cb.type = ShapeType::AABB; cb.half_extents = {1, 0.5, 1}; cb.body_index = 1;
    Contact c;
    QCHECK(CollisionDetector::cylinder_aabb(cyl, cc, box, cb, c));
}
