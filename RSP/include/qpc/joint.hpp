#pragma once
#include <algorithm>
#include <cmath>
#include <limits>
#include "math.hpp"
#include "rigid_body.hpp"

namespace rvdlrsp::qpc
{

    enum class JointType
    {
        BallSocket,
        Hinge,
        Fixed,
        Prismatic,
        Distance
    };

    struct Joint
    {
        JointType type = JointType::BallSocket;
        size_t body_a = 0;
        size_t body_b = 0;
        Vec3 anchor;
        Vec3 axis{0, 1, 0};
        double stiffness = 0.2;

        // 限制
        double min_limit = -std::numeric_limits<double>::infinity();
        double max_limit = +std::numeric_limits<double>::infinity();
        double rest_distance = 0.0; // Distance 关节的静止距离

        // 马达（可选）：驱动到目标角度/位移
        double motor_target = 0.0;
        double motor_speed = 0.0;
        bool motor_enabled = false;
    };

    class JointSolver
    {
    public:
        // 球窝：把两刚体朝锚点对齐
        static void solve_ball_socket(RigidBody &a, RigidBody &b, const Joint &j)
        {
            double inv_mass_sum = a.inv_mass + b.inv_mass;
            if (inv_mass_sum < 1e-12)
                return;

            // 各刚体质心相对锚点的偏移（世界空间）
            Vec3 ra = j.anchor - a.position;
            Vec3 rb = j.anchor - b.position;

            // 让锚点对齐：a 移动 ra，b 移动 rb（按质量反比分配）
            Vec3 corr_a = ra * (a.inv_mass / inv_mass_sum);
            Vec3 corr_b = rb * (b.inv_mass / inv_mass_sum);

            a.position += corr_a * j.stiffness;
            b.position += corr_b * j.stiffness;
        }

        // 轴对齐：让 a/b 的局部轴在世界空间重合
        static void align_axes(RigidBody &a, RigidBody &b, const Joint &j)
        {
            Vec3 axis_a = a.orientation.rotate(j.axis);
            Vec3 axis_b = b.orientation.rotate(j.axis);
            Vec3 delta = axis_a.cross(axis_b);
            double sin_angle = delta.length();
            if (sin_angle < 1e-6)
                return;

            Vec3 n = delta / sin_angle;
            double angle = std::asin(std::min(1.0, sin_angle));
            double inv_inertia_sum = a.inv_inertia + b.inv_inertia;
            if (inv_inertia_sum < 1e-12)
                return;

            double corr = angle / inv_inertia_sum * 0.5;
            Quat qa = Quat::axis_angle(n, -corr * a.inv_inertia);
            Quat qb = Quat::axis_angle(n, corr * b.inv_inertia);
            a.orientation = (qa * a.orientation).normalized();
            b.orientation = (qb * b.orientation).normalized();
        }

        // 铰链：锚点对齐 + 限制绕轴旋转
        static void solve_hinge(RigidBody &a, RigidBody &b, const Joint &j)
        {
            solve_ball_socket(a, b, j);
            align_axes(a, b, j);
        }

        // 固定：锚点对齐 + 朝向完全对齐
        static void solve_fixed(RigidBody &a, RigidBody &b, const Joint &j)
        {
            solve_ball_socket(a, b, j);

            // 相对旋转 delta = b * inverse(a)，各修正一半
            Quat delta = (b.orientation * a.orientation.conjugate()).normalized();
            double angle = 2.0 * std::acos(std::min(1.0, std::abs(delta.w)));
            Vec3 axis(delta.x, delta.y, delta.z);
            double axis_len = axis.length();
            if (axis_len < 1e-12 || angle < 1e-6)
                return;

            axis = axis / axis_len;
            double inv_sum = a.inv_inertia + b.inv_inertia;
            if (inv_sum < 1e-12)
                return;

            double wa = b.inv_inertia / inv_sum;
            double wb = a.inv_inertia / inv_sum;
            Quat qa = Quat::axis_angle(axis, angle * 0.5 * wa);
            Quat qb = Quat::axis_angle(axis, -angle * 0.5 * wb);
            a.orientation = (qa * a.orientation).normalized();
            b.orientation = (qb * b.orientation).normalized();
        }

        // 棱柱：锚点沿轴滑动 + 朝向对齐 + 位移限制/马达
        static void solve_prismatic(RigidBody &a, RigidBody &b, const Joint &j)
        {
            align_axes(a, b, j);

            Vec3 axis_w = a.orientation.rotate(j.axis).normalized();
            Vec3 delta = b.position - a.position;

            double inv_mass_sum = a.inv_mass + b.inv_mass;
            if (inv_mass_sum < 1e-12)
                return;

            // 约束垂直于轴的分量
            Vec3 perp = delta - axis_w * delta.dot(axis_w);
            Vec3 corr = perp * j.stiffness;
            a.position += corr * (a.inv_mass / inv_mass_sum);
            b.position -= corr * (b.inv_mass / inv_mass_sum);

            // 沿轴位移限制 / 马达
            double slide = delta.dot(axis_w);
            double target = slide;
            if (j.motor_enabled)
                target = j.motor_target;
            else
            {
                if (slide < j.min_limit)
                    target = j.min_limit;
                else if (slide > j.max_limit)
                    target = j.max_limit;
            }

            double diff = slide - target;
            if (std::abs(diff) > 1e-9)
            {
                Vec3 lim = axis_w * (diff * j.stiffness);
                a.position += lim * (a.inv_mass / inv_mass_sum);
                b.position -= lim * (b.inv_mass / inv_mass_sum);
            }
        }

        // 距离：保持两刚体距离 = 目标距离
        static void solve_distance(RigidBody &a, RigidBody &b, const Joint &j)
        {
            Vec3 delta = b.position - a.position;
            double dist = delta.length();
            if (dist < 1e-12)
                return;

            Vec3 dir = delta / dist;
            double target = j.rest_distance;
            if (j.motor_enabled)
                target = j.motor_target;
            else
            {
                double lo = std::isfinite(j.min_limit) ? j.min_limit : target;
                double hi = std::isfinite(j.max_limit) ? j.max_limit : target;
                target = std::max(lo, std::min(hi, target));
            }

            double inv_mass_sum = a.inv_mass + b.inv_mass;
            if (inv_mass_sum < 1e-12)
                return;

            double corr = (dist - target) * j.stiffness;
            a.position += dir * (corr * (a.inv_mass / inv_mass_sum));
            b.position -= dir * (corr * (b.inv_mass / inv_mass_sum));
        }

        // 统一入口
        static void solve(RigidBody &a, RigidBody &b, const Joint &j)
        {
            switch (j.type)
            {
            case JointType::BallSocket:
                solve_ball_socket(a, b, j);
                break;
            case JointType::Hinge:
                solve_hinge(a, b, j);
                break;
            case JointType::Fixed:
                solve_fixed(a, b, j);
                break;
            case JointType::Prismatic:
                solve_prismatic(a, b, j);
                break;
            case JointType::Distance:
                solve_distance(a, b, j);
                break;
            }
        }
    };
}