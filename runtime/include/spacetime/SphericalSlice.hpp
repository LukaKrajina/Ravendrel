#pragma once
#include <vector>
#include <complex>
#include <cmath>

// ============================================================================
// 非平坦切片：S² 球面（球谐谱方法）
//
// 切片不必是平坦的——这里实现二维球面 S² 作为一张"拓扑曲面"切片。
// 在球谐基 Y_l^m 下，Laplace–Beltrami 算子是精确对角化的：
//     Δ Y_l^m = -l(l+1)/R² · Y_l^m
// 因此谱拉普拉斯 = 球谐变换(SHT) → 乘 -l(l+1)/R² → 反变换，谱精度。
//
// θ 方向采用高斯–勒让德（Gauss–Legendre）积分节点，它对勒让德多项式
// 精确正交，避免等距中点积分导致的基函数正交性泄漏。
//
// 验证基准：构造 f = Y_{2,0}（实球谐，正比 3cos²θ-1），
//           谱拉普拉斯应给出 Δf = -6/R² · f。
// ============================================================================

namespace rvdl::spacetime
{

    namespace spherical
    {

    constexpr double PI = 3.14159265358979323846;

    // N 点高斯–勒让德积分节点与权重（Newton 迭代求 P_N 零点）
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
                // 递推计算 P_N(z) 与 P_{N-1}(z)
                double p0 = 1.0, p1 = 0.0;
                for (int j = 0; j < N; ++j)
                {
                    double p2 = p1;
                    p1 = p0;
                    p0 = ((2.0 * j + 1.0) * z * p1 - j * p2) / (j + 1.0);
                }
                pp = N * (z * p0 - p1) / (z * z - 1.0);   // P_N'(z)
                double z1 = z;
                z = z1 - p0 / pp;
                if (std::abs(z - z1) < 1e-15) break;
            }
            x[i] = z;
            w[i] = 2.0 / ((1.0 - z * z) * pp * pp);
        }
    }

    // 归一化关联勒让德函数（无 Condon-Shortley 相位），
    // 返回 sqrt((2l+1)/(4π) · (l-m)!/(l+m)!) · P_l^m(x)，m ≥ 0。
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

    // 复球谐 Y_l^m(θ, φ)（含 (-1)^m 相位用于 m<0）
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

    // S² 球面切片：经纬网格（θ 用高斯–勒让德节点）+ 球谐谱拉普拉斯
    struct SphericalSlice
    {
        int    Ntheta = 32;
        int    Nphi   = 64;
        double R      = 1.0;

        std::vector<double> th;   // θ 节点（GL 节点经 arccos 映射，避开极点）
        std::vector<double> gw;   // 高斯–勒让德积分权重

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
                th[i] = std::acos(x[i]);   // θ ∈ (0,π)，避开极点
        }

        double theta(int i) const { return th[i]; }
        double phi(int j) const { return 2.0 * spherical::PI * j / Nphi; }
        int    size() const { return Ntheta * Nphi; }
        int    index(int i, int j) const { return i * Nphi + j; }

        // 前向 SHT：场 → 球谐系数 a_{lm}（高斯–勒让德积分，索引 l² + (l+m)）
        void forward(const std::vector<double> &f, int L,
                    std::vector<std::complex<double>> &coeff) const
        {
            int ncoef = (L + 1) * (L + 1);
            coeff.assign(ncoef, {0.0, 0.0});
            const double dph = 2.0 * spherical::PI / Nphi;

            for (int i = 0; i < Ntheta; ++i)
            {
                double th_i = th[i];
                double w = gw[i] * dph;    // GL 权重 × φ 步长
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

        // 反向 SHT：球谐系数 → 场
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

        // 谱拉普拉斯（Laplace–Beltrami）：Δf = Σ -l(l+1)/R² a_lm Y_lm
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
