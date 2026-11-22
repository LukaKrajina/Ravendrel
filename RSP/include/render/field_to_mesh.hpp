#pragma once
#include <cmath>
#include <algorithm>
#include "scene.hpp"
#include "spacetime/SliceTopology.hpp"

namespace rvdlrsp::render
{

    inline Mesh make_heightfield_mesh(const rvdl::spacetime::Grid &g,
                                      const std::vector<double> &field,
                                      double vmax, double height_scale = 1.0)
    {
        Mesh m;
        m.name = "tachyon_field";
        const int nx = g.n[0], ny = g.n[1];
        const double hx = g.dx(0), hy = g.dx(1);

        auto at = [&](int i, int j) -> double
        {
            return field[static_cast<size_t>(g.index(i, j, 0))];
        };

        auto grad = [&](int i, int j, double &gx, double &gz)
        {
            int im = (i - 1 + nx) % nx, ip = (i + 1) % nx;
            int jm = (j - 1 + ny) % ny, jp = (j + 1) % ny;
            gx = (at(ip, j) - at(im, j)) / (2.0 * hx);
            gz = (at(i, jp) - at(i, jm)) / (2.0 * hy);
        };

        auto color = [&](double v, float &r, float &gg, float &b)
        {
            double t = std::clamp(v / (vmax + 1e-12), -1.0, 1.0);
            r = static_cast<float>(0.5 + 0.5 * t);
            gg = static_cast<float>(0.5 - 0.5 * std::abs(t));
            b = static_cast<float>(0.5 - 0.5 * t);
        };

        const double cx = 0.5 * nx * hx, cy = 0.5 * ny * hy;
        for (int j = 0; j < ny; ++j)
            for (int i = 0; i < nx; ++i)
            {
                double v = at(i, j);
                double dhx, dhz;
                grad(i, j, dhx, dhz);
                Vertex vert;
                vert.position = {static_cast<float>(i * hx - cx),
                                 static_cast<float>(v * height_scale),
                                 static_cast<float>(j * hy - cy)};

                double nl = std::sqrt(dhx * dhx + 1.0 + dhz * dhz);
                vert.normal = {static_cast<float>(-dhx / nl),
                               static_cast<float>(1.0 / nl),
                               static_cast<float>(-dhz / nl)};
                color(v, vert.r, vert.g, vert.b);
                m.vertices.push_back(vert);
            }

        auto vid = [nx](int i, int j) -> uint32_t
        { return static_cast<uint32_t>(j * nx + i); };
        for (int j = 0; j < ny - 1; ++j)
            for (int i = 0; i < nx - 1; ++i)
            {
                uint32_t a = vid(i, j), b = vid(i + 1, j), c = vid(i, j + 1), d = vid(i + 1, j + 1);
                m.indices.insert(m.indices.end(), {a, c, b, b, c, d});
            }
        return m;
    }
}