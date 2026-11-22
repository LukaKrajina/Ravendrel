#pragma once
#include <cmath>
#include <iostream>
#include <cstddef>
#include "../../qhal/IQuantumBackend.hpp"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace vedaros::quantum
{

    class ChimeraWorldModel
    {
    private:
        qhal::IQuantumBackend *backend_;
        size_t num_qubits_;
        double dt_;
        size_t base_;

    public:
        ChimeraWorldModel(qhal::IQuantumBackend *be, size_t num_qubits = 4,
                          double dt = 0.6, size_t base = 0)
            : backend_(be), num_qubits_(num_qubits), dt_(dt), base_(base)
        {
            backend_->allocate_qubits(base_ + num_qubits_);
            std::cout << "[vedaRos.chimera] QCWM online (base=" << base_ << ", "
                      << num_qubits_ << " qubits, dt=" << dt_ << ").\n";
        }

        void world_drift(double dt)
        {
            for (size_t i = 0; i < num_qubits_; ++i)
            {
                backend_->apply_h(base_ + i);
                backend_->apply_rz(base_ + i, dt * (1.61803 + 0.1 * static_cast<double>(i)));
            }
            for (size_t j = 0; j + 1 < num_qubits_; ++j)
            {
                backend_->apply_cnot(base_ + j, base_ + j + 1);
            }
        }

        void world_step(int action, double dt)
        {
            const double theta = static_cast<double>(action) * (M_PI / 8.0);
            for (size_t i = 0; i < num_qubits_; ++i)
            {
                backend_->apply_rz(base_ + i, theta);
            }
            world_drift(dt);
        }

        void imagine(int action, int steps, double dt)
        {
            for (int k = 0; k < steps; ++k)
            {
                world_step(action, dt);
            }
        }

        double world_error(const ChimeraWorldModel &other, size_t ancilla)
        {
            backend_->allocate_qubits(ancilla + 1);
            backend_->apply_h(ancilla);
            for (size_t i = 0; i < num_qubits_; ++i)
            {
                backend_->apply_cswap(ancilla, base_ + i, other.base_ + i);
            }
            backend_->apply_h(ancilla);
            const double overlap = backend_->expectation_z(ancilla);
            backend_->measure(ancilla);
            return 1.0 - overlap;
        }

        double read_sharpness() const
        {
            return backend_->expectation_z(base_);
        }

        size_t qubits() const { return num_qubits_; }
        size_t base() const { return base_; }
        double dt() const { return dt_; }
        void set_dt(double d) { dt_ = d; }
    };

}