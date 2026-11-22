#pragma once
#include <vector>
#include "SliceTopology.hpp"
#include "Differential.hpp"

namespace rvdl::spacetime
{

    inline double tachyon_potential(double phi, double mu2, double lambda)
    {
        return -0.5 * mu2 * phi * phi + 0.25 * lambda * phi * phi * phi * phi;
    }

    inline double tachyon_potential_prime(double phi, double mu2, double lambda)
    {
        return -mu2 * phi + lambda * phi * phi * phi;
    }

    struct TachyonField
    {
        Grid grid;
        TachyonConfig cfg;
        std::vector<double> phi;
        std::vector<double> pi;

        explicit TachyonField(const TachyonConfig &c)
            : grid(c), cfg(c), phi(grid.total(), 0.0), pi(grid.total(), 0.0) {}

        void compute_force(const std::vector<double> &phi_in,
                           std::vector<double> &out) const
        {
            apply_laplacian(grid, phi_in, out, cfg.diff_order);
            const double mu2 = cfg.mu2, lam = cfg.lambda;
            for (size_t p = 0; p < out.size(); ++p)
            {
                const double v = phi_in[p];
                out[p] += mu2 * v - lam * v * v * v;
            }
        }
    };
}