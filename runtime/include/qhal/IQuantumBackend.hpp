#pragma once
#include <cstddef>
#include <vector>
#include <string>
#include <complex>

#ifndef _USE_MATH_DEFINES
#define _USE_MATH_DEFINES
#endif
#include <cmath>

#include "Export.hpp"

namespace qhal
{

    class RVDL_RT_API IQuantumBackend
    {
    public:
        virtual ~IQuantumBackend() = default;
        virtual void allocate_qubits(size_t num_qubits) = 0;
        virtual void allocate_qubit(size_t qubit_id)
        {
            allocate_qubits(qubit_id + 1);
        }
        virtual void release_qubit(size_t qubit_id) = 0;
        virtual void lock_hardware_id(size_t qubit_id) = 0;
        virtual void unlock_hardware_id(size_t qubit_id) = 0;
        virtual int measure(size_t qubit_id) = 0;

        virtual double expectation_z(size_t qubit_id)
        {
            return 2.0 * measure(qubit_id) - 1.0;
        }

        virtual void apply_h(size_t qubit_id) {}
        virtual void apply_x(size_t qubit_id) = 0;
        virtual void apply_z(size_t qubit_id)
        {
            apply_h(qubit_id);
            apply_x(qubit_id);
            apply_h(qubit_id);
        }
        virtual void apply_y(size_t qubit_id)
        {
            apply_rz(qubit_id, -M_PI / 2.0);
            apply_x(qubit_id);
            apply_rz(qubit_id, M_PI / 2.0);
        }
        virtual void apply_rz(size_t qubit_id, double angle) = 0;
        virtual void apply_rx(size_t qubit_id, double angle)
        {
            apply_h(qubit_id);
            apply_rz(qubit_id, angle);
            apply_h(qubit_id);
        }
        virtual void apply_ry(size_t qubit_id, double angle)
        {
            apply_rz(qubit_id, -M_PI / 2.0);
            apply_rx(qubit_id, angle);
            apply_rz(qubit_id, M_PI / 2.0);
        }
        virtual void apply_s(size_t qubit_id)
        {
            apply_rz(qubit_id, M_PI / 2.0);
        }
        virtual void apply_t(size_t qubit_id)
        {
            apply_rz(qubit_id, M_PI / 4.0);
        }
        virtual void apply_cnot(size_t control, size_t target) = 0;
        virtual void apply_toffoli(size_t control1, size_t control2, size_t target) = 0;

        virtual void prepare_amplitudes(const std::vector<std::complex<double>> &amps,
                                        size_t num_qubits)
        {
            (void)amps;
            (void)num_qubits;
        }

        virtual void apply_swap(size_t a, size_t b)
        {
            if (a == b)
                return;
            apply_cnot(a, b);
            apply_cnot(b, a);
            apply_cnot(a, b);
        }

        virtual void apply_crz(size_t control, size_t target, double angle)
        {
            apply_rz(target, angle / 2.0);
            apply_cnot(control, target);
            apply_rz(target, -angle / 2.0);
            apply_cnot(control, target);
            apply_rz(control, angle / 2.0);
        }
        virtual void apply_ch(size_t control, size_t target)
        {
            apply_crz(control, target, M_PI / 2.0);
            apply_h(target);
            apply_crz(control, target, M_PI / 2.0);
            apply_h(target);
            apply_crz(control, target, M_PI / 2.0);
            apply_rz(control, -M_PI / 4.0);
        }
        virtual void apply_cx(size_t control, size_t target)
        {
            apply_cnot(control, target);
        }
        virtual void apply_cswap(size_t control, size_t a, size_t b)
        {
            if (a == b)
                return;
            apply_cnot(b, a);
            apply_toffoli(control, a, b);
            apply_cnot(b, a);
        }
        virtual void apply_c_toffoli(size_t c, size_t a, size_t b, size_t t)
        {
            apply_h(t);
            apply_rz(c, M_PI / 8.0);
            apply_rz(a, M_PI / 8.0);
            apply_rz(b, M_PI / 8.0);
            apply_rz(t, M_PI / 8.0);
            apply_cnot(c, a);
            apply_rz(a, -M_PI / 8.0);
            apply_cnot(c, a);
            apply_cnot(a, b);
            apply_rz(b, -M_PI / 8.0);
            apply_cnot(c, b);
            apply_rz(b, M_PI / 8.0);
            apply_cnot(a, b);
            apply_rz(b, -M_PI / 8.0);
            apply_cnot(c, b);
            apply_cnot(b, t);
            apply_rz(t, -M_PI / 8.0);
            apply_cnot(a, t);
            apply_rz(t, M_PI / 8.0);
            apply_cnot(b, t);
            apply_rz(t, -M_PI / 8.0);
            apply_cnot(c, t);
            apply_rz(t, M_PI / 8.0);
            apply_cnot(b, t);
            apply_rz(t, -M_PI / 8.0);
            apply_cnot(a, t);
            apply_rz(t, M_PI / 8.0);
            apply_cnot(b, t);
            apply_rz(t, -M_PI / 8.0);
            apply_cnot(c, t);
            apply_h(t);
        }

