#pragma once
#include "qai/QCFField.hpp"

namespace rvdl
{
    namespace qai
    {
        struct MotivationSignals
        {
            double delta_entropy = 0.0;
            double delta_holevo = 0.0;
            double tau_cost = 0.0;
            double aleatoric_variance = 0.0;
            double prediction_error = 0.0;
        };

        struct MotivationResult
        {
            double intrinsic = 0.0;
            double extrinsic = 0.0;
            double total = 0.0;
            double expected_free_energy_proxy = 0.0;
        };





        class IntrinsicMotivation
        {
        public:
            explicit IntrinsicMotivation(const QCFConfig &cfg = QCFConfig{}) : cfg_(cfg) {}

            MotivationSignals measure(const State &before, const State &after,
                                      const StateSet &ensemble,
                                      const std::vector<double> &pi,
                                      double tau_cost = 0.0,
                                      double aleatoric_variance = 0.0) const
            {
                MotivationSignals s;
                s.delta_entropy = normalized_measurement_entropy(after)
                                - normalized_measurement_entropy(before);
                s.delta_holevo = ensemble.empty() ? 0.0 : holevo_proxy(ensemble, pi);
                s.tau_cost = tau_cost;
                s.aleatoric_variance = std::max(0.0, aleatoric_variance);
                s.prediction_error = 1.0 - fidelity(before, after);
                return s;
            }

            double reward(const MotivationSignals &s) const
            {
                return cfg_.beta * s.delta_entropy
                     + cfg_.gamma * s.delta_holevo
                     - cfg_.lambda_tau * s.tau_cost
                     - cfg_.zeta * s.aleatoric_variance;
            }





            double expected_free_energy_proxy(const MotivationSignals &s, double pred_fidelity) const
            {
                const double f = std::min(1.0, std::max(1e-12, pred_fidelity));
                return -std::log(f) - cfg_.gamma * s.delta_holevo
                       + cfg_.lambda_tau * s.tau_cost + cfg_.zeta * s.aleatoric_variance;
            }

            MotivationResult write_back(double extrinsic, const MotivationSignals &s,
                                        double pred_fidelity = 1.0) const
            {
                MotivationResult r;
                r.extrinsic = extrinsic;
                r.intrinsic = reward(s);
                r.total = extrinsic + r.intrinsic;
                r.expected_free_energy_proxy = expected_free_energy_proxy(s, pred_fidelity);
                return r;
            }




            static double noisy_tv_guard(double prediction_error, double aleatoric_variance,
                                         double zeta = 0.7)
            {
                return std::max(0.0, prediction_error - zeta * aleatoric_variance);
            }

            static double empowerment_bound(size_t action_space)
            {
                return std::log(double(std::max<size_t>(1, action_space)));
            }

        private:
            QCFConfig cfg_;
        };
    }
}