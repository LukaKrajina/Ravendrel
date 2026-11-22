#pragma once
#include <algorithm>
#include <cmath>
#include <complex>
#include <complex>
#include <cstdint>
#include <limits>
#include <numeric>
#include <string>
#include <vector>

namespace rvdl
{
    namespace lowrank
    {
        using Complex = std::complex<double>;
        using State = std::vector<Complex>;

        struct RasConfig
        {
            size_t tile_bits = 6;
            size_t max_rank = 16;
            double epsilon = 0.0;
            size_t power_iters = 3;
            uint64_t seed = 42;
        };

        struct RasReport
        {
            size_t rows = 0, cols = 0, rank = 0, full_rank = 0;
            size_t tile_bits = 0;
            double kept_energy = 0.0, discarded_energy = 0.0;
            double delta = 0.0;
            double error_bound = 0.0;
            double compression = 0.0;
            size_t refactor_count = 0;
            bool exact = false;
            std::string reason;
            std::vector<double> sigma;
        };

        class LowRankState
        {
        public:
            LowRankState() = default;
            explicit LowRankState(const State &dense, const RasConfig &cfg,
                                  RasReport *report = nullptr)
            {
                build(dense, cfg, report);
            }

            void build(const State &dense, const RasConfig &cfg,
                       RasReport *report = nullptr)
            {
                N_ = dense.size();
                t_ = std::min<size_t>(cfg.tile_bits, size_t(20));
                while (t_ > 0 && (size_t(1) << t_) > N_) --t_;
                C_ = size_t(1) << t_;
                L_ = C_ ? N_ / C_ : N_;
                eps_ = cfg.epsilon;
                max_rank_ = cfg.max_rank;
                iters_ = std::max<size_t>(1, cfg.power_iters);
                rng_ = splitmix_seed(cfg.seed);
                refactors_ = 0;
                U_.clear(); V_.clear(); sigma_.clear();

                const size_t full = std::min(L_, C_);
                const bool exact = cfg.epsilon <= 0.0 ||
                                   cfg.max_rank == 0 || cfg.max_rank >= full;
                if (report)
                {
                    report->rows = L_; report->cols = C_;
                    report->full_rank = full; report->tile_bits = t_;
                    report->exact = exact;
                }
                if (exact)
                {
                    exact_ = true;
                    dense_ = dense;
                    r_ = 0;
                    if (report)
                    {
                        report->rank = full;
                        report->kept_energy = norm_sq_state(dense);
                        report->discarded_energy = 0.0;
                        report->delta = 0.0;
                        report->error_bound = 0.0;
                        report->compression = 1.0;
                        report->reason = "epsilon=0 or full rank: exact dense path (L4)";
                    }
                    return;
                }

                exact_ = false;
                dense_.clear();
                State residual = dense;
                const double total = norm_sq_state(dense);
                double discarded = 0.0;
                const size_t target = std::min(cfg.max_rank, full);
                const double energy_cut = cfg.epsilon * cfg.epsilon * std::max(total, 1e-300);
                for (size_t j = 0; j < target; ++j)
                {
                    Factor f = dominant_triplet(residual);
                    if (!(f.sigma > 1e-14) || !std::isfinite(f.sigma)) break;
                    U_.push_back(std::move(f.u));
                    V_.push_back(std::move(f.v));
                    sigma_.push_back(f.sigma);
                    for (size_t k = 0; k < L_; ++k)
                    {
                        const Complex uk = U_.back()[k];
                        if (uk == Complex(0.0, 0.0)) continue;
                        for (size_t c = 0; c < C_; ++c)
                            residual[(k << t_) | c] -= f.sigma * uk * std::conj(V_.back()[c]);
                    }
                    if (f.sigma * f.sigma <= energy_cut)
                    {
                        break;
                    }
                }
                r_ = sigma_.size();
                std::vector<size_t> order(r_);
                std::iota(order.begin(), order.end(), size_t(0));
                std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
                    return sigma_[a] > sigma_[b];
                });
                std::vector<std::vector<Complex>> nu(r_), nv(r_);
                std::vector<double> ns(r_);
                for (size_t j = 0; j < r_; ++j)
                {
                    nu[j] = std::move(U_[order[j]]);
                    nv[j] = std::move(V_[order[j]]);
                    ns[j] = sigma_[order[j]];
                }
                U_ = std::move(nu); V_ = std::move(nv); sigma_ = std::move(ns);
                const double kept = std::accumulate(sigma_.begin(), sigma_.end(), 0.0,
                    [](double a, double s) { return a + s * s; });
                discarded = std::max(0.0, total - kept);
                delta_ = total > 0.0 ? discarded / total : 0.0;
                if (report)
                {
                    report->rank = r_;
                    report->sigma = sigma_;
                    report->kept_energy = kept;
                    report->discarded_energy = discarded;
                    report->delta = delta_;
                    report->error_bound = error_bound();
                    const double dense_cx = double(N_);
                    const double factor_cx = double(r_) * double(L_ + C_);
                    report->compression = factor_cx > 0.0 ? dense_cx / factor_cx : 1.0;
                    report->reason = "rank-adaptive power deflation";
                }
            }

