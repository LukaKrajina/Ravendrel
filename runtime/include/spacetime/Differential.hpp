#pragma once
#include <vector>
#include "SliceTopology.hpp"

namespace rvdl::spacetime
{

    // 一维二阶导数差分模板（周期边界），返回 Σ co[m] * f[i+off[m]]（未除分母）。
    struct Stencil1D
    {
        int off[7];
        double co[7];
        int nterm;
        double denom;
    };

    inline Stencil1D make_stencil(int order)
    {
        Stencil1D s{};
        if (order == 2)
        {
            s.off[0] = 1;
            s.co[0] = 1.0;
            s.off[1] = 0;
            s.co[1] = -2.0;
            s.off[2] = -1;
            s.co[2] = 1.0;
            s.nterm = 3;
            s.denom = 1.0;
        }
        else if (order == 6)
        {
            s.off[0] = 3;
            s.co[0] = 2.0;
            s.off[1] = 2;
            s.co[1] = -27.0;
            s.off[2] = 1;
            s.co[2] = 270.0;
            s.off[3] = 0;
            s.co[3] = -490.0;
            s.off[4] = -1;
            s.co[4] = 270.0;
            s.off[5] = -2;
            s.co[5] = -27.0;
            s.off[6] = -3;
            s.co[6] = 2.0;
            s.nterm = 7;
            s.denom = 180.0;
        }
        else // 默认 4 阶
        {
            s.off[0] = 2;
            s.co[0] = -1.0;
            s.off[1] = 1;
            s.co[1] = 16.0;
            s.off[2] = 0;
            s.co[2] = -30.0;
            s.off[3] = -1;
            s.co[3] = 16.0;
            s.off[4] = -2;
            s.co[4] = -1.0;
            s.nterm = 5;
            s.denom = 12.0;
        }
        return s;
    }

    // 计算拉普拉斯 ∇²f，结果存入 lap（高阶中心差分 + 周期边界）。
    inline void apply_laplacian(const Grid &g, const std::vector<double> &f,
                                std::vector<double> &lap, int order = 4)
    {
        const int nx = g.n[0], ny = g.n[1], nz = g.n[2];
        const double hx2 = g.dx(0) * g.dx(0);
        const double hy2 = g.dx(1) * g.dx(1);
        const double hz2 = g.dx(2) * g.dx(2);
        const Stencil1D st = make_stencil(order);

        lap.assign(f.size(), 0.0);

        for (int k = 0; k < nz; ++k)
            for (int j = 0; j < ny; ++j)
                for (int i = 0; i < nx; ++i)
                {
                    const int idx = g.index(i, j, k);
                    double s = 0.0;

                    if (nx > 1)
                    {
                        double t = 0.0;
                        for (int m = 0; m < st.nterm; ++m)
                            t += st.co[m] * f[g.index(Grid::wrap(i + st.off[m], nx), j, k)];
                        s += t / (st.denom * hx2);
                    }

                    if (ny > 1)
                    {
                        double t = 0.0;
                        for (int m = 0; m < st.nterm; ++m)
                            t += st.co[m] * f[g.index(i, Grid::wrap(j + st.off[m], ny), k)];
                        s += t / (st.denom * hy2);
                    }
                    
                    if (nz > 1)
                    {
                        double t = 0.0;
                        for (int m = 0; m < st.nterm; ++m)
                            t += st.co[m] * f[g.index(i, j, Grid::wrap(k + st.off[m], nz))];
                        s += t / (st.denom * hz2);
                    }

                    lap[idx] = s;
                }
    }

    // 梯度模方 |∇φ|²（2 阶中心差分），供能量监控使用。
    inline void gradient_sq(const Grid &g, const std::vector<double> &f,
                            std::vector<double> &grad2)
    {
        const int nx = g.n[0], ny = g.n[1], nz = g.n[2];
        grad2.assign(f.size(), 0.0);

        for (int k = 0; k < nz; ++k)
            for (int j = 0; j < ny; ++j)
                for (int i = 0; i < nx; ++i)
                {
                    const int idx = g.index(i, j, k);
                    double s = 0.0;

                    if (nx > 1)
                    {
                        double d = (f[g.index(Grid::wrap(i + 1, nx), j, k)] -
                                    f[g.index(Grid::wrap(i - 1, nx), j, k)]) /
                                   (2.0 * g.dx(0));
                        s += d * d;
                    }
                    if (ny > 1)
                    {
                        double d = (f[g.index(i, Grid::wrap(j + 1, ny), k)] -
                                    f[g.index(i, Grid::wrap(j - 1, ny), k)]) /
                                   (2.0 * g.dx(1));
                        s += d * d;
                    }
                    if (nz > 1)
                    {
                        double d = (f[g.index(i, j, Grid::wrap(k + 1, nz))] -
                                    f[g.index(i, j, Grid::wrap(k - 1, nz))]) /
                                   (2.0 * g.dx(2));
                        s += d * d;
                    }

                    grad2[idx] = s;
                }
    }
}