#pragma once
#include <cmath>

namespace rvdlrsp::qpc
{

    struct Vec3
    {
        double x = 0, y = 0, z = 0;
        Vec3() = default;
        Vec3(double x_, double y_, double z_) : x(x_), y(y_), z(z_) {}

        Vec3 operator+(const Vec3 &o) const { return {x + o.x, y + o.y, z + o.z}; }
        Vec3 operator-(const Vec3 &o) const { return {x - o.x, y - o.y, z - o.z}; }
        Vec3 operator*(double s) const { return {x * s, y * s, z * s}; }
        Vec3 operator/(double s) const { return {x / s, y / s, z / s}; }
        Vec3 operator-() const { return {-x, -y, -z}; }

        Vec3 &operator+=(const Vec3 &o)
        {
            x += o.x;
            y += o.y;
            z += o.z;
            return *this;
        }
        Vec3 &operator-=(const Vec3 &o)
        {
            x -= o.x;
            y -= o.y;
            z -= o.z;
            return *this;
        }
        Vec3 &operator*=(double s)
        {
            x *= s;
            y *= s;
            z *= s;
            return *this;
        }

        double dot(const Vec3 &o) const { return x * o.x + y * o.y + z * o.z; }
        Vec3 cross(const Vec3 &o) const
        {
            return {y * o.z - z * o.y, z * o.x - x * o.z, x * o.y - y * o.x};
        }
        double length() const { return std::sqrt(dot(*this)); }
        double length_sq() const { return dot(*this); }
        Vec3 normalized() const
        {
            double l = length();
            return l > 1e-12 ? *this / l : Vec3{};
        }
    };

    struct Quat
    {
        double x = 0, y = 0, z = 0, w = 1;
        Quat() = default;
        Quat(double x_, double y_, double z_, double w_) : x(x_), y(y_), z(z_), w(w_) {}

        // 单位四元数（绕单位轴 angle 弧度）
        static Quat axis_angle(const Vec3 &axis, double angle)
        {
            double half = angle * 0.5;
            double s = std::sin(half);
            Vec3 a = axis.normalized();
            return {a.x * s, a.y * s, a.z * s, std::cos(half)};
        }

        // 四元数乘法（this * o）
        Quat operator*(const Quat &o) const
        {
            return {
                w * o.x + x * o.w + y * o.z - z * o.y,
                w * o.y - x * o.z + y * o.w + z * o.x,
                w * o.z + x * o.y - y * o.x + z * o.w,
                w * o.w - x * o.x - y * o.y - z * o.z};
        }

        // 标量乘法
        Quat operator*(double s) const
        {
            return {x * s, y * s, z * s, w * s};
        }

        Quat normalized() const
        {
            double l = std::sqrt(x * x + y * y + z * z + w * w);
            return l > 1e-12 ? Quat{x / l, y / l, z / l, w / l} : Quat{};
        }

        Quat conjugate() const { return {-x, -y, -z, w}; }

        // 四元数点积
        double dot(const Quat &o) const { return x * o.x + y * o.y + z * o.z + w * o.w; }

        // 用角速度 omega 积分 dt 秒：dq/dt = 0.5 * (0, omega) * q
        Quat integrate(const Vec3 &omega, double dt) const
        {
            Quat wq(omega.x, omega.y, omega.z, 0.0);
            Quat dq = wq * (*this) * 0.5;
            Quat r{w + dq.w * dt, x + dq.x * dt, y + dq.y * dt, z + dq.z * dt};
            return r.normalized();
        }

        // 旋转向量 v
        Vec3 rotate(const Vec3 &v) const
        {
            Quat p(v.x, v.y, v.z, 0.0);
            Quat qinv(-x, -y, -z, w);
            Quat r = (*this) * p * qinv;
            return {r.x, r.y, r.z};
        }
    };
}