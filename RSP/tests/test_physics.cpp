// 物理内核单元测试（刚体 / 碰撞 / 约束）
#include "test_framework.hpp"
#include "qpc/math.hpp"
#include "qpc/rigid_body.hpp"
#include "qpc/collision.hpp"
#include "qpc/constraint.hpp"
#include "qpc/physics_kernel.hpp"

using namespace rvdlrsp::qpc;

QTEST(rigid_body_mass) {
    RigidBody b;
    b.set_mass(2.0, 3.0);
    QCHECK_NEAR(b.mass, 2.0, 1e-9);
    QCHECK_NEAR(b.inv_mass, 0.5, 1e-9);
    QCHECK_NEAR(b.inv_inertia, 1.0 / 3.0, 1e-9);
}

QTEST(rigid_body_static) {
    RigidBody b;
    b.set_static(true);
    QCHECK_NEAR(b.inv_mass, 0.0, 1e-9);
    QCHECK(b.is_static);
}

QTEST(sphere_sphere_collision) {
    RigidBody a; a.set_mass(1.0); a.position = {0, 0, 0};
    RigidBody b; b.set_mass(1.0); b.position = {1.5, 0, 0};
    Collider ca; ca.type = ShapeType::Sphere; ca.radius = 1.0; ca.body_index = 0;
    Collider cb; cb.type = ShapeType::Sphere; cb.radius = 1.0; cb.body_index = 1;

    Contact c;
    QCHECK(CollisionDetector::sphere_sphere(a, ca, b, cb, c));
    QCHECK_NEAR(c.penetration, 0.5, 1e-9); // 2 - 1.5
}

QTEST(sphere_sphere_no_collision) {
    RigidBody a; a.set_mass(1.0); a.position = {0, 0, 0};
    RigidBody b; b.set_mass(1.0); b.position = {5, 0, 0};
    Collider ca; ca.type = ShapeType::Sphere; ca.radius = 1.0; ca.body_index = 0;
    Collider cb; cb.type = ShapeType::Sphere; cb.radius = 1.0; cb.body_index = 1;
    Contact c;
    QCHECK(!CollisionDetector::sphere_sphere(a, ca, b, cb, c));
}

QTEST(sphere_aabb_collision) {
    RigidBody sphere; sphere.set_mass(1.0); sphere.position = {0, 0, 0};
    RigidBody box; box.set_static(true); box.position = {0, -1.0, 0};
    Collider cs; cs.type = ShapeType::Sphere; cs.radius = 0.5; cs.body_index = 0;
    Collider cb; cb.type = ShapeType::AABB; cb.half_extents = {1, 0.5, 1}; cb.body_index = 1;
    Contact c;
    QCHECK(CollisionDetector::sphere_aabb(sphere, cs, box, cb, c));
}

QTEST(contact_solve_separating) {
    // 法线 {0,1,0} 指向 b；a 向下、b 向上 → 二者远离（分离）
    RigidBody a; a.set_mass(1.0); a.position = {0, 0, 0}; a.linear_velocity = {0, -1, 0};
    RigidBody b; b.set_mass(1.0); b.position = {0, 0, 0}; b.linear_velocity = {0, 1, 0};
    Contact c;
    c.a = 0; c.b = 1; c.normal = {0, 1, 0}; c.penetration = 0.1;
    Vec3 va = a.linear_velocity, vb = b.linear_velocity;
    ConstraintSolver::solve_contact(a, b, c);
    QCHECK_NEAR(a.linear_velocity.y, va.y, 1e-9);
    QCHECK_NEAR(b.linear_velocity.y, vb.y, 1e-9);
}

QTEST(positional_correction) {
    RigidBody a; a.set_mass(1.0); a.position = {0, 0, 0};
    RigidBody b; b.set_mass(1.0); b.position = {0, 0, 0};
    Contact c;
    c.a = 0; c.b = 1; c.normal = {0, 1, 0}; c.penetration = 1.0;
    ConstraintSolver::positional_correction(a, b, c);
    // a 向下、b 向上分开
    QCHECK(a.position.y < 0.0);
    QCHECK(b.position.y > 0.0);
}

// ─── 物理阻尼(替换硬编码 *0.999)─────────────────────

QTEST(physics_damping_decays) {
    PhysicsKernel k(false, 1.0 / 60.0);
    k.set_gravity({0, 0, 0});
    RigidBody b; b.set_mass(1.0);
    b.linear_damping = 1.0;   // 1/s 的阻尼,1 秒后速度约衰减至 e^-1 ≈ 0.368
    b.angular_damping = 1.0;
    b.linear_velocity = {1, 0, 0};
    Collider c; c.type = ShapeType::Sphere; c.radius = 0.5;
    k.add_body(b, c);
    for (int i = 0; i < 60; ++i)
        k.step();
    double v = k.body(0).linear_velocity.x;
    QCHECK(v < 0.5); // 显著衰减
    QCHECK(v > 0.2); // 但未衰减到 0(约 e^-1)
}

