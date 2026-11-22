#pragma once
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numeric>
#include <string>
#include <vector>

namespace rvdl
{
    namespace sparse
    {
        using Complex = std::complex<double>;
        using State = std::vector<Complex>;

        enum class BackendChoice { Dense, DQSparse };

        struct SparseConfig
        {
            size_t dense_qubit_threshold = 16;
            size_t tile_bits = 6;
            double epsilon = 0.0;
            double activation_ratio = 0.25;
            bool allow_prune = true;
            size_t cpu_threads = 0;
            size_t gpu_streams = 0;
            uint64_t vram_budget_bytes = 0;
        };

        struct SparseReport
        {
            BackendChoice backend = BackendChoice::Dense;
            size_t qubits = 0;
            size_t dimension = 0;
            size_t tiles = 0;
            size_t active_tiles = 0;
            double active_ratio = 1.0;
            double skipped_mass = 0.0;
            double error_bound = 0.0;
            double measured_trace_error = 0.0;
            std::string reason;
        };

        inline double norm_sq(const State &s)
        {
            double a = 0.0;
            for (const auto &x : s) a += std::norm(x);
            return a;
        }
        inline void normalize(State &s)
        {
            const double n = std::sqrt(norm_sq(s));
            if (n > 0.0) for (auto &x : s) x /= n;
        }

        struct Tile
        {
            bool active = true;
            double mass = 0.0;
            double delta = 0.0;
            std::vector<Complex> amps;
        };




        inline double quantise_tile(Tile &t, double epsilon)
        {
            if (!t.active || epsilon <= 0.0 || t.amps.empty()) return 0.0;
            const double L = double(t.amps.size());
            const double step = epsilon * std::sqrt(std::max(0.0, t.mass) / L);
            if (!(step > 0.0)) return 0.0;
            double delta = 0.0;
            for (auto &z : t.amps)
            {
                const double re = z.real(), im = z.imag();
                const double qr = std::round(re / step) * step;
                const double qi = std::round(im / step) * step;
                delta += (re - qr) * (re - qr) + (im - qi) * (im - qi);
                z = Complex(qr, qi);
            }
            return delta;
        }

        struct SparsePolicy
        {
            static BackendChoice select(size_t n, const State &dense,
                                        const SparseConfig &cfg, SparseReport *report = nullptr)
            {
                SparseReport r;
                r.qubits = n;
                r.dimension = dense.size();
                const size_t tb = std::min<size_t>(cfg.tile_bits, 20);
                const size_t C = (dense.size() > 0) ? (size_t(1) << std::min<size_t>(tb, 20)) : 1;
                const size_t L = C > 0 ? dense.size() / C : dense.size();
                r.tiles = C;

                const auto decide = [&](BackendChoice b, const char *why)
                {
                    r.backend = b;
                    r.reason = why;
                    if (report) *report = r;
                    return b;
                };

                if (n <= cfg.dense_qubit_threshold)
                    return decide(BackendChoice::Dense, "n <= n0: dense default (theorem 2)");
                if (cfg.epsilon <= 0.0)
                    return decide(BackendChoice::Dense, "epsilon=0: exact dense path (theorem 2)");
                if (dense.empty() || C == 0 || L == 0)
                    return decide(BackendChoice::Dense, "degenerate geometry");

                std::vector<double> mass(C, 0.0);
                for (size_t i = 0; i < dense.size(); ++i) mass[i & (C - 1)] += std::norm(dense[i]);
                double low = 0.0;
                const double cut = cfg.epsilon * cfg.epsilon;
                for (double m : mass) if (m < cut) low += 1.0;
                r.active_ratio = 1.0 - low / double(C);
                r.active_tiles = size_t(r.active_ratio * double(C));

                if (r.active_ratio > 1.0 - cfg.activation_ratio)
                    return decide(BackendChoice::Dense, "too few prunable tiles");

                if (cfg.vram_budget_bytes > 0)
                {
                    const uint64_t need = uint64_t(double(dense.size()) * r.active_ratio * 16.0)
                                        + uint64_t(C) * 64;
                    if (need > cfg.vram_budget_bytes)
                        return decide(BackendChoice::Dense, "vram budget exceeded -> safe fallback");
                }
                return decide(BackendChoice::DQSparse, "cost model: sparse wins");
            }
        };




