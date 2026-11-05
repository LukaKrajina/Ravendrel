#pragma once
#include <vector>
#include "SliceTopology.hpp"
#include "Differential.hpp"
#include "TachyonField.hpp"

// ============================================================================
// 离散哈密顿量：H = Σ [ ½π² + ½(∇φ)² + V(φ) ]
// 均匀网格下求和即积分，体积元因子在物理量对比中相互抵消
// ============================================================================

namespace rvdl::spacetime
{

    struct EnergySnapshot
    {
        double kinetic = 0.0;
        double gradient = 0.0;
        double potential = 0.0;
        double total = 0.0;
    };

    inline EnergySnapshot measure_energy(const Grid &g,
                                         const std::vector<double> &phi,
                                         const std::vector<double> &pi,
                                         double mu2, double lambda,
                                         int diff_order = 4)
    {
        // 梯度能 ½(∇φ)² 通过部分积分写成 -½ φ·∇²φ（周期边界无边界项），
        // 且 ∇² 采用与运动方程力项完全一致的离散拉普拉斯（同一 diff_order），
        // 保证所监控的能量与辛积分所守恒的离散哈密顿量一致，避免了伪漂移。
        std::vector<double> lap;
        apply_laplacian(g, phi, lap, diff_order);

        EnergySnapshot e;
        for (size_t p = 0; p < phi.size(); ++p)
        {
            e.kinetic += 0.5 * pi[p] * pi[p];
            e.gradient += -0.5 * phi[p] * lap[p];
            e.potential += tachyon_potential(phi[p], mu2, lambda);
        }
        e.total = e.kinetic + e.gradient + e.potential;
        return e;
    }
}