            void apply_ry(size_t q, double angle)
            {
                if (exact_) { dense_ry(dense_, q, angle); return; }
                const double c = std::cos(angle * 0.5), s = std::sin(angle * 0.5);
                if (q >= t_)
                {
                    const size_t m = size_t(1) << (q - t_);
                    for (size_t k = 0; k < L_; ++k)
                    {
                        if (k & m) continue;
                        for (size_t j = 0; j < r_; ++j)
                        {
                            const Complex a = U_[j][k], b = U_[j][k | m];
                            U_[j][k] = c * a - s * b;
                            U_[j][k | m] = s * a + c * b;
                        }
                    }
                }
                else
                {
                    const size_t m = size_t(1) << q;
                    for (size_t cc = 0; cc < C_; ++cc)
                    {
                        if (cc & m) continue;
                        for (size_t j = 0; j < r_; ++j)
                        {
                            const Complex a = V_[j][cc], b = V_[j][cc | m];
                            V_[j][cc] = c * a - s * b;
                            V_[j][cc | m] = s * a + c * b;
                        }
                    }
                }
            }

            void apply_rz(size_t q, double angle)
            {
                if (exact_) { dense_rz(dense_, q, angle); return; }
                const Complex phase(std::cos(angle), std::sin(angle));
                if (q >= t_)
                {
                    const size_t m = size_t(1) << (q - t_);
                    for (size_t k = 0; k < L_; ++k)
                        if (k & m)
                            for (size_t j = 0; j < r_; ++j) U_[j][k] *= phase;
                }
                else
                {
                    const size_t m = size_t(1) << q;
                    const Complex vphase = std::conj(phase);
                    for (size_t cc = 0; cc < C_; ++cc)
                        if (cc & m)
                            for (size_t j = 0; j < r_; ++j) V_[j][cc] *= vphase;
                }
            }

            void apply_cnot(size_t control, size_t target)
            {
                if (exact_) { dense_cnot(dense_, control, target); return; }
                const bool ctrl_row = control >= t_, targ_row = target >= t_;
                if (ctrl_row == targ_row)
                {
                    const size_t cm = ctrl_row ? size_t(1) << (control - t_)
                                              : size_t(1) << control;
                    const size_t tm = targ_row ? size_t(1) << (target - t_)
                                              : size_t(1) << target;
                    auto &F = ctrl_row ? U_ : V_;
                    const size_t len = ctrl_row ? L_ : C_;
                    for (size_t x = 0; x < len; ++x)
                    {
                        if (!(x & cm) || (x & tm)) continue;
                        for (size_t j = 0; j < r_; ++j)
                            std::swap(F[j][x], F[j][x | tm]);
                    }
                    return;
                }
                State dense = to_dense();
                dense_cnot(dense, control, target);
                const double prev_delta = delta_;
                const size_t prev_refactors = refactors_;
                RasConfig cfg;
                cfg.tile_bits = t_;
                const size_t full = std::min(L_, C_);
                cfg.max_rank = std::max<size_t>(1, std::min(max_rank_ * 2,
                                                           full > 1 ? full - 1 : 1));
                cfg.epsilon = eps_ > 0.0 ? eps_ : 1e-9;
                cfg.power_iters = iters_;
                cfg.seed = rng_;
                RasReport rep;
                build(dense, cfg, &rep);
                delta_ = std::min(1.0, prev_delta + delta_);
                refactors_ = prev_refactors + 1;
                (void)rep;
            }

