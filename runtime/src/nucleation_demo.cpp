// ============================================================================
// 快子场畴壁成核演示（2D / 3D 切片以及场快照输出）
//
// 从 φ≈0 的快子不稳定态出发（叠加高斯白噪声），低动量模式指数增长，
// 场滚落到 ±φ_vac 并在空间中成核出畴壁。输出 PGM 灰度图
// 快照（2D 直接输出，3D 输出中间切片），可直接查看畴壁网络演化。
// ============================================================================

#include <cstdio>
#include <cmath>
#include <vector>
#include <string>

#include "spacetime/Foliation.hpp"
#include "spacetime/InitialConditions.hpp"
#include "spacetime/SnapshotIO.hpp"

using namespace rvdl::spacetime;

// 2D 畴壁成核：输出 PGM 快照序列
static void run_2d_nucleation()
{
    TachyonConfig cfg;
    cfg.mu2 = 1.0;
    cfg.lambda = 1.0;
    cfg.dim = 2;
    cfg.n = {128, 128, 1};
    cfg.length = {40.0, 40.0, 1.0};
    cfg.dt = 0.005;
    cfg.integrator = IntegratorKind::ForestRuth4;
    cfg.diff_order = 4;
    cfg.noise_amplitude = 1e-3;
    cfg.seed = 12345u;

    Foliation fol(cfg);
    init_gaussian_noise(fol.field(), cfg.noise_amplitude);
    const double vac = cfg.vacuum();

    printf("=== 2D 畴壁成核：%dx%d, φ_vac=%.4f ===\n", cfg.n[0], cfg.n[1], vac);

    const int total_steps = 2000; // T = 10
    const int snap_every = 200;
    const double E0 = fol.energy();

    int snap = 0;
    for (int s = 0; s < total_steps; ++s)
    {
        fol.step();
        if ((s + 1) % snap_every == 0)
        {
            char name[128];
            std::snprintf(name, sizeof(name), "nucleation_2d_%04d.pgm", ++snap);
            write_field_pgm(name, fol.field().grid, fol.field().phi, vac);
        }
    }
    printf("  输出 %d 张 PGM 快照 (nucleation_2d_*.pgm)\n", snap);
    printf("  能量漂移 = %.3e\n", std::abs(fol.energy() - E0));
}

// 3D 畴壁成核：输出中间 z 切片 PGM 快照
static void run_3d_nucleation()
{
    TachyonConfig cfg;
    cfg.mu2 = 1.0;
    cfg.lambda = 1.0;
    cfg.dim = 3;
    cfg.n = {48, 48, 48};
    cfg.length = {24.0, 24.0, 24.0};
    cfg.dt = 0.01;
    cfg.integrator = IntegratorKind::Leapfrog2; // 3D 用 2 阶，方便
    cfg.diff_order = 2;
    cfg.noise_amplitude = 1e-2;
    cfg.seed = 777u;

    Foliation fol(cfg);
    init_gaussian_noise(fol.field(), cfg.noise_amplitude);
    const double vac = cfg.vacuum();
    const int nz = cfg.n[2];

    printf("=== 3D 畴壁成核：%dx%dx%d, φ_vac=%.4f ===\n",
           cfg.n[0], cfg.n[1], cfg.n[2], vac);

    const int total_steps = 600; // T = 6
    const int snap_every = 120;

    // 提取中间 z 切片为 2D 场
    auto extract_slice = [&](int kz)
    {
        Grid g2;
        g2.dim = 2;
        g2.n = {cfg.n[0], cfg.n[1], 1};
        g2.length = {cfg.length[0], cfg.length[1], 1.0};
        std::vector<double> f(g2.total(), 0.0);
        for (int j = 0; j < cfg.n[1]; ++j)
            for (int i = 0; i < cfg.n[0]; ++i)
                f[g2.index(i, j, 0)] = fol.field().phi[fol.field().grid.index(i, j, kz)];
        return std::make_pair(g2, f);
    };

    int snap = 0;
    for (int s = 0; s < total_steps; ++s)
    {
        fol.step();
        if ((s + 1) % snap_every == 0)
        {
            auto slice = extract_slice(nz / 2);
            char name[128];
            std::snprintf(name, sizeof(name), "nucleation_3d_%04d.pgm", ++snap);
            write_field_pgm(name, slice.first, slice.second, vac);
        }
    }
    printf("  输出 %d 张 PGM 快照 (nucleation_3d_*.pgm)\n", snap);
}

int main()
{
    run_2d_nucleation();
    printf("\n");
    run_3d_nucleation();
    printf("\n=== 完成 ===\n");
    return 0;
}