        class SparseState
        {
        public:
            SparseState() = default;
            explicit SparseState(const State &dense, size_t tile_bits, double epsilon = 0.0,
                                 bool allow_prune = true)
            {
                build(dense, tile_bits, epsilon, allow_prune);
            }

            void build(const State &dense, size_t tile_bits, double epsilon = 0.0,
                       bool allow_prune = true)
            {
                d_ = dense;
                t_ = std::min<size_t>(tile_bits, 20);
                C_ = d_.empty() ? 1 : (size_t(1) << std::min<size_t>(t_, size_t(20)));
                L_ = C_ ? d_.size() / C_ : d_.size();
                tiles_.assign(C_, Tile{});
                eps_ = epsilon;
                skipped_mass_ = 0.0;

                std::vector<double> mass(C_, 0.0);
                for (size_t i = 0; i < d_.size(); ++i) mass[i & (C_ - 1)] += std::norm(d_[i]);

                const double cut = epsilon * epsilon;
                for (size_t c = 0; c < C_; ++c)
                {
                    Tile &t = tiles_[c];
                    t.mass = mass[c];
                    t.active = true;
                    if (allow_prune && epsilon > 0.0 && mass[c] < cut)
                    {
                        t.active = false;
                        t.delta = mass[c];
                        skipped_mass_ += mass[c];
                        continue;
                    }
                    t.amps.resize(L_);
                    for (size_t k = 0; k < L_; ++k) t.amps[k] = d_[(k << t_) | c];
                    t.delta = quantise_tile(t, epsilon);
                    skipped_mass_ += t.delta;
                }
                const double total = norm_sq(d_);
                skipped_mass_ = total > 0.0 ? skipped_mass_ / total : 0.0;
                for (auto &t : tiles_) t.delta = total > 0.0 ? t.delta / total : 0.0;
                if (epsilon > 0.0) renorm_active();
            }

            size_t dimension() const { return d_.size(); }
            size_t tiles() const { return C_; }
            size_t active_tiles() const
            {
                size_t n = 0;
                for (const auto &t : tiles_) if (t.active) ++n;
                return n;
            }
            double active_ratio() const { return C_ ? double(active_tiles()) / double(C_) : 0.0; }
            double skipped_mass() const { return skipped_mass_; }

            size_t tile_bits_raw() const { return t_; }
            size_t tile_len() const { return L_; }
            size_t tile_count() const { return C_; }
            bool tile_active(size_t c) const { return c < C_ && tiles_[c].active; }
            double tile_delta(size_t c) const { return c < C_ ? tiles_[c].delta : 0.0; }

            std::vector<size_t> active_tile_ids() const
            {
                std::vector<size_t> ids;
                for (size_t c = 0; c < C_; ++c) if (tiles_[c].active) ids.push_back(c);
                return ids;
            }

            std::vector<Complex> *tile_amps(size_t c)
            {
                return (c < C_ && tiles_[c].active) ? &tiles_[c].amps : nullptr;
            }
            const std::vector<Complex> *tile_amps(size_t c) const
            {
                return (c < C_ && tiles_[c].active) ? &tiles_[c].amps : nullptr;
            }

            Complex *at(size_t i)
            {
                const size_t c = i & (C_ - 1);
                const size_t k = i >> t_;
                if (c >= C_ || !tiles_[c].active) return nullptr;
                if (k >= tiles_[c].amps.size()) return nullptr;
                return &tiles_[c].amps[k];
            }
            const Complex *at(size_t i) const
            {
                const size_t c = i & (C_ - 1);
                const size_t k = i >> t_;
                if (c >= C_ || !tiles_[c].active) return nullptr;
                if (k >= tiles_[c].amps.size()) return nullptr;
                return &tiles_[c].amps[k];
            }