            std::vector<double> probability_one(const std::vector<size_t> &bits) const
            {
                if (exact_) return dense_probability_one(dense_, bits);
                std::vector<std::vector<Complex>> G(r_, std::vector<Complex>(r_));
                std::vector<std::vector<Complex>> F(r_, std::vector<Complex>(r_));
                for (size_t j = 0; j < r_; ++j)
                    for (size_t l = 0; l < r_; ++l)
                    {
                        Complex g(0, 0), f(0, 0);
                        for (size_t c = 0; c < C_; ++c) g += std::conj(V_[j][c]) * V_[l][c];
                        for (size_t k = 0; k < L_; ++k) f += std::conj(U_[l][k]) * U_[j][k];
                        G[j][l] = g; F[j][l] = f;
                    }
                std::vector<double> p1(bits.size(), 0.0);
                for (size_t bi = 0; bi < bits.size(); ++bi)
                {
                    const size_t q = bits[bi];
                    if (q >= t_)
                    {
                        const size_t m = size_t(1) << (q - t_);
                        for (size_t k = 0; k < L_; ++k)
                        {
                            if (!(k & m)) continue;
                            for (size_t j = 0; j < r_; ++j)
                                for (size_t l = 0; l < r_; ++l)
                                    p1[bi] += (sigma_[j] * sigma_[l] *
                                               U_[j][k] * std::conj(U_[l][k]) * G[j][l]).real();
                        }
                    }
                    else
                    {
                        const size_t m = size_t(1) << q;
                        for (size_t c = 0; c < C_; ++c)
                        {
                            if (!(c & m)) continue;
                            for (size_t j = 0; j < r_; ++j)
                                for (size_t l = 0; l < r_; ++l)
                                    p1[bi] += (sigma_[j] * sigma_[l] *
                                               std::conj(V_[j][c]) * V_[l][c] * F[j][l]).real();
                        }
                    }
                }
                return p1;
            }

            State to_dense() const
            {
                if (exact_) return dense_;
                State out(N_, Complex(0.0, 0.0));
                for (size_t k = 0; k < L_; ++k)
                    for (size_t c = 0; c < C_; ++c)
                    {
                        Complex z(0.0, 0.0);
                        for (size_t j = 0; j < r_; ++j)
                            z += sigma_[j] * U_[j][k] * std::conj(V_[j][c]);
                        out[(k << t_) | c] = z;
                    }
                return out;
            }

