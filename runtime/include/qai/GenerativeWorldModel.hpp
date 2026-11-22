#pragma once
#include "qai/QCFField.hpp"
#include <functional>

namespace rvdl
{
    namespace qai
    {
        struct WorldStep
        {
            State state;
            int action = 0;
            size_t observation = 0;
            double fidelity = 1.0;
        };

        struct Trajectory
        {
            std::vector<WorldStep> steps;
            uint64_t seed = 0;
            size_t length() const { return steps.size(); }
            const State &final_state() const { return steps.back().state; }
        };





        class GenerativeWorldModel
        {
        public:
            using Transition = std::function<State(const State &, int)>;

            explicit GenerativeWorldModel(uint64_t seed = 42) : rng_(seed) {}

            Trajectory rollout(const State &initial, const std::vector<int> &actions,
                               const Transition &transition) const
            {
                Trajectory tr;
                tr.seed = 42;
                tr.steps.push_back(WorldStep{normalized(initial), 0, 0, 1.0});
                State cur = normalized(initial);
                for (int a : actions)
                {
                    State next = normalized(transition(cur, a));
                    WorldStep ws;
                    ws.state = next;
                    ws.action = a;
                    ws.observation = born_sample(next, rng_);
                    ws.fidelity = fidelity(cur, next);
                    tr.steps.push_back(std::move(ws));
                    cur = std::move(next);
                }
                return tr;
            }

            double counterfactual(const State &initial, const std::vector<int> &factual,
                                  const std::vector<int> &alternative,
                                  const Transition &transition) const
            {
                const Trajectory a = rollout(initial, factual, transition);
                const Trajectory b = rollout(initial, alternative, transition);
                return fidelity(a.final_state(), b.final_state());
            }

            static double replay_consistency(const Trajectory &tr, const Transition &transition)
            {
                if (tr.steps.size() < 2) return 1.0;
                State cur = tr.steps[0].state;
                double worst = 1.0;
                for (size_t k = 1; k < tr.steps.size(); ++k)
                {
                    cur = normalized(transition(cur, tr.steps[k].action));
                    worst = std::min(worst, fidelity(cur, tr.steps[k].state));
                }
                return worst;
            }

            static double bures_bound(size_t k, double eps_u, double eps_m)
            {
                return double(k) * eps_u + eps_m;
            }

            static double fidelity_bound(size_t k, double eps_u, double eps_m)
            {
                const double d = bures_bound(k, eps_u, eps_m);
                return std::max(0.0, 1.0 - 0.25 * d * d);
            }

            static size_t safe_horizon(double delta, double eps_u, double eps_m)
            {
                if (eps_u <= 0.0) return size_t(1000000);
                return size_t(std::max(0.0, (2.0 * std::sqrt(std::max(0.0, delta)) - eps_m) / eps_u));
            }

            static double quantum_elbo(const std::vector<double> &observation_likelihoods,
                                       const std::vector<double> &model_fidelities)
            {
                const size_t n = std::min(observation_likelihoods.size(), model_fidelities.size());
                double acc = 0.0;
                for (size_t k = 0; k < n; ++k)
                {
                    const double p = std::max(1e-12, observation_likelihoods[k]);
                    const double f = std::min(1.0, std::max(1e-12, model_fidelities[k]));
                    acc += std::log(p) + std::log(f);
                }
                return acc;
            }

        private:
            mutable std::mt19937_64 rng_;
        };
    }
}