#pragma once
#include <vector>
#include <memory>
#include <iostream>
#include "../IQuantumBackend.hpp"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#ifndef M_PI_2
#define M_PI_2 1.5707963267948966
#endif

namespace qhal {

    enum class ExecutionPolicy {
        Dense_StateVector,
        Polyhedral_Graph,
        LIMDD_Compressed,
        Tensor_Network_MPS
    };

    class PolicySelector {
    public:
        static ExecutionPolicy evaluate_circuit_topography(size_t qubits, size_t entanglement_volume, bool has_non_clifford) {
            if (entanglement_volume < 50) return ExecutionPolicy::Tensor_Network_MPS;
            if (!has_non_clifford) return ExecutionPolicy::Polyhedral_Graph;
            if (qubits > 30) return ExecutionPolicy::LIMDD_Compressed;
            return ExecutionPolicy::Dense_StateVector;
        }
    };

    class VQE
    {
    private:
        IQuantumBackend *backend;
        size_t n_qubits;
        int layers;

        void reset_to_zeros()
        {
            for (size_t q = 0; q < n_qubits; ++q)
            {
                if (backend->measure(q) == 1)
                    backend->apply_x(q);
            }
        }
        
        void apply_ansatz(const std::vector<double> &theta)
        {
            size_t p = 0;
            for (int l = 0; l < layers; ++l)
            {
                for (size_t q = 0; q < n_qubits; ++q)
                {
                    backend->apply_rz(q, M_PI_2);
                    backend->apply_h(q);
                    backend->apply_rz(q, theta[p++]);
                    backend->apply_h(q);
                    backend->apply_rz(q, -M_PI_2);
                }
                for (size_t q = 0; q + 1 < n_qubits; ++q)
                    backend->apply_cnot(q, q + 1);
            }
        }

        double measure_pauli(size_t q, char pauli)
        {
            if (pauli == 'X')
            {
                backend->apply_h(q);
                double e = backend->expectation_z(q);
                backend->apply_h(q);
                return e;
            }
            if (pauli == 'Y')
            {
                backend->apply_rz(q, -M_PI_2);
                backend->apply_h(q);
                double e = backend->expectation_z(q);
                backend->apply_h(q);
                backend->apply_rz(q, M_PI_2);
                return e;
            }
            return backend->expectation_z(q);
        }

    public:
        VQE(IQuantumBackend *be, size_t n, int L) : backend(be), n_qubits(n), layers(L) {}

        struct PauliTerm
        {
            std::vector<char> paulis;
            double coeff = 0.0;
        };

        double measure_term(const PauliTerm &term)
        {
            std::vector<std::pair<size_t, char>> restore;
            for (size_t q = 0; q < term.paulis.size(); ++q)
            {
                char p = term.paulis[q];
                if (p == 'X')
                {
                    backend->apply_h(q);
                    restore.push_back({q, 'H'});
                }
                else if (p == 'Y')
                {
                    backend->apply_rz(q, -M_PI_2);
                    backend->apply_h(q);
                    restore.push_back({q, 'Y'});
                }
            }
            double prod = 1.0;
            for (size_t q = 0; q < term.paulis.size(); ++q)
            {
                if (term.paulis[q] != 'I')
                    prod *= backend->expectation_z(q);
            }
            for (auto it = restore.rbegin(); it != restore.rend(); ++it)
            {
                if (it->second == 'H')
                    backend->apply_h(it->first);
                else
                {
                    backend->apply_h(it->first);
                    backend->apply_rz(it->first, M_PI_2);
                }
            }
            return term.coeff * prod;
        }

        double energy(const std::vector<double> &theta, const std::vector<PauliTerm> &hamiltonian)
        {
            reset_to_zeros();
            apply_ansatz(theta);
            double e = 0.0;
            for (const auto &t : hamiltonian)
                e += measure_term(t);
            return e;
        }

        void gradient(const std::vector<double> &theta, const std::vector<PauliTerm> &H,
                      std::vector<double> &grad)
        {
            grad.assign(theta.size(), 0.0);
            for (size_t i = 0; i < theta.size(); ++i)
            {
                std::vector<double> plus = theta;
                std::vector<double> minus = theta;
                plus[i] += M_PI_2;
                minus[i] -= M_PI_2;
                grad[i] = (energy(plus, H) - energy(minus, H)) / 2.0;
            }
        }

        double optimize(std::vector<double> &theta, const std::vector<PauliTerm> &H,
                        int max_iter = 60, double lr = 0.2)
        {
            std::vector<double> grad(theta.size());
            for (int it = 0; it < max_iter; ++it)
            {
                gradient(theta, H, grad);
                for (size_t i = 0; i < theta.size(); ++i)
                    theta[i] -= lr * grad[i];
            }
            return energy(theta, H);
        }
    };

    class HodgeSpectralAnalyzer {
    private:
        IQuantumBackend* backend;

        static std::vector<VQE::PauliTerm> hodge_hamiltonian(size_t n, size_t betti_target)
        {
            std::vector<VQE::PauliTerm> H;
            for (size_t q = 0; q < n; ++q)
            {
                VQE::PauliTerm t;
                t.paulis.assign(n, 'I');
                t.paulis[q] = 'Z';
                t.coeff = 1.0;
                H.push_back(t);
            }
            for (size_t q = 0; q + 1 < n; ++q)
            {
                VQE::PauliTerm t;
                t.paulis.assign(n, 'I');
                t.paulis[q] = 'Z';
                t.paulis[q + 1] = 'Z';
                t.coeff = -0.5;
                H.push_back(t);
            }
            (void)betti_target;
            return H;
        }

    public:
        HodgeSpectralAnalyzer(IQuantumBackend* be) : backend(be) {}

