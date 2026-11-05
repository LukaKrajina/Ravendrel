#pragma once
#include "math.hpp"

namespace rvdlrsp::qpc
{

    struct RigidBody
    {
        // ─── 质量属性 ──────────────────────────────────────
        double mass = 1.0;
        double inv_mass = 1.0;
        double inertia = 1.0; // 标量惯性（绕质心）
        double inv_inertia = 1.0;
        double restitution = 0.3; // 弹性系数 [0,1]
        double friction = 0.6;    // 库仑摩擦系数
        double linear_damping = 0.05;  // 线性阻尼系数 (1/s),物理衰减与步长无关
        double angular_damping = 0.05; // 角阻尼系数 (1/s)

        // ─── 运动状态 ──────────────────────────────────────
        Vec3 position;
        Quat orientation;
        Vec3 linear_velocity;
        Vec3 angular_velocity;

        // ─── 累积力/力矩（每步清零） ────────────────────────
        Vec3 force;
        Vec3 torque;

        bool is_static = false;

        void set_mass(double m, double I = 1.0)
        {
            mass = m;
            inertia = I;
            inv_mass = (is_static || m <= 0.0) ? 0.0 : 1.0 / m;
            inv_inertia = (is_static || I <= 0.0) ? 0.0 : 1.0 / I;
        }

        void set_static(bool s)
        {
            is_static = s;
            set_mass(mass, inertia);
        }

        void apply_force(const Vec3 &f) { force += f; }

        void apply_force_at_point(const Vec3 &f, const Vec3 &rel_point)
        {
            force += f;
            torque += rel_point.cross(f);
        }

        void apply_torque(const Vec3 &t) { torque += t; }

        void clear_forces()
        {
            force = Vec3{};
            torque = Vec3{};
        }
    };
}