            SparseState zero_like() const
            {
                SparseState s;
                s.d_ = d_;
                s.t_ = t_; s.C_ = C_; s.L_ = L_;
                s.eps_ = eps_;
                s.skipped_mass_ = skipped_mass_;
                s.tiles_.resize(C_);
                for (size_t c = 0; c < C_; ++c)
                {
                    s.tiles_[c].active = tiles_[c].active;
                    s.tiles_[c].mass = tiles_[c].mass;
                    s.tiles_[c].delta = tiles_[c].delta;
                    if (tiles_[c].active) s.tiles_[c].amps.assign(L_, Complex(0.0, 0.0));
                }
                return s;
            }

            double error_bound() const
            {
                double b = 0.0;
                for (const auto &t : tiles_) b += 2.0 * std::sqrt(std::max(0.0, t.delta)) + t.delta;
                return b;
            }

            State to_dense() const
            {
                State out(d_.size(), Complex(0.0, 0.0));
                for (size_t c = 0; c < C_; ++c)
                {
                    const Tile &t = tiles_[c];
                    if (!t.active) continue;
                    for (size_t k = 0; k < L_ && k < t.amps.size(); ++k)
                        out[(k << t_) | c] = t.amps[k];
                }
                return out;
            }





            double measured_error(const State &dense) const
            {
                const State s = to_dense();
                if (dense.empty() || s.empty()) return 1.0;
                const size_t n = std::min(dense.size(), s.size());
                Complex ip(0.0, 0.0);
                double n1 = 0.0, n2 = 0.0;
                for (size_t i = 0; i < n; ++i)
                {
                    ip += std::conj(dense[i]) * s[i];
                    n1 += std::norm(dense[i]);
                    n2 += std::norm(s[i]);
                }
                const double f = std::norm(ip) / (n1 * n2 + 1e-300);
                return std::sqrt(std::max(0.0, 1.0 - std::min(1.0, f)));
            }

            void apply_ry(size_t q, double angle)
            {
                const double c = std::cos(angle * 0.5), s = std::sin(angle * 0.5);
                each_pair(q, [&](Complex &a, Complex &b)
                {
                    const Complex x = a, y = b;
                    a = c * x - s * y;
                    b = s * x + c * y;
                });
            }
            void apply_rz(size_t q, double angle)
            {
                const Complex phase = std::polar(1.0, angle);
                for_each(q, true, [&](Complex &z) { z *= phase; });
            }
            void apply_cnot(size_t control, size_t target)
            {
                for_each_masked(control, target, [](Complex &a, Complex &b)
                {
                    std::swap(a, b);
                });
            }

            std::vector<double> probability_one(const std::vector<size_t> &bits) const
            {
                std::vector<double> p1(bits.size(), 0.0);
                for (size_t c = 0; c < C_; ++c)
                {
                    const Tile &t = tiles_[c];
                    if (!t.active) continue;
                    for (size_t k = 0; k < L_ && k < t.amps.size(); ++k)
                    {
                        const size_t i = (k << t_) | c;
                        const double p = std::norm(t.amps[k]);
                        for (size_t j = 0; j < bits.size(); ++j)
                            if (i & (size_t(1) << bits[j])) p1[j] += p;
                    }
                }
                return p1;
            }

        private:
            void renorm_active()
            {
                double kept = 0.0;
                for (const auto &t : tiles_)
                    if (t.active) for (const auto &z : t.amps) kept += std::norm(z);
                if (kept <= 0.0) return;
                const double inv = 1.0 / std::sqrt(kept);
                for (auto &t : tiles_) if (t.active) for (auto &z : t.amps) z *= inv;
            }