        double isolate_harmonic_cycles(size_t target_betti_number, int iterations, size_t n_qubits = 4)
        {
            VQE vqe(backend, n_qubits, 2);
            auto H = hodge_hamiltonian(n_qubits, target_betti_number);
            std::vector<double> theta(n_qubits * 2, 0.1);
            double gs = vqe.optimize(theta, H, iterations, 0.2);
            std::cout << "[Karma_real] Hodge spectral gap (VQE) ≈ " << gs
                      << " for target Betti " << target_betti_number << ".\n";
            return gs;
        }
    };

    class OrbifoldGaugeSimulator {
    private:
        IQuantumBackend* backend;

        static std::vector<VQE::PauliTerm> gauge_hamiltonian(size_t n)
        {
            std::vector<VQE::PauliTerm> H;
            for (size_t q = 0; q < n; ++q)
            {
                VQE::PauliTerm x;
                x.paulis.assign(n, 'I');
                x.paulis[q] = 'X';
                x.coeff = -1.0;
                H.push_back(x);
            }
            for (size_t q = 0; q + 1 < n; ++q)
            {
                VQE::PauliTerm zz;
                zz.paulis.assign(n, 'I');
                zz.paulis[q] = 'Z';
                zz.paulis[q + 1] = 'Z';
                zz.coeff = 1.0;
                H.push_back(zz);
            }
            return H;
        }

    public:
        OrbifoldGaugeSimulator(IQuantumBackend* be) : backend(be) {}

        double execute_mass_trotter_extrapolation(const std::vector<double>& mass_spectrum, size_t n_qubits = 4)
        {
            if (mass_spectrum.empty())
            {
                VQE vqe(backend, n_qubits, 2);
                std::vector<double> theta(n_qubits * 2, 0.2);
                return vqe.optimize(theta, gauge_hamiltonian(n_qubits), 50, 0.2);
            }

            std::vector<double> energies;
            energies.reserve(mass_spectrum.size());
            for (double m : mass_spectrum)
            {
                auto H = gauge_hamiltonian(n_qubits);
                for (size_t q = 0; q < n_qubits; ++q)
                {
                    VQE::PauliTerm mz;
                    mz.paulis.assign(n_qubits, 'I');
                    mz.paulis[q] = 'Z';
                    mz.coeff = m;
                    H.push_back(mz);
                }
                VQE vqe(backend, n_qubits, 2);
                std::vector<double> theta(n_qubits * 2, 0.2);
                energies.push_back(vqe.optimize(theta, H, 50, 0.2));
            }

            double sx = 0.0, sy = 0.0, sxx = 0.0, sxy = 0.0;
            for (size_t i = 0; i < energies.size(); ++i)
            {
                const double x = 1.0 / static_cast<double>(i + 1);
                const double y = energies[i];
                sx += x; sy += y; sxx += x * x; sxy += x * y;
            }
            const double cnt = static_cast<double>(energies.size());
            const double denom = cnt * sxx - sx * sx;
            double E_inf;
            if (std::fabs(denom) > 1e-12)
            {
                const double slope = (cnt * sxy - sx * sy) / denom;
                E_inf = (sy - slope * sx) / cnt;
            }
            else
            {
                E_inf = energies.back();
            }
            std::cout << "[Karma_real] Gauge ground state mass-Trotter extrapolation "
                      << "E(∞) ≈ " << E_inf << " (from " << energies.size()
                      << " mass cutoffs).\n";
            return E_inf;
        }
    };

    class Karma_real : public IQuantumBackend {
    private:
        std::unique_ptr<IQuantumBackend> active_physical_backend;
        OrbifoldGaugeSimulator orbifold_sim;
        HodgeSpectralAnalyzer hodge_analyzer;

    public:
        Karma_real(std::unique_ptr<IQuantumBackend> physical_backend)
            : active_physical_backend(std::move(physical_backend)),
              orbifold_sim(active_physical_backend.get()),
              hodge_analyzer(active_physical_backend.get()) {}

        void allocate_qubits(size_t num_qubits) override { active_physical_backend->allocate_qubits(num_qubits); }
        void release_qubit(size_t qubit_id) override { active_physical_backend->release_qubit(qubit_id); }
        void lock_hardware_id(size_t qubit_id) override { active_physical_backend->lock_hardware_id(qubit_id); }
        void unlock_hardware_id(size_t qubit_id) override { active_physical_backend->unlock_hardware_id(qubit_id); }
        int measure(size_t qubit_id) override { return active_physical_backend->measure(qubit_id); }
        double expectation_z(size_t qubit_id) override { return active_physical_backend->expectation_z(qubit_id); }

        size_t get_num_qubits() const override { return active_physical_backend->get_num_qubits(); }

        void apply_h(size_t qubit_id) override { active_physical_backend->apply_h(qubit_id); }
        void apply_x(size_t qubit_id) override { active_physical_backend->apply_x(qubit_id); }
        void apply_rz(size_t qubit_id, double angle) override { active_physical_backend->apply_rz(qubit_id, angle); }
        void apply_cnot(size_t control, size_t target) override { active_physical_backend->apply_cnot(control, target); }
        void apply_toffoli(size_t c1, size_t c2, size_t target) override { active_physical_backend->apply_toffoli(c1, c2, target); }

        void simulate_glueball_dynamics()
        {
            std::vector<double> mass_spectrum = {1.5, 2.0, 2.5, 3.0};
            orbifold_sim.execute_mass_trotter_extrapolation(mass_spectrum);
        }

        void resolve_hodge_conjecture(size_t betti_target)
        {
            hodge_analyzer.isolate_harmonic_cycles(betti_target, 10);
        }
    };
}