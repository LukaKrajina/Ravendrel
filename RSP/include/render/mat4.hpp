#pragma once
#include <cmath>
#include <algorithm>
#include <array>
#include "qpc/math.hpp"

namespace rvdlrsp::render
{

    struct Mat4
    {
        // 列主序：m[col][row]
        float m[16] = {
            1, 0, 0, 0,
            0, 1, 0, 0,
            0, 0, 1, 0,
            0, 0, 0, 1};

        static Mat4 identity() { return Mat4{}; }

        static Mat4 perspective(double fov_deg, double aspect, double _near, double _far)
        {
            Mat4 r;
            double f = 1.0 / std::tan(fov_deg * 0.5 * 3.141592653589793 / 180.0);
            r.m[0] = static_cast<float>(f / aspect);
            r.m[5] = static_cast<float>(f);
            r.m[10] = static_cast<float>((_far + _near) / (_near - _far));
            r.m[11] = -1.0f;
            r.m[14] = static_cast<float>((2.0 * _far * _near) / (_near - _far));
            r.m[15] = 0.0f;
            return r;
        }

        static Mat4 look_at(const qpc::Vec3 &eye, const qpc::Vec3 &target, const qpc::Vec3 &up)
        {
            qpc::Vec3 f = (target - eye).normalized();
            qpc::Vec3 s = f.cross(up).normalized();
            qpc::Vec3 u = s.cross(f);

            Mat4 r;
            r.m[0] = static_cast<float>(s.x);
            r.m[4] = static_cast<float>(s.y);
            r.m[8] = static_cast<float>(s.z);
            r.m[1] = static_cast<float>(u.x);
            r.m[5] = static_cast<float>(u.y);
            r.m[9] = static_cast<float>(u.z);
            r.m[2] = static_cast<float>(-f.x);
            r.m[6] = static_cast<float>(-f.y);
            r.m[10] = static_cast<float>(-f.z);
            r.m[12] = static_cast<float>(-s.dot(eye));
            r.m[13] = static_cast<float>(-u.dot(eye));
            r.m[14] = static_cast<float>(f.dot(eye));
            return r;
        }

        static Mat4 model(const qpc::Vec3 &pos, const qpc::Quat &orient, const qpc::Vec3 &scale)
        {
            // 四元数 → 旋转矩阵
            double x = orient.x, y = orient.y, z = orient.z, w = orient.w;
            Mat4 r;
            r.m[0] = static_cast<float>((1 - 2 * (y * y + z * z)) * scale.x);
            r.m[1] = static_cast<float>((2 * (x * y + w * z)) * scale.x);
            r.m[2] = static_cast<float>((2 * (x * z - w * y)) * scale.x);
            r.m[4] = static_cast<float>((2 * (x * y - w * z)) * scale.y);
            r.m[5] = static_cast<float>((1 - 2 * (x * x + z * z)) * scale.y);
            r.m[6] = static_cast<float>((2 * (y * z + w * x)) * scale.y);
            r.m[8] = static_cast<float>((2 * (x * z + w * y)) * scale.z);
            r.m[9] = static_cast<float>((2 * (y * z - w * x)) * scale.z);
            r.m[10] = static_cast<float>((1 - 2 * (x * x + y * y)) * scale.z);
            r.m[12] = static_cast<float>(pos.x);
            r.m[13] = static_cast<float>(pos.y);
            r.m[14] = static_cast<float>(pos.z);
            return r;
        }

        Mat4 operator*(const Mat4 &o) const
        {
            Mat4 r;
            for (int c = 0; c < 4; ++c)
            {
                for (int row = 0; row < 4; ++row)
                {
                    r.m[c * 4 + row] =
                        m[0 * 4 + row] * o.m[c * 4 + 0] +
                        m[1 * 4 + row] * o.m[c * 4 + 1] +
                        m[2 * 4 + row] * o.m[c * 4 + 2] +
                        m[3 * 4 + row] * o.m[c * 4 + 3];
                }
            }
            return r;
        }

        // 4x4 矩阵求逆（Gauss-Jordan 消元，通用，含仿射变换）
        Mat4 inverse() const
        {
            float a[4][8];
            for (int i = 0; i < 4; ++i)
                for (int j = 0; j < 4; ++j)
                {
                    a[i][j] = m[j * 4 + i];          // 转置为行主序
                    a[i][j + 4] = (i == j) ? 1.0f : 0.0f;
                }

            for (int col = 0; col < 4; ++col)
            {
                int pivot = col;
                for (int i = col + 1; i < 4; ++i)
                    if (std::fabs(a[i][col]) > std::fabs(a[pivot][col]))
                        pivot = i;
                if (std::fabs(a[pivot][col]) < 1e-12f)
                    return Mat4::identity();
                if (pivot != col)
                    for (int j = 0; j < 8; ++j)
                        std::swap(a[pivot][j], a[col][j]);
                float d = a[col][col];
                for (int j = 0; j < 8; ++j)
                    a[col][j] /= d;
                for (int i = 0; i < 4; ++i)
                {
                    if (i == col)
                        continue;
                    float f = a[i][col];
                    for (int j = 0; j < 8; ++j)
                        a[i][j] -= f * a[col][j];
                }
            }

            Mat4 out;
            for (int i = 0; i < 4; ++i)
                for (int j = 0; j < 4; ++j)
                    out.m[j * 4 + i] = a[i][j + 4];
            return out;
        }

        const float *data() const { return m; }
    };
}