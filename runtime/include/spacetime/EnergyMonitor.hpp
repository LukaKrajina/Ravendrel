#pragma once
#include <vector>
#include "SliceTopology.hpp"
#include "Differential.hpp"
#include "TachyonField.hpp"


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