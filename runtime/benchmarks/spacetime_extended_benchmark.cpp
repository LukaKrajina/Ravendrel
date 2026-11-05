// ============================================================================
// 扩展模块基准：谱方法 / S² 球面 / 高亏格 / KK / O(D,D)
//
//   [1] 对 cos(kx) 精确到机器精度
//   [2] S² 球面球谐谱拉普拉斯
//   [3] Euler 特征数 + cotan 算子验证
//   [4] Kaluza–Klein 质量谱
//   [5] O(D,D) 广义度规 + section condition
// ============================================================================

#include <cstdio>
#include <cmath>
#include <vector>
#include <algorithm>

#include "spacetime/Spectral.hpp"
#include "spacetime/SphericalSlice.hpp"
#include "spacetime/LaplaceBeltrami.hpp"
#include "spacetime/KaluzaKlein.hpp"
#include "spacetime/GeneralizedMetric.hpp"

using namespace rvdl::spacetime;

namespace
{
    constexpr double PI = 3.14159265358979323846;

    // ─── 谱方法 FFT 拉普拉斯 ─────────────────────────────────
    bool test_spectral()
    {
        Grid g;
        g.dim = 1;
        g.n = {64, 1, 1};
        g.length = {2.0 * PI, 1.0, 1.0};

        const int kk = 3;
        std::vector<double> f(64);
        for (int i = 0; i < 64; ++i)
        {
            double x = i * g.dx(0);
            f[i] = std::cos(kk * x);
        }

        std::vector<double> lap;
        spectral::apply_laplacian_spectral(g, f, lap);

        double maxerr = 0.0;
        for (int i = 0; i < 64; ++i)
            maxerr = std::max(maxerr, std::abs(lap[i] - (-9.0) * f[i]));
        printf("  谱拉普拉斯 max 误差 = %.3e (期望 ~1e-12)\n", maxerr);
        return maxerr < 1e-10;
    }

    // ─── S² 球面球谐谱拉普拉斯 ──────────────────────────────
    bool test_spherical()
    {
        SphericalSlice s(32, 64, 1.0);
        std::vector<double> f(s.size());
        for (int i = 0; i < s.Ntheta; ++i)
            for (int j = 0; j < s.Nphi; ++j)
            {
                double ct = std::cos(s.theta(i));
                f[s.index(i, j)] = std::sqrt(5.0 / (4.0 * PI)) * (3.0 * ct * ct - 1.0) / 2.0;
            }

        std::vector<double> lap;
        const int L = 8;
        s.apply_laplacian_spectral(f, lap, L);

        double maxerr = 0.0, maxf = 0.0;
        for (int p = 0; p < s.size(); ++p)
        {
            maxf = std::max(maxf, std::abs(f[p]));
            maxerr = std::max(maxerr, std::abs(lap[p] - (-6.0) * f[p]));
        }
        printf("  S² 球面谱拉普拉斯 max 误差 = %.3e (相对 %.3e)\n", maxerr, maxerr / maxf);
        return (maxerr / maxf) < 1e-2;
    }

    // ─── 高亏格 Laplace–Beltrami ────────────────────────────
    bool test_laplace_beltrami()
    {
        auto sph = make_sphere_mesh(1.0, 16, 32);
        auto tor = make_torus_mesh(2.0, 0.5, 24, 16);
        auto g2 = make_genus2_mesh(2.0, 0.5, 24, 16);

        int chi_sph = sph.euler_characteristic();
        int chi_tor = tor.euler_characteristic();
        int chi_g2 = g2.euler_characteristic();
        printf("  Euler χ: 球面=%d(期望2) 环面=%d(期望0) 八字环面=%d(期望-2)\n",
               chi_sph, chi_tor, chi_g2);
        std::vector<double> ones(sph.vertex_count(), 1.0);
        std::vector<double> lap;
        apply_laplace_beltrami(sph, ones, lap);
        double maxconst = 0.0;
        for (double v : lap)
            maxconst = std::max(maxconst, std::abs(v));

        // 球面调和
        std::vector<double> fz(sph.vertex_count());
        for (int i = 0; i < sph.vertex_count(); ++i)
            fz[i] = sph.vertices[i][2];
        apply_laplace_beltrami(sph, fz, lap);
        double maxerr = 0.0;
        for (int i = 0; i < sph.vertex_count(); ++i)
            maxerr = std::max(maxerr, std::abs(lap[i] - (-2.0) * fz[i]));

        printf("  常数 |Lf| 最大 = %.3e (期望~0),  z 调和误差 = %.3e (期望<0.2)\n",
               maxconst, maxerr);

        bool ok = (chi_sph == 2) && (chi_tor == 0) && (chi_g2 == -2) &&
                  (maxconst < 1e-10) && (maxerr < 0.2);
        return ok;
    }

