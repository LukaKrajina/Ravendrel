#pragma once
#include <cstdint>
#include <cmath>
#include <array>

namespace rvdl::spacetime
{

    enum class SliceTopologyKind
    {
        FlatPeriodic
    };

    enum class IntegratorKind
    {
        Leapfrog2,
        ForestRuth4
    };

    struct TachyonConfig
    {
        double mu2 = 1.0;
        double lambda = 1.0;

        int dim = 1;
        std::array<int, 3> n = {256, 1, 1};
        std::array<double, 3> length = {20.0, 1.0, 1.0};

        double dt = 0.005;
        IntegratorKind integrator = IntegratorKind::ForestRuth4;
        int diff_order = 4;

        uint64_t seed = 42u;
        double noise_amplitude = 1e-6;

        double vacuum() const { return std::sqrt(mu2 / lambda); }
        double dx(int a) const { return length[a] / n[a]; }
        int total_points() const { return n[0] * n[1] * n[2]; }
    };
}