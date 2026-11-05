// ============================================================================
// 快子场 + 切片 数值框架基准测试
//
// 四项可验证基准（对照解析解）：
//   [1] 线性增长阶段：增长率 γ 须收敛到 √(μ²−k²)
//   [2] kink 静解保持：φ = φ_vac·tanh(μx/√2) 形状不变
//   [3] 能量守恒：Si积分下长时间相对漂移不增长
//   [4] 收敛阶：Forest–Ruth 4 阶（dt 减半，误差 16 倍下降）
// ============================================================================

#include <cstdio>
#include <cmath>
#include <vector>
#include <algorithm>

#include "spacetime/Foliation.hpp"
#include "spacetime/InitialConditions.hpp"

using namespace rvdl::spacetime;

namespace
{
    constexpr double PI = 3.14159265358979323846;

    double l2norm(const std::vector<double> &v)
    {
        double s = 0.0;
        for (double x : v)
            s += x * x;
        return std::sqrt(s);
    }

    double maxnorm(const std::vector<double> &a, const std::vector<double> &b)
    {
        double m = 0.0;
        for (size_t i = 0; i < a.size(); ++i)
            m = std::max(m, std::abs(a[i] - b[i]));
        return m;
    }

    // ─── 线性增长──────────────────────────────
    bool test_linear_growth()
    {
        TachyonConfig cfg;
        cfg.mu2 = 1.0;
        cfg.lambda = 1.0;
        cfg.dim = 1;
        cfg.n = {256, 1, 1};
        cfg.length = {20.0, 1.0, 1.0};
        cfg.dt = 0.005;
        cfg.integrator = IntegratorKind::ForestRuth4;
        cfg.diff_order = 4;
        cfg.noise_amplitude = 0.0;

        Foliation fol(cfg);

        const double eps = 1e-7;
        const double L = cfg.length[0];
        const double k = 2.0 * PI / L; // 最低非零模式
        const double k2 = k * k;
        if (k2 >= cfg.mu2)
        {
            printf("  [跳过] k² >= μ²\n");
            return false;
        }
        const double gamma_exact = std::sqrt(cfg.mu2 - k2);

        auto &phi = fol.field().phi;
        auto &pi = fol.field().pi;
        for (int i = 0; i < cfg.n[0]; ++i)
        {
            double x = i * cfg.dx(0);
            phi[i] = eps * std::cos(k * x);
            pi[i] = 0.0;
        }
        const double A0 = l2norm(phi);

        const double T = 8.0;
        const int steps = (int)std::round(T / cfg.dt);
        for (int s = 0; s < steps; ++s)
            fol.step();

        const double At = l2norm(phi);
        const double gamma_num = std::acosh(At / A0) / T;

        double rel = std::abs(gamma_num - gamma_exact) / gamma_exact;
        printf("  γ_exact = %.6f,  γ_num = %.6f,  相对误差 = %.3e\n",
               gamma_exact, gamma_num, rel);
        return rel < 5e-3;
    }

    // ─── kink 静解保持 ───────────────────────────────────────
    bool test_kink_stability()
    {
        TachyonConfig cfg;
        cfg.mu2 = 1.0;
        cfg.lambda = 1.0;
        cfg.dim = 1;
        cfg.n = {512, 1, 1};
        cfg.length = {40.0, 1.0, 1.0};
        cfg.dt = 0.005;
        cfg.integrator = IntegratorKind::ForestRuth4;
        cfg.diff_order = 4;

        Foliation fol(cfg);
        init_kink_antikink_pair(fol.field(), 0);
        const std::vector<double> phi0 = fol.field().phi;

        const double T = 5.0;
        const int steps = (int)std::round(T / cfg.dt);
        for (int s = 0; s < steps; ++s)
            fol.step();

        double err = maxnorm(fol.field().phi, phi0);
        printf("  max|φ(t)-φ(0)| = %.3e\n", err);
        return err < 5e-3;
    }

