#pragma once
#include "qai/QCFField.hpp"

namespace rvdl
{
    namespace qai
    {
        enum class ExtensionKind { None, AppendGenerator, ExtendHilbert };

        struct ExtensionPlan
        {
            ExtensionKind kind = ExtensionKind::None;
            size_t new_qubits = 0;
            size_t generator_qubit = 0;
            size_t generator_control = 0;
            int generator_type = 0;
            double residual_norm = 0.0;
            double expected_gap_reduction = 0.0;
        };

        inline void apply_ry(State &s, size_t q, double angle)
        {
            const size_t mask = size_t(1) << q;
            const double c = std::cos(angle * 0.5), sn = std::sin(angle * 0.5);
            for (size_t i = 0; i < s.size(); ++i)
            {
                if (i & mask) continue;
                const size_t j = i | mask;
                const Complex a = s[i], b = s[j];
                s[i] = c * a - sn * b;
                s[j] = sn * a + c * b;
            }
        }
        inline void apply_rz(State &s, size_t q, double angle)
        {
            const size_t mask = size_t(1) << q;
            const Complex phase = std::polar(1.0, angle);
            for (size_t i = 0; i < s.size(); ++i) if (i & mask) s[i] *= phase;
        }
        inline void apply_cnot(State &s, size_t control, size_t target)
        {
            const size_t cm = size_t(1) << control, tm = size_t(1) << target;
            for (size_t i = 0; i < s.size(); ++i)
            {
                if ((i & cm) && !(i & tm)) std::swap(s[i], s[i | tm]);
            }
        }
        inline void apply_gate(State &s, const ExtensionPlan &p, double angle)
        {
            if (p.generator_type == 0) apply_ry(s, p.generator_qubit, angle);
            else if (p.generator_type == 1) apply_rz(s, p.generator_qubit, angle);
            else apply_cnot(s, p.generator_control, p.generator_qubit);
        }





        class GapFreeGrowth
        {
        public:
            explicit GapFreeGrowth(size_t max_states = 24, double threshold = 0.02,
                                   size_t extra_qubits_per_step = 1)
                : cap_(std::max<size_t>(4, max_states)),
                  tau_(threshold), step_qubits_(extra_qubits_per_step) {}

            void observe(const State &s)
            {
                if (s.empty()) return;
                State n = normalized(s);
                if (reachable_.size() >= cap_) reachable_.erase(reachable_.begin());
                reachable_.push_back(std::move(n));
            }

            void reset() { reachable_.clear(); }
            const StateSet &reachable() const { return reachable_; }
            size_t dimension() const { return reachable_.empty() ? 0 : reachable_.front().size(); }

            double gap(size_t d) const { return frame_gap(reachable_, d); }
            double potential() const { return frame_potential(reachable_); }





            ExtensionPlan propose(size_t d, size_t current_qubits, size_t max_qubits) const
            {
                ExtensionPlan p;
                const double delta = gap(d);
                if (reachable_.size() < 2 || delta <= tau_) return p;

                p.residual_norm = 0.0;
                const size_t nq = std::min<size_t>(current_qubits, 20);
                std::vector<double> z(nq > 0 ? nq : 1, 0.0);
                for (const auto &s : reachable_)
                {
                    const auto probs = born_probabilities(s);
                    for (size_t i = 0; i < probs.size(); ++i)
                        for (size_t q = 0; q < nq; ++q)
                            z[q] += probs[i] * ((i >> q) & 1 ? -1.0 : 1.0);
                }
                for (double &v : z) v /= double(reachable_.size());
                size_t best = 0;
                for (size_t q = 1; q < z.size(); ++q)
                    if (std::fabs(z[q]) > std::fabs(z[best])) best = q;
                p.residual_norm = std::fabs(z[best]);

                if (delta > 4.0 * tau_ && current_qubits + step_qubits_ <= max_qubits)
                {
                    p.kind = ExtensionKind::ExtendHilbert;
                    p.new_qubits = step_qubits_;
                    p.expected_gap_reduction = delta * 0.5;
                }
                else
                {
                    p.kind = ExtensionKind::AppendGenerator;
                    p.generator_qubit = best % std::max<size_t>(1, current_qubits);
                    p.generator_control = (best + 1) % std::max<size_t>(1, current_qubits);
                    p.generator_type = 0;
                    p.expected_gap_reduction = delta * 0.25;
                }
                return p;
            }

            static State extend_hilbert(const State &s, size_t extra_qubits)
            {
                const size_t factor = size_t(1) << std::min<size_t>(extra_qubits, 8);
                State out(s.size() * factor, Complex(0.0, 0.0));
                std::copy(s.begin(), s.end(), out.begin());
                return out;
            }





            StateSet apply_extension(const StateSet &set, const ExtensionPlan &p, size_t samples = 4) const
            {
                StateSet out = set;
                if (p.kind == ExtensionKind::None || set.empty()) return out;
                for (size_t k = 1; k <= samples; ++k)
                {
                    const double angle = 0.37 * double(k);
                    for (const auto &base : set)
                    {
                        State s = (p.kind == ExtensionKind::ExtendHilbert)
                            ? extend_hilbert(base, p.new_qubits) : base;
                        if (p.kind == ExtensionKind::ExtendHilbert)
                            apply_ry(s, std::max<size_t>(0, log2_size(base.size())), angle);
                        else
                            apply_gate(s, p, angle);
                        out.push_back(normalized(std::move(s)));
                    }
                }
                return out;
            }

        private:
            static size_t log2_size(size_t n)
            {
                size_t q = 0;
                while ((size_t(1) << q) < n && q < 24) ++q;
                return q;
            }
            StateSet reachable_;
            size_t cap_;
            double tau_;
            size_t step_qubits_;
        };
    }
}