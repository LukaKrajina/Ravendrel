#pragma once
#include <vector>
#include <complex>
#include <cmath>
#include "SliceTopology.hpp"
#include "Differential.hpp"

namespace rvdl::spacetime
{

    namespace spectral
    {

        constexpr double PI = 3.14159265358979323846;

        inline bool is_power_of_two(size_t n) { return n != 0 && (n & (n - 1)) == 0; }

        inline void fft_radix2(std::vector<std::complex<double>> &a,
                               size_t offset, size_t stride, size_t n, bool inverse)
        {
            if (n <= 1)
                return;

            for (size_t i = 1, j = 0; i < n; ++i)
            {
                size_t bit = n >> 1;
                for (; j & bit; bit >>= 1)
                    j ^= bit;
                j ^= bit;
                if (i < j)
                    std::swap(a[offset + i * stride], a[offset + j * stride]);
            }

            for (size_t len = 2; len <= n; len <<= 1)
            {
                double ang = 2.0 * PI / static_cast<double>(len) * (inverse ? 1.0 : -1.0);
                std::complex<double> wlen(std::cos(ang), std::sin(ang));
                for (size_t i = 0; i < n; i += len)
                {
                    std::complex<double> w(1.0, 0.0);
                    for (size_t k = 0; k < len / 2; ++k)
                    {
                        std::complex<double> u = a[offset + (i + k) * stride];
                        std::complex<double> v = a[offset + (i + k + len / 2) * stride] * w;
                        a[offset + (i + k) * stride] = u + v;
                        a[offset + (i + k + len / 2) * stride] = u - v;
                        w *= wlen;
                    }
                }
            }

            if (inverse)
                for (size_t i = 0; i < n; ++i)
                    a[offset + i * stride] /= static_cast<double>(n);
        }

        inline void fft_along_axis(std::vector<std::complex<double>> &a,
                                   const Grid &g, int axis, bool inverse)
        {
            const int nx = g.n[0], ny = g.n[1], nz = g.n[2];
            if (axis == 0)
                for (int k = 0; k < nz; ++k)
                    for (int j = 0; j < ny; ++j)
                        fft_radix2(a, static_cast<size_t>(g.index(0, j, k)), 1, nx, inverse);
            else if (axis == 1)
                for (int k = 0; k < nz; ++k)
                    for (int i = 0; i < nx; ++i)
                        fft_radix2(a, static_cast<size_t>(g.index(i, 0, k)), nx, ny, inverse);
            else
                for (int j = 0; j < ny; ++j)
                    for (int i = 0; i < nx; ++i)
                        fft_radix2(a, static_cast<size_t>(g.index(i, j, 0)), nx * ny, nz, inverse);
        }

        inline void apply_laplacian_spectral(const Grid &g, const std::vector<double> &f,
                                             std::vector<double> &lap)
        {
            const int nx = g.n[0], ny = g.n[1], nz = g.n[2];

            if (!is_power_of_two(nx) || !is_power_of_two(ny) || !is_power_of_two(nz))
            {
                apply_laplacian(g, f, lap, 4);
                return;
            }

            std::vector<std::complex<double>> a(f.size());
            for (size_t p = 0; p < f.size(); ++p)
                a[p] = std::complex<double>(f[p], 0.0);

            if (nx > 1)
                fft_along_axis(a, g, 0, false);
            if (ny > 1)
                fft_along_axis(a, g, 1, false);
            if (nz > 1)
                fft_along_axis(a, g, 2, false);

            auto wave = [](int j, int N, double L)
            {
                int fr = (j < N / 2) ? j : (j - N);
                return 2.0 * PI / L * static_cast<double>(fr);
            };
            for (int k = 0; k < nz; ++k)
                for (int j = 0; j < ny; ++j)
                    for (int i = 0; i < nx; ++i)
                    {
                        double k2 = 0.0;
                        if (nx > 1)
                        {
                            double kk = wave(i, nx, g.length[0]);
                            k2 += kk * kk;
                        }
                        if (ny > 1)
                        {
                            double kk = wave(j, ny, g.length[1]);
                            k2 += kk * kk;
                        }
                        if (nz > 1)
                        {
                            double kk = wave(k, nz, g.length[2]);
                            k2 += kk * kk;
                        }
                        a[static_cast<size_t>(g.index(i, j, k))] *= -k2;
                    }

            if (nz > 1)
                fft_along_axis(a, g, 2, true);
            if (ny > 1)
                fft_along_axis(a, g, 1, true);
            if (nx > 1)
                fft_along_axis(a, g, 0, true);

            lap.resize(f.size());
            for (size_t p = 0; p < f.size(); ++p)
                lap[p] = a[p].real();
        }
    }
}