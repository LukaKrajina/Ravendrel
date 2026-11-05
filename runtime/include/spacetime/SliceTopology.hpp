#pragma once
#include <array>
#include "spacetime_config.h"

namespace rvdl::spacetime
{

    struct Grid
    {
        int dim = 1;
        std::array<int, 3> n = {1, 1, 1};
        std::array<double, 3> length = {1.0, 1.0, 1.0};

        Grid() = default;

        explicit Grid(const TachyonConfig &c)
        {
            dim = c.dim;
            for (int a = 0; a < 3; ++a)
            {
                n[a] = c.n[a];
                length[a] = c.length[a];
            }
        }

        int total() const { return n[0] * n[1] * n[2]; }
        double dx(int a) const { return length[a] / n[a]; }

        // row-major 展平索引
        int index(int i, int j, int k) const
        {
            return i + n[0] * (j + n[1] * k);
        }

        // 周期边界包裹（环面拓扑）
        static int wrap(int i, int N)
        {
            int r = i % N;
            return (r < 0) ? (r + N) : r;
        }
    };
}