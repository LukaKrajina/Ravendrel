#pragma once
#include <vector>
#include <complex>
#include <cmath>


namespace rvdl::spacetime
{

    namespace spherical
    {

    constexpr double PI = 3.14159265358979323846;

    inline void gauss_legendre(int N, std::vector<double> &x, std::vector<double> &w)
    {
        x.resize(N);
        w.resize(N);
        for (int i = 0; i < N; ++i)
        {
            double z = std::cos(PI * (i + 0.75) / (N + 0.5));
            double pp = 0.0;
            for (int iter = 0; iter < 100; ++iter)
            {
                double p0 = 1.0, p1 = 0.0;
                for (int j = 0; j < N; ++j)
                {
                    double p2 = p1;
                    p1 = p0;
                    p0 = ((2.0 * j + 1.0) * z * p1 - j * p2) / (j + 1.0);
                }
                pp = N * (z * p0 - p1) / (z * z - 1.0);
                double z1 = z;
                z = z1 - p0 / pp;
                if (std::abs(z - z1) < 1e-15) break;
            }
            x[i] = z;
            w[i] = 2.0 / ((1.0 - z * z) * pp * pp);
        }
    }

    inline double normalized_ALP(int l, int m, double x)
    {
        double pmm = 1.0;
        if (m > 0)
        {
            double somx2 = std::sqrt((1.0 - x) * (1.0 + x));
            double fact = 1.0;
            for (int i = 1; i <= m; ++i)
            {
                pmm *= fact * somx2;
                fact += 2.0;
            }
        }

        double P = pmm;
        if (l > m)
        {
            double pmmp1 = x * (2.0 * m + 1.0) * pmm;
            if (l == m + 1) P = pmmp1;
            else
            {
                for (int ll = m + 2; ll <= l; ++ll)
                {
                    double pll = ((2.0 * ll - 1.0) * x * pmmp1 - (ll + m - 1.0) * pmm) / (ll - m);
                    pmm = pmmp1;
                    pmmp1 = pll;
                }
                P = pmmp1;
            }
        }

        double norm = std::sqrt((2.0 * l + 1.0) / (4.0 * PI));
        double denom = 1.0;
        for (int k = l - m + 1; k <= l + m; ++k) denom *= k;
        norm *= std::sqrt(1.0 / denom);
        return norm * P;
    }

    inline std::complex<double> spherical_harmonic(int l, int m, double theta, double phi)
    {
        int ma = std::abs(m);
        double P = normalized_ALP(l, ma, std::cos(theta));
        double phase = m * phi;
        std::complex<double> e(std::cos(phase), std::sin(phase));
        std::complex<double> Y = P * e;
        if (m < 0 && (ma % 2 == 1)) Y = -Y;
        return Y;
    }

    }

    struct SphericalSlice
    {
        int    Ntheta = 32;
        int    Nphi   = 64;
        double R      = 1.0;

        std::vector<double> th;
        std::vector<double> gw;

        SphericalSlice() { setup(32, 64, 1.0); }
        SphericalSlice(int nth, int nph, double radius) { setup(nth, nph, radius); }

        void setup(int nth, int nph, double radius)
        {
            Ntheta = nth;
            Nphi = nph;
            R = radius;
            std::vector<double> x;
            spherical::gauss_legendre(Ntheta, x, gw);
            th.resize(Ntheta);
            for (int i = 0; i < Ntheta; ++i)
                th[i] = std::acos(x[i]);
        }

        double theta(int i) const { return th[i]; }
        double phi(int j) const { return 2.0 * spherical::PI * j / Nphi; }
        int    size() const { return Ntheta * Nphi; }
        int    index(int i, int j) const { return i * Nphi + j; }

        void forward(const std::vector<double> &f, int L,
                    std::vector<std::complex<double>> &coeff) const
        {
            int ncoef = (L + 1) * (L + 1);
            coeff.assign(ncoef, {0.0, 0.0});
            const double dph = 2.0 * spherical::PI / Nphi;

            for (int i = 0; i < Ntheta; ++i)
            {
                double th_i = th[i];
                double w = gw[i] * dph;
                double ct = std::cos(th_i);
                for (int j = 0; j < Nphi; ++j)
                {
                    double ph = phi(j);
                    double fv = f[index(i, j)];
                    for (int l = 0; l <= L; ++l)
                    for (int m = -l; m <= l; ++m)
                    {
                        int ma = std::abs(m);
                        double P = spherical::normalized_ALP(l, ma, ct);
                        double sign = (m < 0 && (ma % 2 == 1)) ? -1.0 : 1.0;
                        double pha = -m * ph;
                        std::complex<double> conjY = sign * P *
                            std::complex<double>(std::cos(pha), std::sin(pha));
                        coeff[static_cast<size_t>(l * l + (l + m))] += fv * conjY * w;
                    }
                }
            }
        }

        void backward(const std::vector<std::complex<double>> &coeff, int L,
                    std::vector<double> &f) const
        {
            f.assign(size(), 0.0);
            for (int i = 0; i < Ntheta; ++i)
            for (int j = 0; j < Nphi; ++j)
            {
                double th_i = th[i], ph = phi(j);
                std::complex<double> acc(0.0, 0.0);
                for (int l = 0; l <= L; ++l)
                for (int m = -l; m <= l; ++m)
                    acc += coeff[static_cast<size_t>(l * l + (l + m))] *
                        spherical::spherical_harmonic(l, m, th_i, ph);
                f[index(i, j)] = acc.real();
            }
        }

        void apply_laplacian_spectral(const std::vector<double> &f,
                                    std::vector<double> &lap, int L) const
        {
            std::vector<std::complex<double>> coeff;
            forward(f, L, coeff);
            for (int l = 0; l <= L; ++l)
            for (int m = -l; m <= l; ++m)
                coeff[static_cast<size_t>(l * l + (l + m))] *= -l * (l + 1) / (R * R);
            backward(coeff, L, lap);
        }
    };
}