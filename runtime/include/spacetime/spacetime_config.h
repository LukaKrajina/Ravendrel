#pragma once
#include <cstdint>
#include <cmath>
#include <array>

namespace rvdl::spacetime
{

    // 切片拓扑类型：当前仅平坦周期格点（T^N 环面）。
    // 预留扩展：S² 球面、高亏格曲面等。
    enum class SliceTopologyKind
    {
        FlatPeriodic
    };

    // 辛积分器类型
    enum class IntegratorKind
    {
        Leapfrog2,
        ForestRuth4
    };

    struct TachyonConfig
    {
        // ── 势能参数 ──
        double mu2 = 1.0;    // μ² > 0（等效 m²_eff = -μ² < 0，即快子）
        double lambda = 1.0; // λ > 0

        // ── 空间切片（叶状结构中单张拓扑曲面的离散化）──
        int dim = 1;                                     // 空间维度 1 / 2 / 3
        std::array<int, 3> n = {256, 1, 1};              // 每维格点数（未用维度填 1）
        std::array<double, 3> length = {20.0, 1.0, 1.0}; // 每维物理长度

        // ── 时间推进 ──
        double dt = 0.005;
        IntegratorKind integrator = IntegratorKind::ForestRuth4;
        int diff_order = 4; // 拉普拉斯差分阶数：2 / 4 / 6

        // ── 初始条件（可复现）──
        uint64_t seed = 42u;
        double noise_amplitude = 1e-6; // 高斯白噪声幅值

        // ── 派生量 ──
        double vacuum() const { return std::sqrt(mu2 / lambda); }
        double dx(int a) const { return length[a] / n[a]; }
        int total_points() const { return n[0] * n[1] * n[2]; }
    };
}