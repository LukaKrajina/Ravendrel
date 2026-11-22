#pragma once
#include <vector>
#include <complex>
#include <cmath>

namespace rvdl::spacetime
{

    constexpr double KK_PI = 3.14159265358979323846;
    struct KaluzaKlein
    {
        double R;
        double theta = 0.0;

        explicit KaluzaKlein(double radius, double wilson_line = 0.0)
            : R(radius), theta(wilson_line) {}

        double mode_mass(int n, double m0 = 0.0) const
        {
            double p = static_cast<double>(n) / R + theta;
            return std::sqrt(m0 * m0 + p * p);
        }

        std::vector<double> mass_spectrum(int nmax, double m0 = 0.0) const
        {
            std::vector<double> spec;
            spec.reserve(2 * nmax + 1);
            for (int n = -nmax; n <= nmax; ++n)
                spec.push_back(mode_mass(n, m0));
            return spec;
        }

        void decompose(const std::vector<double> &field5d, int n4d, int n5,
                       std::vector<std::vector<std::complex<double>>> &modes,
                       int nmax) const
        {
            modes.assign(2 * nmax + 1, std::vector<std::complex<double>>(n4d, {0.0, 0.0}));
            for (int n = -nmax; n <= nmax; ++n)
            {
                auto &out = modes[n + nmax];
                for (int i4 = 0; i4 < n4d; ++i4)
                {
                    std::complex<double> acc(0.0, 0.0);
                    for (int i5 = 0; i5 < n5; ++i5)
                    {
                        double y = 2.0 * KK_PI * static_cast<double>(i5) / n5;
                        double phase = static_cast<double>(n) * y;
                        acc += std::complex<double>(field5d[i4 * n5 + i5], 0.0) *
                               std::complex<double>(std::cos(phase), -std::sin(phase));
                    }
                    out[i4] = acc / static_cast<double>(n5);
                }
            }
        }

        void reconstruct(const std::vector<std::vector<std::complex<double>>> &modes,
                         int n4d, int n5, int nmax, std::vector<double> &field5d) const
        {
            field5d.assign(static_cast<size_t>(n4d) * n5, 0.0);
            for (int i4 = 0; i4 < n4d; ++i4)
                for (int i5 = 0; i5 < n5; ++i5)
                {
                    double y = 2.0 * KK_PI * static_cast<double>(i5) / n5;
                    std::complex<double> acc(0.0, 0.0);
                    for (int n = -nmax; n <= nmax; ++n)
                        acc += modes[n + nmax][i4] *
                               std::complex<double>(std::cos(n * y), std::sin(n * y));
                    field5d[i4 * n5 + i5] = acc.real();
                }
        }
    };
}