            template <class F>
            void for_each(size_t q, bool want_one, F &&fn)
            {
                const size_t bit = size_t(1) << q;
                if (q >= t_)
                {
                    const size_t inner = size_t(1) << (q - t_);
                    for (size_t c = 0; c < C_; ++c)
                    {
                        Tile &t = tiles_[c];
                        if (!t.active) continue;
                        for (size_t k = 0; k < L_ && k < t.amps.size(); ++k)
                            if (((k & inner) != 0) == want_one) fn(t.amps[k]);
                    }
                }
                else
                {
                    for (size_t c = 0; c < C_; ++c)
                    {
                        if (((c & bit) != 0) != want_one) continue;
                        Tile &t = tiles_[c];
                        if (!t.active) continue;
                        for (auto &z : t.amps) fn(z);
                    }
                }
            }

            template <class F>
            void each_pair(size_t q, F &&fn)
            {
                if (q >= t_)
                {
                    const size_t inner = size_t(1) << (q - t_);
                    for (size_t c = 0; c < C_; ++c)
                    {
                        Tile &t = tiles_[c];
                        if (!t.active) continue;
                        for (size_t k = 0; k < L_ && k < t.amps.size(); ++k)
                        {
                            if (k & inner) continue;
                            const size_t k2 = k | inner;
                            if (k2 >= t.amps.size()) continue;
                            fn(t.amps[k], t.amps[k2]);
                        }
                    }
                }
                else
                {
                    const size_t bit = size_t(1) << q;
                    for (size_t c = 0; c < C_; ++c)
                    {
                        if (c & bit) continue;
                        Tile &a = tiles_[c], &b = tiles_[c | bit];
                        if (!a.active || !b.active) continue;
                        const size_t n = std::min(a.amps.size(), b.amps.size());
                        for (size_t k = 0; k < n; ++k) fn(a.amps[k], b.amps[k]);
                    }
                }
            }

            template <class F>
            void for_each_masked(size_t control, size_t target, F &&fn)
            {
                const bool ctrl_tile = control < t_;
                const bool targ_tile = target < t_;

                for (size_t c = 0; c < C_; ++c)
                {
                    Tile &tc = tiles_[c];
                    if (!tc.active) continue;
                    const bool ctrl_in_c = ctrl_tile ? ((c & (size_t(1) << control)) != 0) : false;
                    for (size_t k = 0; k < L_ && k < tc.amps.size(); ++k)
                    {
                        const bool ctrl = ctrl_tile ? ctrl_in_c : ((k & (size_t(1) << (control - t_))) != 0);
                        if (!ctrl) continue;
                        const bool targ_zero = targ_tile
                            ? ((c & (size_t(1) << target)) == 0)
                            : ((k & (size_t(1) << (target - t_))) == 0);
                        if (!targ_zero) continue;

                        size_t c2 = c, k2 = k;
                        if (targ_tile) c2 = c | (size_t(1) << target);
                        else k2 = k | (size_t(1) << (target - t_));
                        if (c2 >= C_ || !tiles_[c2].active) continue;
                        Tile &tb = tiles_[c2];
                        if (k2 >= tb.amps.size()) continue;
                        fn(tc.amps[k], tb.amps[k2]);
                    }
                }
            }

            State d_;
            size_t t_ = 0, C_ = 1, L_ = 0;
            double eps_ = 0.0, skipped_mass_ = 0.0;
            std::vector<Tile> tiles_;
        };

        inline SparseReport evaluate(const State &dense, size_t n, const SparseConfig &cfg)
        {
            SparseReport report;
            const BackendChoice choice = SparsePolicy::select(n, dense, cfg, &report);
            if (choice == BackendChoice::Dense) return report;
            SparseState s(dense, cfg.tile_bits, cfg.epsilon, cfg.allow_prune);
            report.active_tiles = s.active_tiles();
            report.active_ratio = s.active_ratio();
            report.skipped_mass = s.skipped_mass();
            report.error_bound = s.error_bound();
            report.measured_trace_error = s.measured_error(dense);
            return report;
        }
    }
}