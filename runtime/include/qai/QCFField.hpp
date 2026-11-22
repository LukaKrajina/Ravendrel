#pragma once
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <limits>
#include <numeric>
#include <random>
#include <vector>

namespace rvdl
{
    namespace qai
    {
        using Complex = std::complex<double>;
        using State = std::vector<Complex>;
        using StateSet = std::vector<State>;

        inline double norm_sq(const State &s)
        {
            double acc = 0.0;
            for (const auto &x : s) acc += std::norm(x);
            return acc;
        }

        inline void normalize(State &s)
        {
            const double n = std::sqrt(norm_sq(s));
            if (n <= 0.0) return;
            for (auto &x : s) x /= n;
        }

        inline State normalized(State s) { normalize(s); return s; }

        inline Complex inner(const State &a, const State &b)
        {
            const size_t n = std::min(a.size(), b.size());
            Complex acc(0.0, 0.0);
            for (size_t i = 0; i < n; ++i) acc += std::conj(a[i]) * b[i];
            return acc;
        }

        inline double fidelity(const State &a, const State &b)
        {
            const double f = std::norm(inner(a, b)) / (norm_sq(a) * norm_sq(b) + 1e-300);
            return std::min(1.0, std::max(0.0, f));
        }

        inline std::vector<double> born_probabilities(const State &s)
        {
            const double z = norm_sq(s);
            std::vector<double> p(s.size());
            for (size_t i = 0; i < s.size(); ++i) p[i] = std::norm(s[i]) / (z + 1e-300);
            return p;
        }

        inline double shannon(const std::vector<double> &p)
        {
            double h = 0.0;
            for (double x : p) if (x > 0.0) h -= x * std::log(x);
            return h;
        }

        inline double normalized_measurement_entropy(const State &s)
        {
            if (s.size() <= 1) return 0.0;
            return std::min(1.0, shannon(born_probabilities(s)) / std::log(double(s.size())));
        }

        inline double ensemble_purity(const StateSet &ensemble, const std::vector<double> &pi)
        {
            if (ensemble.empty()) return 1.0;
            double acc = 0.0;
            for (size_t i = 0; i < ensemble.size(); ++i)
                for (size_t j = 0; j < ensemble.size(); ++j)
                    acc += pi[i] * pi[j] * fidelity(ensemble[i], ensemble[j]);
            return std::min(1.0, acc);
        }

        inline double holevo_proxy(const StateSet &ensemble, const std::vector<double> &pi)
        {
            return std::max(0.0, 1.0 - ensemble_purity(ensemble, pi));
        }

        inline double frame_potential(const StateSet &set)
        {
            if (set.empty()) return 1.0;
            const double m = double(set.size());
            double acc = 0.0;
            for (size_t i = 0; i < set.size(); ++i)
                for (size_t j = 0; j < set.size(); ++j)
                {
                    const double f = fidelity(set[i], set[j]);
                    acc += f * f;
                }
            return acc / (m * m);
        }

        inline double haar_frame_potential(size_t d)
        {
            const double dd = double(std::max<size_t>(2, d));
            return 2.0 / (dd * (dd + 1.0));
        }

        inline double frame_gap(const StateSet &set, size_t d)
        {
            return std::max(0.0, frame_potential(set) - haar_frame_potential(d));
        }

        struct QCFConfig
        {
            double T0 = 1.0;
            double mu0 = 1.0;
            double critical_time = 20.0;
            double motivation_decay = 30.0;
            double lambda_tau = 0.1;
            double beta = 1.0;
            double gamma = 1.0;
            double zeta = 0.7;
        };

        struct QCFSignals
        {
            double pred_fidelity = 1.0;
            double frame_gap = 0.0;
            double entropy = 0.0;
            double holevo = 0.0;
            double tau_cost = 0.0;
            double T = 1.0;
            double mu = 1.0;
        };

        struct QCFStepLog
        {
            double F_before = 0.0;
            double F_after = 0.0;
            double delta_F = 0.0;
            bool monotone = true;
            size_t steps = 0;
        };

        inline double free_energy(const QCFSignals &s, const QCFConfig &cfg)
        {
            const double f = std::min(1.0, std::max(1e-12, s.pred_fidelity));
            const double world = -std::log(f);
            return world + s.T * (s.frame_gap - s.entropy)
                   - s.mu * s.holevo + cfg.lambda_tau * s.tau_cost;
        }





        inline QCFStepLog descent_step(QCFSignals &s, const QCFConfig &cfg, double eta = 0.1)
        {
            QCFStepLog log;
            log.F_before = free_energy(s, cfg);
            const auto try_update = [&](auto &&apply) -> bool
            {
                QCFSignals cand = s;
                apply(cand);
                const double before = free_energy(s, cfg);
                double after = free_energy(cand, cfg);
                double a = eta;
                for (int k = 0; k < 12 && after > before + 1e-15; ++k)
                {
                    a *= 0.5;
                    const double r = a / eta;
                    QCFSignals scaled = cand;
                    const auto lerp = [&](double from, double to) { return from + (to - from) * r; };
                    scaled.pred_fidelity = lerp(s.pred_fidelity, cand.pred_fidelity);
                    scaled.frame_gap = lerp(s.frame_gap, cand.frame_gap);
                    scaled.entropy = lerp(s.entropy, cand.entropy);
                    scaled.holevo = lerp(s.holevo, cand.holevo);
                    scaled.T = lerp(s.T, cand.T);
                    scaled.mu = lerp(s.mu, cand.mu);
                    after = free_energy(scaled, cfg);
                    if (after <= before + 1e-15) { cand = scaled; break; }
                }
                if (after <= before + 1e-15) { s = cand; log.steps++; return true; }
                return false;
            };

            try_update([&](QCFSignals &x) { x.pred_fidelity = std::min(1.0, x.pred_fidelity + eta * (1.0 - x.pred_fidelity)); });
            try_update([&](QCFSignals &x) { x.frame_gap = std::max(0.0, x.frame_gap - eta * x.frame_gap); });
            try_update([&](QCFSignals &x) { x.holevo = std::min(1.0, x.holevo + eta * (1.0 - x.holevo)); });
            try_update([&](QCFSignals &x)
            {
                x.T = x.T / (1.0 + 1.0 / std::max(1.0, cfg.critical_time));
                x.mu = x.mu * std::exp(-1.0 / std::max(1.0, cfg.motivation_decay));
            });

            log.F_after = free_energy(s, cfg);
            log.delta_F = log.F_after - log.F_before;
            log.monotone = log.delta_F <= 1e-12;
            return log;
        }

        inline size_t born_sample(const State &s, std::mt19937_64 &rng)
        {
            const auto p = born_probabilities(s);
            std::uniform_real_distribution<double> u(0.0, 1.0);
            const double x = u(rng);
            double acc = 0.0;
            for (size_t i = 0; i < p.size(); ++i)
            {
                acc += p[i];
                if (x <= acc) return i;
            }
            return p.empty() ? 0 : p.size() - 1;
        }
    }
}