    // ─── 能量守恒───────────────────────────────────
    bool test_energy_conservation()
    {
        TachyonConfig cfg;
        cfg.mu2 = 1.0;
        cfg.lambda = 1.0;
        cfg.dim = 1;
        cfg.n = {256, 1, 1};
        cfg.length = {20.0, 1.0, 1.0};
        cfg.dt = 0.005;
        cfg.integrator = IntegratorKind::ForestRuth4;
        cfg.diff_order = 4;
        cfg.noise_amplitude = 1e-3;
        cfg.seed = 42u;

        Foliation fol(cfg);
        init_gaussian_noise(fol.field(), cfg.noise_amplitude);
        const double E0 = fol.energy();

        const int steps = 20000; // T = 100
        double max_abs_drift = 0.0;
        double max_rel_drift = 0.0;
        for (int s = 0; s < steps; ++s)
        {
            fol.step();
            if ((s + 1) % 100 == 0)
            {
                double d = std::abs(fol.energy() - E0);
                max_abs_drift = std::max(max_abs_drift, d);
                max_rel_drift = std::max(max_rel_drift, d / (std::abs(E0) + 1e-12));
            }
        }
        printf("  最大绝对漂移 = %.3e,  最大相对漂移 = %.3e  (Forest-Ruth, T=100)\n",
               max_abs_drift, max_rel_drift);
        // 辛积分守恒的是"伪哈密顿量"，监控的离散 H 存在 O(dt⁴) 有界振荡
        // （非单调漂移）。1e-5 的绝对阈值在强非线性（快子滚落+畴壁）下仍严格。
        return max_abs_drift < 1e-5;
    }

    // ─── 收敛阶（Forest-Ruth 4 阶）───────────────────────────
    bool test_convergence_order()
    {
        TachyonConfig base;
        base.mu2 = 1.0;
        base.lambda = 1.0;
        base.dim = 1;
        base.n = {256, 1, 1};
        base.length = {20.0, 1.0, 1.0};
        base.diff_order = 4;
        base.noise_amplitude = 0.0;

        auto run_to_T = [&](double dt)
        {
            TachyonConfig cfg = base;
            cfg.dt = dt;
            Foliation fol(cfg);
            for (int i = 0; i < cfg.n[0]; ++i)
            {
                double x = i * cfg.dx(0);
                double g = std::exp(-0.5 * (x - 10.0) * (x - 10.0));
                fol.field().phi[i] = 0.1 * g;
                fol.field().pi[i] = 0.0;
            }
            int steps = (int)std::round(1.0 / dt); // T = 1
            for (int s = 0; s < steps; ++s)
                fol.step();
            return fol.field().phi;
        };

        const double dt = 0.02;
        auto ref = run_to_T(dt / 64.0);
        auto p1 = run_to_T(dt);
        auto p2 = run_to_T(dt / 2.0);
        auto p3 = run_to_T(dt / 4.0);

        auto l2diff = [](const std::vector<double> &a, const std::vector<double> &b)
        {
            double s = 0.0;
            for (size_t i = 0; i < a.size(); ++i)
            {
                double d = a[i] - b[i];
                s += d * d;
            }
            return std::sqrt(s);
        };

        double e1 = l2diff(p1, ref);
        double e2 = l2diff(p2, ref);
        double e3 = l2diff(p3, ref);
        double order_obs = std::log2(e1 / e2);

        printf("  e(dt)=%.3e, e(dt/2)=%.3e, e(dt/4)=%.3e,  阶数≈%.2f (期望≈4)\n",
               e1, e2, e3, order_obs);
        return order_obs > 3.0;
    }

}

int main()
{
    printf("=== 快子场 + 切片 数值基准 ===\n\n");

    int pass = 0, total = 0;
    bool r;

    printf("[1] 线性增长阶段（验证拉普拉斯+势能+色散）\n");
    r = test_linear_growth();
    printf("    => %s\n\n", r ? "PASS" : "FAIL");
    total++;
    pass += r;

    printf("[2] kink 静解保持（验证非线性项）\n");
    r = test_kink_stability();
    printf("    => %s\n\n", r ? "PASS" : "FAIL");
    total++;
    pass += r;

    printf("[3] 能量守恒\n");
    r = test_energy_conservation();
    printf("    => %s\n\n", r ? "PASS" : "FAIL");
    total++;
    pass += r;

    printf("[4] 收敛阶（Forest-Ruth 4 阶）\n");
    r = test_convergence_order();
    printf("    => %s\n\n", r ? "PASS" : "FAIL");
    total++;
    pass += r;

    printf("=== 结果：%d/%d 通过 ===\n", pass, total);
    return pass == total ? 0 : 1;
}