        virtual void apply_noise(size_t qubit_id, int channel, double param)
        {
            (void)qubit_id;
            (void)channel;
            (void)param;
        }

        virtual void apply_qft(size_t lo, size_t hi)
        {
            for (int i = static_cast<int>(hi); i >= static_cast<int>(lo); --i)
            {
                apply_h(static_cast<size_t>(i));
                for (int j = static_cast<int>(lo); j < i; ++j)
                {
                    double theta = 2.0 * M_PI / std::pow(2.0, i - j + 1);
                    apply_crz(static_cast<size_t>(i), static_cast<size_t>(j), theta);
                }
            }
        }

        virtual void apply_iqft(size_t lo, size_t hi)
        {
            for (int i = static_cast<int>(lo); i <= static_cast<int>(hi); ++i)
            {
                for (int j = i - 1; j >= static_cast<int>(lo); --j)
                {
                    double theta = 2.0 * M_PI / std::pow(2.0, i - j + 1);
                    apply_crz(static_cast<size_t>(i), static_cast<size_t>(j), -theta);
                }
                apply_h(static_cast<size_t>(i));
            }
        }

        virtual void apply_cqft(size_t control, size_t lo, size_t hi)
        {
            for (int i = static_cast<int>(hi); i >= static_cast<int>(lo); --i)
            {
                apply_ch(control, static_cast<size_t>(i));
                for (int j = static_cast<int>(lo); j < i; ++j)
                {
                    double theta = 2.0 * M_PI / std::pow(2.0, i - j + 1);
                    apply_rz(static_cast<size_t>(j), theta / 2.0);
                    apply_toffoli(control, static_cast<size_t>(i), static_cast<size_t>(j));
                    apply_rz(static_cast<size_t>(j), -theta / 2.0);
                    apply_toffoli(control, static_cast<size_t>(i), static_cast<size_t>(j));
                    apply_crz(control, static_cast<size_t>(i), theta / 2.0);
                }
            }
        }

        virtual int measure_basis(size_t qubit_id, char basis)
        {
            if (basis == 'X' || basis == 'x')
                apply_h(qubit_id);
            else if (basis == 'Y' || basis == 'y')
            {
                apply_rz(qubit_id, M_PI / 2.0);
                apply_h(qubit_id);
            }
            return measure(qubit_id);
        }

        virtual void apply_basis(size_t qubit_id, double theta, double phi)
        {
            apply_rz(qubit_id, -M_PI / 2.0);
            apply_h(qubit_id);
            apply_rz(qubit_id, theta);
            apply_h(qubit_id);
            apply_rz(qubit_id, M_PI / 2.0);
            apply_rz(qubit_id, phi);
        }

        virtual void apply_braid(size_t a, size_t b)
        {
            apply_swap(a, b);
        }

        virtual void apply_cbraid(size_t control, size_t a, size_t b)
        {
            apply_cswap(control, a, b);
        }

        virtual std::vector<std::complex<double>> get_state_vector() const { return {}; }
        virtual size_t get_num_qubits() const { return 0; }

        virtual size_t get_max_qubits() const { return 0; }
    };
}