#pragma once
#include "TachyonField.hpp"
#include "Integrators.hpp"
#include "EnergyMonitor.hpp"

namespace rvdl::spacetime
{
    class Foliation
    {
    public:
        explicit Foliation(const TachyonConfig &cfg) : field_(cfg) {}

        void step()
        {
            symplectic_step(field_, field_.cfg.dt, field_.cfg.integrator);
            ++step_;
        }

        double energy() const
        {
            return energy_snapshot().total;
        }

        EnergySnapshot energy_snapshot() const
        {
            return measure_energy(field_.grid, field_.phi, field_.pi,
                                  field_.cfg.mu2, field_.cfg.lambda,
                                  field_.cfg.diff_order);
        }

        TachyonField &field() { return field_; }
        const TachyonField &field() const { return field_; }
        int step_count() const { return step_; }

    private:
        TachyonField field_;
        int step_ = 0;
    };
}