            double error_bound() const { return 2.0 * std::sqrt(delta_) + delta_; }

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
                    n1 += std::norm(dense[i]); n2 += std::norm(s[i]);
                }
                const double f = std::norm(ip) / (n1 * n2 + 1e-300);
                return std::sqrt(std::max(0.0, 1.0 - std::min(1.0, f)));
            }






            void set_factors(std::vector<std::vector<Complex>> U,
                             std::vector<std::vector<Complex>> V,
                             std::vector<double> sigma,
                             size_t tile_bits, double delta,
                             size_t max_rank = 0, double eps = 1e-9,
                             size_t iters = 7, uint64_t seed = 42)
            {
                exact_ = false;
                dense_.clear();
                t_ = std::min<size_t>(tile_bits, size_t(20));
                C_ = size_t(1) << t_;
                L_ = N_ / C_;
                U_ = std::move(U);
                V_ = std::move(V);
                sigma_ = std::move(sigma);
                r_ = sigma_.size();
                delta_ = delta;
                max_rank_ = max_rank ? max_rank : std::max<size_t>(r_, 1);
                eps_ = eps;
                iters_ = std::max<size_t>(1, iters);
                rng_ = seed;
            }

            const std::vector<std::vector<Complex>> &U() const { return U_; }
            const std::vector<std::vector<Complex>> &V() const { return V_; }
            const std::vector<double> &sigma() const { return sigma_; }
            size_t tile_bits() const { return t_; }
            void set_dimension(size_t n) { N_ = n; }

            size_t rank() const { return exact_ ? 0 : r_; }
            size_t dimension() const { return N_; }
            size_t rows() const { return L_; }
            size_t cols() const { return C_; }
            bool exact() const { return exact_; }
            size_t refactor_count() const { return refactors_; }
            double delta() const { return delta_; }

        private:
            struct Factor
            {
                std::vector<Complex> u, v;
                double sigma = 0.0;
            };

            static uint64_t splitmix_seed(uint64_t x)
            {
                x += 0x9e3779b97f4a7c15ULL;
                x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
                x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
                return x ^ (x >> 31);
            }
            double next_unit() const
            {
                rng_ = splitmix_seed(rng_);
                return double(rng_ >> 11) / 9007199254740992.0;
            }
            static double norm_sq_state(const State &s)
            {
                double a = 0.0;
                for (const auto &z : s) a += std::norm(z);
                return a;
            }

            Factor dominant_triplet(const State &A) const
            {
                std::vector<Complex> v(C_);
                for (size_t c = 0; c < C_; ++c)
                    v[c] = Complex(next_unit() * 2.0 - 1.0, next_unit() * 2.0 - 1.0);
                double vn = std::sqrt(norm_sq_state(v));
                if (vn > 0) for (auto &z : v) z /= vn;

                std::vector<Complex> u(L_, Complex(0, 0));
                for (size_t it = 0; it < iters_; ++it)
                {
                    for (size_t k = 0; k < L_; ++k)
                    {
                        Complex acc(0, 0);
                        for (size_t c = 0; c < C_; ++c) acc += A[(k << t_) | c] * v[c];
                        u[k] = acc;
                    }
                    const double un = std::sqrt(norm_sq_state(u));
                    if (un <= 1e-300) break;
                    for (auto &z : u) z /= un;
                    for (size_t c = 0; c < C_; ++c)
                    {
                        Complex acc(0, 0);
                        for (size_t k = 0; k < L_; ++k)
                            acc += std::conj(A[(k << t_) | c]) * u[k];
                        v[c] = acc;
                    }
                    vn = std::sqrt(norm_sq_state(v));
                    if (vn <= 1e-300) break;
                    for (auto &z : v) z /= vn;
                }
                for (size_t k = 0; k < L_; ++k)
                {
                    Complex acc(0, 0);
                    for (size_t c = 0; c < C_; ++c) acc += A[(k << t_) | c] * v[c];
                    u[k] = acc;
                }
                const double sigma = std::sqrt(norm_sq_state(u));
                if (sigma > 1e-300) for (auto &z : u) z /= sigma;
                return Factor{std::move(u), std::move(v), sigma};
            }

            static void dense_ry(State &s, size_t q, double angle)
            {
                const size_t m = size_t(1) << q;
                const double c = std::cos(angle * 0.5), sn = std::sin(angle * 0.5);
                for (size_t i = 0; i < s.size(); ++i)
                {
                    if (i & m) continue;
                    const Complex a = s[i], b = s[i | m];
                    s[i] = c * a - sn * b;
                    s[i | m] = sn * a + c * b;
                }
            }
            static void dense_rz(State &s, size_t q, double angle)
            {
                const size_t m = size_t(1) << q;
                const Complex phase(std::cos(angle), std::sin(angle));
                for (size_t i = 0; i < s.size(); ++i) if (i & m) s[i] *= phase;
            }
            static void dense_cnot(State &s, size_t control, size_t target)
            {
                const size_t cm = size_t(1) << control, tm = size_t(1) << target;
                for (size_t i = 0; i < s.size(); ++i)
                    if ((i & cm) && !(i & tm)) std::swap(s[i], s[i | tm]);
            }
            static std::vector<double> dense_probability_one(const State &s,
                                                             const std::vector<size_t> &bits)
            {
                std::vector<double> out(bits.size(), 0.0);
                for (size_t i = 0; i < s.size(); ++i)
                    for (size_t j = 0; j < bits.size(); ++j)
                        if (i & (size_t(1) << bits[j])) out[j] += std::norm(s[i]);
                return out;
            }

            size_t N_ = 0, L_ = 1, C_ = 1, t_ = 0, r_ = 0;
            size_t max_rank_ = 0, iters_ = 3, refactors_ = 0;
            double eps_ = 0.0, delta_ = 0.0;
            mutable uint64_t rng_ = 42;
            bool exact_ = true;
            State dense_;
            std::vector<std::vector<Complex>> U_;
            std::vector<std::vector<Complex>> V_;
            std::vector<double> sigma_;
        };

    }
}