    // ─── Kaluza–Klein 质量谱 ─────────────────────────────────
    bool test_kk()
    {
        KaluzaKlein kk(2.0); // R = 2
        bool ok = true;
        for (int n = -5; n <= 5; ++n)
        {
            double m = kk.mode_mass(n, 0.0);
            double expected = std::abs(n) / 2.0;
            if (std::abs(m - expected) > 1e-12)
                ok = false;
        }
        printf("  KK 质量谱 m_n = |n|/R 验证：%s (m_0=%.4f, m_1=%.4f, m_2=%.4f)\n",
               ok ? "PASS" : "FAIL", kk.mode_mass(0), kk.mode_mass(1), kk.mode_mass(2));
        return ok;
    }

    // ─── O(D,D) 广义度规 + section condition ─────────────────
    bool test_generalized_metric()
    {
        const int D = 2;
        GeneralizedMetric gm(D);
        gm.set_diagonal({1.0, 4.0}); // g = diag(1, 4)
        gm.B[0 * D + 1] = 1.0;       // 反对称 B 场
        gm.B[1 * D + 0] = -1.0;
        gm.build();

        double res = gm.o_dd_residual();
        printf("  O(D,D) 残差 ‖HηH-η‖ = %.3e (期望 ~1e-15)\n", res);

        // section condition
        // ∂ 仅沿 x 方向（不依赖对偶坐标 x̃）
        std::vector<double> dM(2 * D, 0.0);
        dM[0] = 1.0;
        dM[1] = 2.0;
        double sc = gm.section_condition_residual(dM, dM);
        printf("  section condition 残差 = %.3e (期望 0)\n", sc);

        // T-对偶
        double Rd = gm.dual_radius(2.0);
        printf("  T-对偶：R=2.0 → R̃=%.4f (期望 0.5)\n", Rd);

        return (res < 1e-10) && (sc < 1e-12) && (std::abs(Rd - 0.5) < 1e-12);
    }

}

int main()
{
    printf("=== 扩展模块基准 ===\n\n");

    int pass = 0, total = 0;
    bool r;

    printf("[1] 谱方法 FFT 拉普拉斯\n");
    r = test_spectral();
    printf("    => %s\n\n", r ? "PASS" : "FAIL");
    total++;
    pass += r;

    printf("[2] S² 球面球谐谱拉普拉斯\n");
    r = test_spherical();
    printf("    => %s\n\n", r ? "PASS" : "FAIL");
    total++;
    pass += r;

    printf("[3] 高亏格 Laplace–Beltrami\n");
    r = test_laplace_beltrami();
    printf("    => %s\n\n", r ? "PASS" : "FAIL");
    total++;
    pass += r;

    printf("[4] Kaluza–Klein 质量谱\n");
    r = test_kk();
    printf("    => %s\n\n", r ? "PASS" : "FAIL");
    total++;
    pass += r;

    printf("[5] O(D,D) 广义度规 + section condition\n");
    r = test_generalized_metric();
    printf("    => %s\n\n", r ? "PASS" : "FAIL");
    total++;
    pass += r;

    printf("=== 结果：%d/%d 通过 ===\n", pass, total);
    return pass == total ? 0 : 1;
}