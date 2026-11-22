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
    QCHECK_NEAR(c.penetration, 0.5, 1e-9);
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
    QCHECK(a.position.y < 0.0);
    QCHECK(b.position.y > 0.0);
}


QTEST(physics_damping_decays) {
    PhysicsKernel k(false, 1.0 / 60.0);
    k.set_gravity({0, 0, 0});
    RigidBody b; b.set_mass(1.0);
    b.linear_damping = 1.0;
    b.angular_damping = 1.0;
    b.linear_velocity = {1, 0, 0};
    Collider c; c.type = ShapeType::Sphere; c.radius = 0.5;
    k.add_body(b, c);
    for (int i = 0; i < 60; ++i)
        k.step();
    double v = k.body(0).linear_velocity.x;
    QCHECK(v < 0.5);
    QCHECK(v > 0.2);
}

QTEST(physics_damping_dt_independent) {
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


QTEST(physics_energy_conservation) {
    PhysicsKernel k(false, 1.0 / 60.0);
    k.set_gravity({0, -9.81, 0});
    RigidBody b; b.set_mass(1.0);
    b.linear_damping = 0.0;
    b.angular_damping = 0.0;
    b.position = {0, 5.0, 0};
    Collider c; c.type = ShapeType::Sphere; c.radius = 0.5;
    k.add_body(b, c);

    double initial_pe = 1.0 * 9.81 * 5.0;

    for (int i = 0; i < 30; ++i)
        k.step();

    const auto &rb = k.body(0);
    double ke = 0.5 * 1.0 * rb.linear_velocity.length_sq();
    double pe = 1.0 * 9.81 * rb.position.y;
    QCHECK_NEAR(ke + pe, initial_pe, initial_pe * 0.01);
}

QTEST(physics_no_tunneling_ground) {
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

    for (int i = 0; i < 300; ++i)
        k.step();

    QCHECK(k.body(1).position.y > 0.5);
}

QTEST(physics_stacking_stable) {
    PhysicsKernel k(false, 1.0 / 60.0);
    k.set_gravity({0, -9.81, 0});

    RigidBody ground; ground.set_static(true);
    ground.position = {0, 0, 0};
    Collider gc; gc.type = ShapeType::AABB; gc.half_extents = {5, 0.5, 5};
    k.add_body(ground, gc);

    RigidBody lower; lower.set_mass(1.0);
    lower.position = {0, 1.5, 0};
    Collider lc; lc.type = ShapeType::AABB; lc.half_extents = {0.5, 0.5, 0.5};
    k.add_body(lower, lc);

    RigidBody upper; upper.set_mass(1.0);
    upper.position = {0, 2.6, 0};
    Collider uc; uc.type = ShapeType::AABB; uc.half_extents = {0.5, 0.5, 0.5};
    k.add_body(upper, uc);

    for (int i = 0; i < 120; ++i)
        k.step();

    QCHECK(k.body(2).position.y > k.body(1).position.y);
}