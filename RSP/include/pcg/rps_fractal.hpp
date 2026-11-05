#pragma once
#include <cmath>
#include <vector>
#include <cstdint>
#include <array>
#include <algorithm>
#include "render/scene.hpp"

namespace rvdlrsp::pcg
{

    // RPSions 代数元素
    struct RPSion
    {
        double x = 0, y = 0, z = 0;

        static RPSion mul(const RPSion &a, const RPSion &b)
        {
            return {
                (a.x + a.z) * b.x + a.x * b.z,
                a.y * b.x + (a.x + a.y) * b.y,
                a.z * b.y + (a.y + a.z) * b.z};
        }
        static RPSion sq(const RPSion &w)
        {
            return {w.x * w.x + 2 * w.x * w.z,
                    w.y * w.y + 2 * w.x * w.y,
                    w.z * w.z + 2 * w.y * w.z};
        }
        static RPSion add(const RPSion &a, const RPSion &b)
        {
            return {a.x + b.x, a.y + b.y, a.z + b.z};
        }
        double norm() const { return std::sqrt(x * x + y * y + z * z); }
    };

    // 逃逸计数
    inline int escape_count(const RPSion &c, int max_iter = 256,
                            double bailout = 4.0, double fixed_threshold = 1e-4)
    {
        RPSion w{0, 0, 0};
        for (int m = 1; m <= max_iter; ++m)
        {
            RPSion next = RPSion::add(RPSion::sq(w), c);
            RPSion diff{next.x - w.x, next.y - w.y, next.z - w.z};
            if (diff.norm() < fixed_threshold)
                return 0;
            w = next;
            if (w.norm() > bailout)
                return m;
        }
        return 0;
    }

    // 二维截面逃逸计数网格
    inline std::vector<int> escape_grid(double u0, double u1, double v0, double v1,
                                        double w_fixed, int W, int H, int max_iter = 256)
    {
        std::vector<int> out(static_cast<size_t>(W) * H);
        for (int j = 0; j < H; ++j)
        {
            double v = v0 + (v1 - v0) * j / (H - 1);
            for (int i = 0; i < W; ++i)
            {
                double u = u0 + (u1 - u0) * i / (W - 1);
                out[static_cast<size_t>(j) * W + i] = escape_count({u, v, w_fixed}, max_iter);
            }
        }
        return out;
    }

    inline render::Mesh heightfield_mesh(const std::vector<int> &grid, int W, int H,
                                         double x0, double x1, double z0, double z1,
                                         double height_scale = 0.02)
    {
        render::Mesh m;
        m.name = "rps_fractal";
        auto color = [](int cnt)
        {
            float t = cnt <= 0 ? 0.0f : 1.0f - 1.0f / (1.0f + 0.05f * cnt);
            return std::array<float, 3>{0.1f + 0.9f * t, 0.1f + 0.5f * (1.0f - t), 0.3f + 0.7f * t};
        };

        for (int j = 0; j < H; ++j)
        {
            float fz = static_cast<float>(j) / (H - 1);
            for (int i = 0; i < W; ++i)
            {
                float fx = static_cast<float>(i) / (W - 1);
                int cnt = grid[static_cast<size_t>(j) * W + i];
                render::Vertex v;
                v.position = {x0 + (x1 - x0) * fx, height_scale * cnt, z0 + (z1 - z0) * fz};
                v.normal = {0, 1, 0};
                v.u = fx;
                v.v = fz;
                auto c = color(cnt);
                v.r = c[0];
                v.g = c[1];
                v.b = c[2];
                m.vertices.push_back(v);
            }
        }

        double dx = (x1 - x0) / (W - 1), dz = (z1 - z0) / (H - 1);
        for (int j = 0; j < H; ++j)
            for (int i = 0; i < W; ++i)
            {
                int il = std::max(i - 1, 0), ir = std::min(i + 1, W - 1);
                int jd = std::max(j - 1, 0), ju = std::min(j + 1, H - 1);
                double hL = grid[static_cast<size_t>(j) * W + il];
                double hR = grid[static_cast<size_t>(j) * W + ir];
                double hD = grid[static_cast<size_t>(jd) * W + i];
                double hU = grid[static_cast<size_t>(ju) * W + i];
                double gx = (hR - hL) / (2 * dx) * height_scale;
                double gz = (hU - hD) / (2 * dz) * height_scale;
                rvdlrsp::qpc::Vec3 n{-gx, 1.0, -gz};
                m.vertices[static_cast<size_t>(j) * W + i].normal = n.normalized();
            }

        for (int j = 0; j < H - 1; ++j)
            for (int i = 0; i < W - 1; ++i)
            {
                uint32_t a = static_cast<uint32_t>(j * W + i), b = a + W;
                m.indices.insert(m.indices.end(), {a, b, a + 1, a + 1, b, b + 1});
            }
        return m;
    }
}