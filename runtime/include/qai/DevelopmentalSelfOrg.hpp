#pragma once
#include "qai/QCFField.hpp"

namespace rvdl
{
    namespace qai
    {
        struct DevStage
        {
            size_t index = 0;
            size_t qubits = 0;
            size_t generators = 0;
            double T = 1.0;
            double mu = 1.0;
            double growth_threshold = 0.02;
        };

        struct PruneResult
        {
            State state;
            size_t pruned_amplitudes = 0;
            double skipped_mass = 0.0;
            double error_bound = 0.0;
        };




        class DevelopmentalSelfOrg
        {
        public:
            explicit DevelopmentalSelfOrg(const QCFConfig &cfg = QCFConfig{},
                                          size_t initial_qubits = 4,
                                          size_t max_qubits = 20)
                : cfg_(cfg), max_qubits_(max_qubits)
            {
                stage_.qubits = initial_qubits;
                stage_.generators = initial_qubits * 2;
                stage_.T = cfg_.T0;
                stage_.mu = cfg_.mu0;
            }

            const DevStage &current() const { return stage_; }

            static double temperature(double t, double tc, double T0 = 1.0)
            {
                return T0 / (1.0 + t / std::max(1.0, tc));
            }
            static double motivation_weight(double t, double tmu, double mu0 = 1.0)
            {
                return mu0 * std::exp(-t / std::max(1.0, tmu));
            }

            DevStage tick()
            {
                ++stage_.index;
                const double t = double(stage_.index);
                stage_.T = temperature(t, cfg_.critical_time, cfg_.T0);
                stage_.mu = motivation_weight(t, cfg_.motivation_decay, cfg_.mu0);
                stage_.growth_threshold = std::max(1e-4, 0.02 * stage_.T);
                return stage_;
            }

            bool should_grow(double gap) const
            {
                return gap > stage_.growth_threshold && stage_.qubits < max_qubits_;
            }
            void register_growth(size_t extra_qubits, size_t extra_generators)
            {
                stage_.qubits = std::min(max_qubits_, stage_.qubits + extra_qubits);
                stage_.generators += extra_generators;
            }




            static PruneResult prune(const State &s, double eps)
            {
                PruneResult r;
                r.state = s;
                const double cutoff = eps * eps;
                double kept = 0.0;
                for (size_t i = 0; i < r.state.size(); ++i)
                {
                    const double p = std::norm(r.state[i]);
                    if (p < cutoff)
                    {
                        r.skipped_mass += p;
                        r.state[i] = Complex(0.0, 0.0);
                        ++r.pruned_amplitudes;
                    }
                    else kept += p;
                }
                if (kept > 0.0)
                {
                    const double inv = 1.0 / std::sqrt(kept);
                    for (auto &x : r.state) x *= inv;
                }
                const double total = kept + r.skipped_mass;
                r.skipped_mass = total > 0.0 ? r.skipped_mass / total : 0.0;
                r.error_bound = 2.0 * std::sqrt(r.skipped_mass) + r.skipped_mass;
                return r;
            }





            static double hebbian_step(std::vector<double> &theta,
                                       const std::vector<double> &residual,
                                       double eta, double qfi_bound)
            {
                const double safe = qfi_bound > 0.0 ? std::min(eta, 1.8 / qfi_bound) : eta;
                double norm = 0.0;
                for (size_t j = 0; j < theta.size() && j < residual.size(); ++j)
                {
                    theta[j] -= safe * residual[j];
                    norm += residual[j] * residual[j];
                }
                return std::sqrt(norm);
            }

        private:
            QCFConfig cfg_;
            DevStage stage_;
            size_t max_qubits_;
        };
    }
}