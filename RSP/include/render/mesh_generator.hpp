#pragma once

// ============================================================================
// mesh_generator —— 程序化几何体生成
// ----------------------------------------------------------------------------
// 提供机器人零件建模所需的基础几何体：球体 / 立方体 / 圆柱体 / 胶囊体。
// 供渲染（QVulkanWindow）与机器人装配（Robot）共用。
// ============================================================================

#include <cmath>
#include "scene.hpp"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace rvdlrsp::render
{

    // ─── 球体 ────────────────────────────────────────────────────────
    inline Mesh make_sphere(float radius, int stacks = 16, int slices = 24)
    {
        Mesh m;
        m.name = "sphere";
        for (int i = 0; i <= stacks; ++i)
        {
            double phi = M_PI * i / stacks;
            double y = std::cos(phi), r = std::sin(phi);
            for (int j = 0; j <= slices; ++j)
            {
                double th = 2.0 * M_PI * j / slices;
                double x = r * std::cos(th), z = r * std::sin(th);
                Vertex v;
                v.position = {x * radius, y * radius, z * radius};
                v.normal = {x, y, z};
                v.r = static_cast<float>(0.5 + 0.5 * y);
                v.g = static_cast<float>(0.4 + 0.4 * (1.0 - y));
                v.b = 0.9f;
                m.vertices.push_back(v);
            }
        }
        for (int i = 0; i < stacks; ++i)
            for (int j = 0; j < slices; ++j)
            {
                uint32_t a = i * (slices + 1) + j, b = a + slices + 1;
                m.indices.insert(m.indices.end(), {a, b, a + 1, a + 1, b, b + 1});
            }
        return m;
    }

    // ─── 立方体（轴对齐，每面独立顶点 + 正确法线）─────────────────
    inline Mesh make_cube(float scale)
    {
        Mesh m;
        m.name = "cube";
        float h = 0.5f * scale;

        // 6 个面：(法线 n, 面内两正交轴 u/v)，每面 4 顶点 + 2 三角形
        struct Face { qpc::Vec3 n, u, v; };
        Face faces[6] = {
            {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}},    // +X
            {{-1, 0, 0}, {0, 0, 1}, {0, 1, 0}},   // -X
            {{0, 1, 0}, {0, 0, 1}, {1, 0, 0}},    // +Y
            {{0, -1, 0}, {1, 0, 0}, {0, 0, 1}},   // -Y
            {{0, 0, 1}, {0, 1, 0}, {1, 0, 0}},    // +Z
            {{0, 0, -1}, {1, 0, 0}, {0, 1, 0}},   // -Z
        };

        for (const auto &f : faces)
        {
            uint32_t base = static_cast<uint32_t>(m.vertices.size());
            qpc::Vec3 corners[4] = {
                f.n * h - f.u * h - f.v * h,
                f.n * h + f.u * h - f.v * h,
                f.n * h + f.u * h + f.v * h,
                f.n * h - f.u * h + f.v * h,
            };
            for (const auto &c : corners)
            {
                Vertex v;
                v.position = c;
                v.normal = f.n;
                v.r = v.g = v.b = 0.7f;
                m.vertices.push_back(v);
            }
            m.indices.insert(m.indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
        }
        return m;
    }

    // ─── 圆柱体（沿 Y 轴，从 -h/2 到 +h/2）─────────────────────────
    inline Mesh make_cylinder(float radius, float height, int segments = 16)
    {
        Mesh m;
        m.name = "cylinder";
        float half = height * 0.5f;

        // 顶面 + 底面圆心
        uint32_t top_center = static_cast<uint32_t>(m.vertices.size());
        Vertex tc;
        tc.position = {0, half, 0};
        tc.normal = {0, 1, 0};
        tc.r = tc.g = tc.b = 0.7f;
        m.vertices.push_back(tc);

        uint32_t bottom_center = static_cast<uint32_t>(m.vertices.size());
        Vertex bc;
        bc.position = {0, -half, 0};
        bc.normal = {0, -1, 0};
        bc.r = bc.g = bc.b = 0.7f;
        m.vertices.push_back(bc);

        // 侧面顶点（上环 + 下环）
        std::vector<uint32_t> top_ring, bottom_ring;
        for (int i = 0; i <= segments; ++i)
        {
            double th = 2.0 * M_PI * i / segments;
            float x = radius * static_cast<float>(std::cos(th));
            float z = radius * static_cast<float>(std::sin(th));
            float u = static_cast<float>(i) / segments;

            Vertex tv;
            tv.position = {x, half, z};
            tv.normal = {x / radius, 0, z / radius};
            tv.u = u; tv.v = 1.0f;
            tv.r = tv.g = tv.b = 0.7f;
            top_ring.push_back(static_cast<uint32_t>(m.vertices.size()));
            m.vertices.push_back(tv);

            Vertex bv;
            bv.position = {x, -half, z};
            bv.normal = {x / radius, 0, z / radius};
            bv.u = u; bv.v = 0.0f;
            bv.r = bv.g = bv.b = 0.7f;
            bottom_ring.push_back(static_cast<uint32_t>(m.vertices.size()));
            m.vertices.push_back(bv);
        }

        // 侧面（两个三角形/段，绕序朝外）
        for (int i = 0; i < segments; ++i)
        {
            uint32_t t0 = top_ring[i], t1 = top_ring[i + 1];
            uint32_t b0 = bottom_ring[i], b1 = bottom_ring[i + 1];
            m.indices.insert(m.indices.end(), {t0, t1, b1, t0, b1, b0});
        }

        // 顶面 + 底面
        for (int i = 0; i < segments; ++i)
        {
            m.indices.insert(m.indices.end(), {top_center, top_ring[i + 1], top_ring[i]});
            m.indices.insert(m.indices.end(), {bottom_center, bottom_ring[i], bottom_ring[i + 1]});
        }

        return m;
    }

    // ─── 胶囊体（沿 Y 轴，圆柱 + 两端半球）─────────────────────────
    inline Mesh make_capsule(float radius, float height, int segments = 16)
    {
        Mesh m;
        m.name = "capsule";
        float half_cyl = height * 0.5f;         // 中部圆柱半高
        int stacks = segments / 2;              // 每端半球的分段数
        if (stacks < 1)
            stacks = 1;

        // 赤道环（上下各一，与中部圆柱、两端半球共享顶点）
        std::vector<uint32_t> top_ring, bottom_ring;
        top_ring.reserve(segments + 1);
        bottom_ring.reserve(segments + 1);
        for (int i = 0; i <= segments; ++i)
        {
            double th = 2.0 * M_PI * i / segments;
            float x = radius * static_cast<float>(std::cos(th));
            float z = radius * static_cast<float>(std::sin(th));

            Vertex tv;
            tv.position = {x, half_cyl, z};
            tv.normal = {x / radius, 0.0f, z / radius};
            tv.r = tv.g = tv.b = 0.7f;
            top_ring.push_back(static_cast<uint32_t>(m.vertices.size()));
            m.vertices.push_back(tv);

            Vertex bv;
            bv.position = {x, -half_cyl, z};
            bv.normal = {x / radius, 0.0f, z / radius};
            bv.r = bv.g = bv.b = 0.7f;
            bottom_ring.push_back(static_cast<uint32_t>(m.vertices.size()));
            m.vertices.push_back(bv);
        }

        // 中部圆柱侧面（绕序朝外）
        for (int i = 0; i < segments; ++i)
        {
            uint32_t t0 = top_ring[i], t1 = top_ring[i + 1];
            uint32_t b0 = bottom_ring[i], b1 = bottom_ring[i + 1];
            m.indices.insert(m.indices.end(), {t0, t1, b1, t0, b1, b0});
        }

        // 半球（从赤道环向顶点逐层收敛）
        auto add_hemisphere = [&](const std::vector<uint32_t> &equator, int sign)
        {
            // 顶点（北极 / 南极）
            uint32_t apex = static_cast<uint32_t>(m.vertices.size());
            Vertex ap;
            ap.position = {0.0f, sign * (half_cyl + radius), 0.0f};
            ap.normal = {0.0f, static_cast<float>(sign), 0.0f};
            ap.r = ap.g = ap.b = 0.7f;
            m.vertices.push_back(ap);

            // 中间层（赤道 φ=0 → 顶点 φ=π/2）
            std::vector<uint32_t> prev = equator;
            for (int s = 1; s < stacks; ++s)
            {
                double phi = M_PI * 0.5 * static_cast<double>(s) / stacks;
                double cphi = std::cos(phi);
                double sphi = std::sin(phi);
                float r = radius * static_cast<float>(cphi);
                float y = half_cyl + sign * radius * static_cast<float>(sphi);
                float ny = sign * static_cast<float>(sphi);

                std::vector<uint32_t> cur;
                cur.reserve(segments + 1);
                for (int i = 0; i <= segments; ++i)
                {
                    double th = 2.0 * M_PI * i / segments;
                    double cx = std::cos(th);
                    double cz = std::sin(th);

                    Vertex v;
                    v.position = {r * cx, y, r * cz};
                    v.normal = {cx * cphi, ny, cz * cphi};
                    v.r = v.g = v.b = 0.7f;
                    cur.push_back(static_cast<uint32_t>(m.vertices.size()));
                    m.vertices.push_back(v);
                }

                // 上下半球绕序镜像（保证都朝外）
                for (int i = 0; i < segments; ++i)
                {
                    uint32_t a0 = prev[i], a1 = prev[i + 1];
                    uint32_t b0 = cur[i], b1 = cur[i + 1];
                    if (sign > 0)
                        m.indices.insert(m.indices.end(), {a0, b0, a1, a1, b0, b1});
                    else
                        m.indices.insert(m.indices.end(), {a0, a1, b0, a1, b1, b0});
                }
                prev = cur;
            }

            // 最后一层（最靠近顶点）与顶点闭合
            for (int i = 0; i < segments; ++i)
            {
                if (sign > 0)
                    m.indices.insert(m.indices.end(), {prev[i], apex, prev[i + 1]});
                else
                    m.indices.insert(m.indices.end(), {prev[i], prev[i + 1], apex});
            }
        };

        add_hemisphere(top_ring, +1);      // 上半球
        add_hemisphere(bottom_ring, -1);   // 下半球

        return m;
    }

    // ─── 圆锥（沿 Y 轴，顶点在 +h/2，底面在 -h/2）─────────────────
    inline Mesh make_cone(float radius, float height, int segments = 24)
    {
        Mesh m;
        m.name = "cone";
        float half = height * 0.5f;
        double slope = std::sqrt(radius * radius + height * height);
        float cos_slope = static_cast<float>(height / slope);
        float sin_slope = static_cast<float>(radius / slope);

        // 顶点（上）
        uint32_t apex = static_cast<uint32_t>(m.vertices.size());
        Vertex ap;
        ap.position = {0, half, 0};
        ap.normal = {0, 1, 0};
        ap.u = 0.5f; ap.v = 1.0f;
        ap.r = ap.g = ap.b = 0.7f;
        m.vertices.push_back(ap);

        // 底面圆心（下）
        uint32_t base_center = static_cast<uint32_t>(m.vertices.size());
        Vertex bc;
        bc.position = {0, -half, 0};
        bc.normal = {0, -1, 0};
        bc.u = 0.5f; bc.v = 0.0f;
        bc.r = bc.g = bc.b = 0.7f;
        m.vertices.push_back(bc);

        // 侧面环（底面高度，法线沿斜面）+ 底面环（法线朝下）
        std::vector<uint32_t> side_ring, base_ring;
        for (int i = 0; i <= segments; ++i)
        {
            double th = 2.0 * M_PI * i / segments;
            float x = radius * static_cast<float>(std::cos(th));
            float z = radius * static_cast<float>(std::sin(th));
            float u = static_cast<float>(i) / segments;

            Vertex sv;
            sv.position = {x, -half, z};
            sv.normal = {x / radius * cos_slope, sin_slope, z / radius * cos_slope};
            sv.u = u; sv.v = 0.0f;
            sv.r = sv.g = sv.b = 0.7f;
            side_ring.push_back(static_cast<uint32_t>(m.vertices.size()));
            m.vertices.push_back(sv);

            Vertex bv;
            bv.position = {x, -half, z};
            bv.normal = {0, -1, 0};
            bv.u = x / (2.0f * radius) + 0.5f;
            bv.v = z / (2.0f * radius) + 0.5f;
            bv.r = bv.g = bv.b = 0.7f;
            base_ring.push_back(static_cast<uint32_t>(m.vertices.size()));
            m.vertices.push_back(bv);
        }

        // 侧面（绕序朝外）
        for (int i = 0; i < segments; ++i)
            m.indices.insert(m.indices.end(), {apex, side_ring[i + 1], side_ring[i]});

        // 底面（绕序朝下）
        for (int i = 0; i < segments; ++i)
            m.indices.insert(m.indices.end(), {base_center, base_ring[i], base_ring[i + 1]});

        return m;
    }

    // ─── 圆环（绕 Y 轴，主半径 R，管半径 r）──────────────────────
    inline Mesh make_torus(float major_radius, float minor_radius,
                           int major_seg = 24, int minor_seg = 12)
    {
        Mesh m;
        m.name = "torus";
        for (int i = 0; i <= major_seg; ++i)
        {
            double th = 2.0 * M_PI * i / major_seg;
            double cth = std::cos(th), sth = std::sin(th);
            for (int j = 0; j <= minor_seg; ++j)
            {
                double ph = 2.0 * M_PI * j / minor_seg;
                double cph = std::cos(ph), sph = std::sin(ph);
                double r = major_radius + minor_radius * cph;

                Vertex v;
                v.position = {r * cth, minor_radius * sph, r * sth};
                v.normal = {static_cast<float>(cth * cph), static_cast<float>(sph), static_cast<float>(sth * cph)};
                v.u = static_cast<float>(i) / major_seg;
                v.v = static_cast<float>(j) / minor_seg;
                v.r = v.g = v.b = 0.7f;
                m.vertices.push_back(v);
            }
        }
        for (int i = 0; i < major_seg; ++i)
            for (int j = 0; j < minor_seg; ++j)
            {
                uint32_t a = static_cast<uint32_t>(i * (minor_seg + 1) + j);
                uint32_t b = a + minor_seg + 1;
                m.indices.insert(m.indices.end(), {a, b, a + 1, a + 1, b, b + 1});
            }
        return m;
    }

    // ─── 平面（XZ 平面，中心在原点，边长 size）───────────────────
    inline Mesh make_plane(float size)
    {
        Mesh m;
        m.name = "plane";
        float h = size * 0.5f;
        Vertex v[4] = {
            {{-h, 0, -h}, {0, 1, 0}, 0, 0, 0.7f, 0.7f, 0.7f},
            {{h, 0, -h}, {0, 1, 0}, 1, 0, 0.7f, 0.7f, 0.7f},
            {{h, 0, h}, {0, 1, 0}, 1, 1, 0.7f, 0.7f, 0.7f},
            {{-h, 0, h}, {0, 1, 0}, 0, 1, 0.7f, 0.7f, 0.7f}};
        for (auto &x : v)
            m.vertices.push_back(x);
        m.indices.insert(m.indices.end(), {0, 1, 2, 0, 2, 3});
        return m;
    }
}
