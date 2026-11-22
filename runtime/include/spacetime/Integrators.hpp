#pragma once
#include <vector>
#include "TachyonField.hpp"

namespace rvdl::spacetime
{

    inline void leapfrog_step(TachyonField &tf, double dt)
    {
        std::vector<double> force(tf.phi.size());

        tf.compute_force(tf.phi, force);
        for (size_t p = 0; p < tf.pi.size(); ++p)
            tf.pi[p] += 0.5 * dt * force[p];

        for (size_t p = 0; p < tf.phi.size(); ++p)
            tf.phi[p] += dt * tf.pi[p];

        tf.compute_force(tf.phi, force);
        for (size_t p = 0; p < tf.pi.size(); ++p)
            tf.pi[p] += 0.5 * dt * force[p];
    }

    inline void forest_ruth_step(TachyonField &tf, double dt)
    {
        constexpr double w1 = 1.0 / (2.0 - 1.2599210498948732);
        constexpr double w0 = 1.0 - 2.0 * w1;

        const double c1 = w1 / 2.0;
        const double c2 = (w0 + w1) / 2.0;
        const double c3 = c2;
        const double c4 = c1;
        const double d1 = w1;
        const double d2 = w0;
        const double d3 = w1;

        std::vector<double> force(tf.phi.size());

        auto drift = [&](double c)
        {
            for (size_t p = 0; p < tf.phi.size(); ++p)
                tf.phi[p] += c * dt * tf.pi[p];
        };
        auto kick = [&](double d)
        {
            tf.compute_force(tf.phi, force);
            for (size_t p = 0; p < tf.pi.size(); ++p)
                tf.pi[p] += d * dt * force[p];
        };

        drift(c1);
        kick(d1);
        drift(c2);
        kick(d2);
        drift(c3);
        kick(d3);
        drift(c4);
    }

    inline void symplectic_step(TachyonField &tf, double dt, IntegratorKind kind)
    {
        if (kind == IntegratorKind::Leapfrog2)
            leapfrog_step(tf, dt);
        else
            forest_ruth_step(tf, dt);
    }
}