QTEST(physics_damping_dt_independent) {
    // 相同总时间(1s),不同步长下阻尼效果应一致(物理正确,与帧率无关)
    auto run = [](double dt, int steps) {
        PhysicsKernel k(false, dt);
        k.set_gravity({0, 0, 0});
        RigidBody b; b.set_mass(1.0);
        b.linear_damping = 1.0;
        b.angular_damping = 1.0;
        b.linear_velocity = {1, 0, 0};
        Collider c; c.type = ShapeType::Sphere; c.radius = 0.5;
        k.add_body(b, c);
        for (int i = 0; i < steps; ++i)
            k.step();
        return k.body(0).linear_velocity.x;
    };
    double v60 = run(1.0 / 60.0, 60);
    double v30 = run(1.0 / 30.0, 30);
    QCHECK_NEAR(v60, v30, 1e-2);
}

// ─── 物理稳定性回归 ───────────────────────────────────

QTEST(physics_energy_conservation) {
    // 自由落体(无阻尼、无碰撞),机械能应守恒。
    // 半隐式欧拉对匀加速场(重力)精确,能量误差应极小。
    PhysicsKernel k(false, 1.0 / 60.0);
    k.set_gravity({0, -9.81, 0});
    RigidBody b; b.set_mass(1.0);
    b.linear_damping = 0.0;
    b.angular_damping = 0.0;
    b.position = {0, 5.0, 0};
    Collider c; c.type = ShapeType::Sphere; c.radius = 0.5;
    k.add_body(b, c);

    double initial_pe = 1.0 * 9.81 * 5.0;

    // 落体 0.5s(30 步),未触地
    for (int i = 0; i < 30; ++i)
        k.step();

    const auto &rb = k.body(0);
    double ke = 0.5 * 1.0 * rb.linear_velocity.length_sq();
    double pe = 1.0 * 9.81 * rb.position.y;
    QCHECK_NEAR(ke + pe, initial_pe, initial_pe * 0.01); // 1% 容差
}

QTEST(physics_no_tunneling_ground) {
    // 球体从高处落下撞击地面,不应穿透到地面以下。
    PhysicsKernel k(false, 1.0 / 60.0);
    k.set_gravity({0, -9.81, 0});

    RigidBody ground; ground.set_static(true);
    ground.position = {0, 0, 0};
    Collider gc; gc.type = ShapeType::AABB; gc.half_extents = {5, 0.5, 5};
    k.add_body(ground, gc);

    RigidBody b; b.set_mass(1.0);
    b.linear_damping = 0.0;
    b.angular_damping = 0.0;
    b.position = {0, 4.0, 0};
    Collider c; c.type = ShapeType::Sphere; c.radius = 0.5;
    k.add_body(b, c);

    for (int i = 0; i < 300; ++i) // 5s,足以反弹并稳定
        k.step();

    // 球体中心不应低于地面顶面(y=0.5)
    QCHECK(k.body(1).position.y > 0.5);
}

QTEST(physics_stacking_stable) {
    // 两个盒子堆叠,步进后应保持大致堆叠(下盒在上盒之下,不飞散)。
    PhysicsKernel k(false, 1.0 / 60.0);
    k.set_gravity({0, -9.81, 0});

    RigidBody ground; ground.set_static(true);
    ground.position = {0, 0, 0};
    Collider gc; gc.type = ShapeType::AABB; gc.half_extents = {5, 0.5, 5};
    k.add_body(ground, gc);

    // 下盒:中心 y=1.5,半高 0.5 → 底在 y=1.0(地面顶 0.5 之上)
    RigidBody lower; lower.set_mass(1.0);
    lower.position = {0, 1.5, 0};
    Collider lc; lc.type = ShapeType::AABB; lc.half_extents = {0.5, 0.5, 0.5};
    k.add_body(lower, lc);

    // 上盒:中心 y=2.6,叠在下盒上(下盒顶在 y=2.0)
    RigidBody upper; upper.set_mass(1.0);
    upper.position = {0, 2.6, 0};
    Collider uc; uc.type = ShapeType::AABB; uc.half_extents = {0.5, 0.5, 0.5};
    k.add_body(upper, uc);

    for (int i = 0; i < 120; ++i) // 2s
        k.step();

    // 上盒应仍在下盒之上(未穿透/飞散)
    QCHECK(k.body(2).position.y > k.body(1).position.y);
}
