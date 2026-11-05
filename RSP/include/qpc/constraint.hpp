#pragma once
#include <algorithm>
#include "math.hpp"
#include "rigid_body.hpp"
#include "collision.hpp"

namespace rvdlrsp::qpc
{

    class ConstraintSolver
    {
    public:
        // 冲量求解：沿接触法线施加冲量，含弹性、摩擦与角冲量
        static void solve_contact(RigidBody &a, RigidBody &b, const Contact &c)
        {
            Vec3 rel_vel = b.linear_velocity - a.linear_velocity;
            double vel_along_normal = rel_vel.dot(c.normal);
            if (vel_along_normal > 0.0)
                return;

            double e = std::min(a.restitution, b.restitution);
            double inv_mass_sum = a.inv_mass + b.inv_mass;
            if (inv_mass_sum < 1e-12)
                return;

            // 接触点相对质心的力臂
            Vec3 ra = c.point - a.position;
            Vec3 rb = c.point - b.position;

            // 法线冲量
            double j = -(1.0 + e) * vel_along_normal / inv_mass_sum;
            Vec3 impulse = c.normal * j;
            a.linear_velocity -= impulse * a.inv_mass;
            b.linear_velocity += impulse * b.inv_mass;

            // 角冲量（力矩 = 力臂 × 冲量）
            a.angular_velocity -= ra.cross(impulse) * a.inv_inertia;
            b.angular_velocity += rb.cross(impulse) * b.inv_inertia;

            // 摩擦冲量（沿切向）
            Vec3 tangent = rel_vel - c.normal * vel_along_normal;
            double tangent_len = tangent.length();
            if (tangent_len > 1e-12)
            {
                Vec3 t = tangent / tangent_len;
                double jt = -rel_vel.dot(t) / inv_mass_sum;
                double mu = std::sqrt(a.friction * b.friction);
                double max_jt = std::abs(j) * mu;
                jt = std::max(-max_jt, std::min(jt, max_jt));
                Vec3 friction_impulse = t * jt;
                a.linear_velocity -= friction_impulse * a.inv_mass;
                b.linear_velocity += friction_impulse * b.inv_mass;
                a.angular_velocity -= ra.cross(friction_impulse) * a.inv_inertia;
                b.angular_velocity += rb.cross(friction_impulse) * b.inv_inertia;
            }
        }

        // 位置修正：消除穿透
        static void positional_correction(RigidBody &a, RigidBody &b, const Contact &c)
        {
            const double percent = 0.8; // 修正比例
            const double slop = 0.01;   // 允许穿透
            double inv_mass_sum = a.inv_mass + b.inv_mass;
            if (inv_mass_sum < 1e-12)
                return;

            double correction = std::max(c.penetration - slop, 0.0) / inv_mass_sum * percent;
            Vec3 corr = c.normal * correction;
            a.position -= corr * a.inv_mass;
            b.position += corr * b.inv_mass;
        }
    };
}