#pragma once
#include <fstream>
#include <string>
#include <vector>
#include <algorithm>
#include "SliceTopology.hpp"

namespace rvdl::spacetime
{

    // 2D 场 → CSV；1D 场退化为单行
    inline void write_field_csv(const std::string &path, const Grid &g,
                                const std::vector<double> &f)
    {
        std::ofstream os(path);
        int nx = g.n[0], ny = g.n[1];
        for (int j = 0; j < ny; ++j)
        {
            for (int i = 0; i < nx; ++i)
            {
                os << f[static_cast<size_t>(g.index(i, j, 0))];
                if (i + 1 < nx)
                    os << ",";
            }
            os << "\n";
        }
    }

    // 1D 场 → CSV
    inline void write_field_1d_csv(const std::string &path, const Grid &g,
                                   const std::vector<double> &f)
    {
        std::ofstream os(path);
        os << "x,f\n";
        int nx = g.n[0];
        for (int i = 0; i < nx; ++i)
            os << i * g.dx(0) << "," << f[static_cast<size_t>(i)] << "\n";
    }

    // 2D 场 → PGM 灰度图
    inline void write_field_pgm(const std::string &path, const Grid &g,
                                const std::vector<double> &f, double vmax)
    {
        int nx = g.n[0], ny = g.n[1];
        std::ofstream os(path, std::ios::binary);
        os << "P5\n"
           << nx << " " << ny << "\n255\n";
        for (int j = 0; j < ny; ++j)
            for (int i = 0; i < nx; ++i)
            {
                double v = f[static_cast<size_t>(g.index(i, j, 0))];
                double t = (v + vmax) / (2.0 * vmax);
                t = std::clamp(t, 0.0, 1.0);
                unsigned char c = static_cast<unsigned char>(t * 255.0);
                os.write(reinterpret_cast<const char *>(&c), 1);
            }
    }
}