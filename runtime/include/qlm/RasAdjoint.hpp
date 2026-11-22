#pragma once
#include "qhal/LowRankState.hpp"
#include "qlm/StatePolicy.hpp"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <numeric>
#include <vector>

namespace rvdl
{
    namespace ras_adjoint
    {
        using Complex = rvdl::lowrank::Complex;
        using State = rvdl::lowrank::State;
        using LowRankState = rvdl::lowrank::LowRankState;

        static constexpr size_t SHARED_GATE = static_cast<size_t>(-1);

        inline bool gate_active(size_t expert, size_t experts, size_t active)
        {
            return experts <= 1 || expert == SHARED_GATE || expert == active;
        }

        struct Result
        {
            double loss = 0.0, accuracy = 0.0;
            double skipped_mass = 0.0, error_bound = 0.0;
            size_t rank = 0, lambda_rank = 0, refactors = 0;
            std::vector<double> grad, pred, sample_loss, sample_acc;
        };

        struct FactorSet
        {
            std::vector<std::vector<Complex>> U, V;
            std::vector<double> sigma;
            size_t r() const { return sigma.size(); }
            size_t rows() const { return U.empty() ? 0 : U[0].size(); }
            size_t cols() const { return V.empty() ? 0 : V[0].size(); }
            void clear() { U.clear(); V.clear(); sigma.clear(); }
            void push(std::vector<Complex> u, std::vector<Complex> v, double s)
            {
                U.push_back(std::move(u)); V.push_back(std::move(v)); sigma.push_back(s);
            }
        };

        inline FactorSet extract(const LowRankState &st)
        {
            FactorSet f;
            f.U = st.U(); f.V = st.V(); f.sigma = st.sigma();
            return f;
        }

        inline State materialize(const FactorSet &f, size_t L, size_t C, size_t t)
        {
            State out(L * C, Complex(0.0, 0.0));
            for (size_t i = 0; i < f.r(); ++i)
                for (size_t k = 0; k < L; ++k)
                {
                    const Complex uk = f.sigma[i] * f.U[i][k];
                    if (uk == Complex(0.0, 0.0)) continue;
                    for (size_t c = 0; c < C; ++c)
                        out[(k << t) | c] += uk * std::conj(f.V[i][c]);
                }
            return out;
        }

        inline void dense_cnot(State &s, size_t ctrl, size_t targ)
        {
            const size_t cm = size_t(1) << ctrl, tm = size_t(1) << targ;
            for (size_t i = 0; i < s.size(); ++i)
                if ((i & cm) && !(i & tm)) std::swap(s[i], s[i ^ tm]);
        }

        inline void apply_row(FactorSet &f, int type, size_t idx, double angle,
                              size_t q1_idx = 0)
        {
            if (type != 2)
            {
                const size_t m = size_t(1) << idx;
                const double c = std::cos(angle * 0.5), s = std::sin(angle * 0.5);
                const Complex ph(std::cos(angle), std::sin(angle));
                for (size_t i = 0; i < f.r(); ++i)
                {
                    auto &d = f.U[i];
                    if (type == 0)
                        for (size_t x = 0; x < d.size(); ++x)
                        {
                            if (x & m) continue;
                            const Complex a0 = d[x], a1 = d[x | m];
                            d[x] = c * a0 - s * a1;
                            d[x | m] = s * a0 + c * a1;
                        }
                    else
                        for (size_t x = 0; x < d.size(); ++x) if (x & m) d[x] *= ph;
                }
            }
            else
            {
                const size_t cm = size_t(1) << idx, tm = size_t(1) << q1_idx;
                for (size_t i = 0; i < f.r(); ++i)
                {
                    auto &d = f.U[i];
                    for (size_t x = 0; x < d.size(); ++x)
                        if ((x & cm) && !(x & tm)) std::swap(d[x], d[x | tm]);
                }
            }
        }

        inline void apply_col(FactorSet &f, int type, size_t idx, double angle,
                              size_t q1_idx = 0)
        {
            if (type != 2)
            {
                const size_t m = size_t(1) << idx;
                const double c = std::cos(angle * 0.5), s = std::sin(angle * 0.5);
                const Complex vph = std::conj(Complex(std::cos(angle), std::sin(angle)));
                for (size_t i = 0; i < f.r(); ++i)
                {
                    auto &d = f.V[i];
                    if (type == 0)
                        for (size_t x = 0; x < d.size(); ++x)
                        {
                            if (x & m) continue;
                            const Complex a0 = d[x], a1 = d[x | m];
                            d[x] = c * a0 - s * a1;
                            d[x | m] = s * a0 + c * a1;
                        }
                    else
                        for (size_t x = 0; x < d.size(); ++x) if (x & m) d[x] *= vph;
                }
            }
            else
            {
                const size_t cm = size_t(1) << idx, tm = size_t(1) << q1_idx;
                for (size_t i = 0; i < f.r(); ++i)
                {
                    auto &d = f.V[i];
                    for (size_t x = 0; x < d.size(); ++x)
                        if ((x & cm) && !(x & tm)) std::swap(d[x], d[x | tm]);
                }
            }
        }






        template <bool RowDomain>
        inline double dot_side(const FactorSet &phi, const FactorSet &lam,
                               int type, size_t idx)
        {
            const size_t m = size_t(1) << idx;
            const size_t rp = phi.r(), rl = lam.r();
            const size_t axis = RowDomain ? phi.rows() : phi.cols();
            const size_t other = RowDomain ? phi.cols() : phi.rows();
            auto &pA = RowDomain ? phi.U : phi.V;
            auto &lA = RowDomain ? lam.U : lam.V;
            auto &pB = RowDomain ? phi.V : phi.U;
            auto &lB = RowDomain ? lam.V : lam.U;

            std::vector<std::vector<Complex>> G1(rp, std::vector<Complex>(rl, Complex(0, 0)));
            std::vector<std::vector<Complex>> G2, G3;
            if (type == 0)
            {
                G2.assign(rp, std::vector<Complex>(rl, Complex(0, 0)));
                G3.assign(rp, std::vector<Complex>(rl, Complex(0, 0)));
            }
            for (size_t a = 0; a < rp; ++a)
            {
                const auto &pa = pA[a];
                for (size_t b = 0; b < rl; ++b)
                {
                    const auto &lb = lA[b];
                    Complex s1(0, 0), s2(0, 0), s3(0, 0);
                    for (size_t x = 0; x < axis; ++x)
                    {
                        if (type == 1)
                        {
                            if (!(x & m)) continue;
                            s1 += std::conj(lb[x]) * pa[x];
                        }
                        else
                        {
                            if (x & m) continue;
                            s2 += std::conj(lb[x | m]) * pa[x];
                            s3 += std::conj(lb[x]) * pa[x | m];
                        }
                    }
                    if (type == 1) G1[a][b] = s1;
                    else { G2[a][b] = s2; G3[a][b] = s3; }
                }
            }
            std::vector<std::vector<Complex>> H(rp, std::vector<Complex>(rl, Complex(0, 0)));
            for (size_t a = 0; a < rp; ++a)
                for (size_t b = 0; b < rl; ++b)
                {
                    Complex h(0, 0);
                    for (size_t y = 0; y < other; ++y)
                        h += std::conj(pB[a][y]) * lB[b][y];
                    H[a][b] = h;
                }
            Complex total(0, 0);
            for (size_t a = 0; a < rp; ++a)
                for (size_t b = 0; b < rl; ++b)
                {
                    const double w = phi.sigma[a] * lam.sigma[b];
                    if (type == 1) total += w * G1[a][b] * H[a][b];
                    else total += w * (G2[a][b] - G3[a][b]) * H[a][b];
                }
            return type == 1 ? total.imag() : total.real();
        }

        inline double sample_adjoint_ras(
            const unsigned char *gate_type, const size_t *q0, const size_t *q1,
            const size_t *param, const double *scale, size_t G,
            const Complex *ket, size_t N,
            const double *theta, const size_t *readout_bits, size_t D,
            uint8_t target_token, size_t P,
            const qlm::AdjointRuntimeConfig &cfg,
            std::vector<double> &grad, std::vector<double> &pred,
            double &accuracy, Result *report = nullptr)
        {
            grad.assign(P, 0.0);
            pred.assign(D, 0.0);

            rvdl::lowrank::RasConfig rc;
            rc.tile_bits = cfg.sparse_tile_bits ? cfg.sparse_tile_bits : 6;
            rc.max_rank = cfg.ras_rank ? cfg.ras_rank
                                       : std::max<size_t>(2, std::min<size_t>(16, N / 64));
            rc.epsilon = cfg.ras_epsilon > 0.0 ? cfg.ras_epsilon : cfg.sparse_epsilon;
            rc.power_iters = 7;
            rc.seed = cfg.ras_seed;

            const State ket_v(ket, ket + N);
            LowRankState phi(ket_v, rc);

            if (phi.exact())
            {
                std::vector<size_t> bits(readout_bits, readout_bits + D);
                State ph = ket_v, lm(N, Complex(0, 0));
                auto fwd = [&](State &s, size_t g) {
                    const int type = gate_type[g];
                    const double a = (type == 2 || param[g] >= P)
                        ? 0.0 : theta[param[g]] * scale[g];
                    if (type == 0)
                    {
                        const size_t m = size_t(1) << q0[g];
                        const double c = std::cos(a * 0.5), sn = std::sin(a * 0.5);
                        for (size_t i = 0; i < N; ++i)
                        {
                            if (i & m) continue;
                            const Complex x = s[i], y = s[i | m];
                            s[i] = c * x - sn * y;
                            s[i | m] = sn * x + c * y;
                        }
                    }
                    else if (type == 1)
                    {
                        const size_t m = size_t(1) << q0[g];
                        const Complex ph2(std::cos(a), std::sin(a));
                        for (size_t i = 0; i < N; ++i) if (i & m) s[i] *= ph2;
                    }
                    else
                    {
                        const size_t cm = size_t(1) << q0[g], tm = size_t(1) << q1[g];
                        for (size_t i = 0; i < N; ++i)
                            if ((i & cm) && !(i & tm)) std::swap(s[i], s[i | tm]);
                    }
                };
                for (size_t g = 0; g < G; ++g)
                {
                    const size_t expert = cfg.gate_expert ? cfg.gate_expert[g] : 0;
                    if (gate_active(expert, cfg.moe_experts, cfg.active_expert)) fwd(ph, g);
                }
                pred.assign(D, 0.0);
                std::vector<double> gz(D, 0.0);
                double loss = 0.0;
                accuracy = 0.0;
                for (size_t j = 0; j < D; ++j)
                {
                    const size_t mask = size_t(1) << bits[j];
                    double p1 = 0.0;
                    for (size_t s = 0; s < N; ++s) if (s & mask) p1 += std::norm(ph[s]);
                    pred[j] = 1.0 - 2.0 * p1;
                    const int bit = (target_token >> j) & 1;
                    const double tg = 1.0 - 2.0 * bit;
                    const double d = pred[j] - tg;
                    loss += d * d;
                    gz[j] = 2.0 * d / double(D);
                    if (((pred[j] < 0.0) ? 1 : 0) == bit) accuracy += 1.0;
                }
                loss /= double(D);
                accuracy /= double(D);
                for (size_t s = 0; s < N; ++s)
                {
                    double c = 0.0;
                    for (size_t j = 0; j < D; ++j)
                        c += gz[j] * (((s >> bits[j]) & 1) ? -1.0 : 1.0);
                    lm[s] = c * ph[s];
                }
                for (size_t kk = G; kk > 0; --kk)
                {
                    const size_t g = kk - 1;
                    const size_t expert = cfg.gate_expert ? cfg.gate_expert[g] : 0;
                    if (!gate_active(expert, cfg.moe_experts, cfg.active_expert)) continue;
                    const int type = gate_type[g];
                    const size_t mask = size_t(1) << q0[g];
                    const size_t cmask = size_t(1) << q0[g];
                    const size_t tmask = size_t(1) << q1[g];
                    const double a = (type == 2 || param[g] >= P)
                        ? 0.0 : theta[param[g]] * scale[g];
                    double dot = 0.0;
                    if (type == 1)
                    {
                        for (size_t s = 0; s < N; ++s)
                            if (s & mask)
                                dot += (std::conj(lm[s]) * ph[s]).imag();
                    }
                    else if (type == 0)
                    {
                        for (size_t s = 0; s < N; ++s)
                        {
                            if (s & mask) continue;
                            dot += (std::conj(lm[s | mask]) * ph[s] -
                                    std::conj(lm[s]) * ph[s | mask]).real();
                        }
                    }
                    (void)cmask; (void)tmask;
                    if (type != 2 && param[g] < P)
                    {
                        if (type == 1) grad[param[g]] += -2.0 * scale[g] * dot;
                        else grad[param[g]] += scale[g] * dot;
                    }
                    const double saved = scale[g];
                    if (type != 2)
                    {
                        const double na = -a;
                        const size_t m = size_t(1) << q0[g];
                        if (type == 0)
                        {
                            const double c = std::cos(na * 0.5), sn = std::sin(na * 0.5);
                            for (size_t i = 0; i < N; ++i)
                            {
                                if (i & m) continue;
                                const Complex x = ph[i], y = ph[i | m];
                                ph[i] = c * x - sn * y;
                                ph[i | m] = sn * x + c * y;
                                const Complex lx = lm[i], ly = lm[i | m];
                                lm[i] = c * lx - sn * ly;
                                lm[i | m] = sn * lx + c * ly;
                            }
                        }
                        else
                        {
                            const Complex ph2(std::cos(na), std::sin(na));
                            for (size_t i = 0; i < N; ++i)
                                if (i & m) { ph[i] *= ph2; lm[i] *= ph2; }
                        }
                    }
                    else
                    {
                        for (size_t i = 0; i < N; ++i)
                            if ((i & cmask) && !(i & tmask))
                            { std::swap(ph[i], ph[i | tmask]); std::swap(lm[i], lm[i | tmask]); }
                    }
                    (void)saved;
                }
                if (report) { report->rank = 0; report->lambda_rank = 0; report->error_bound = 0.0; }
                return loss;
            }

            const size_t t = phi.tile_bits();
            const size_t L = phi.rows(), C = phi.cols();
            for (size_t g = 0; g < G; ++g)
            {
                const size_t expert = cfg.gate_expert ? cfg.gate_expert[g] : 0;
                if (!gate_active(expert, cfg.moe_experts, cfg.active_expert)) continue;
                const int type = gate_type[g];
                const double a = (type == 2 || param[g] >= P)
                    ? 0.0 : theta[param[g]] * scale[g];
                if (type == 0) phi.apply_ry(q0[g], a);
                else if (type == 1) phi.apply_rz(q0[g], a);
                else phi.apply_cnot(q0[g], q1[g]);
            }
            FactorSet fphi = extract(phi);

            std::vector<size_t> bits(readout_bits, readout_bits + D);
            {
                LowRankState tmp;
                tmp.set_dimension(N);
                tmp.set_factors(fphi.U, fphi.V, fphi.sigma, t, phi.delta());
                const auto p1 = tmp.probability_one(bits);
                pred.assign(D, 0.0);
                std::vector<double> gz(D, 0.0);
                double loss = 0.0;
                accuracy = 0.0;
                for (size_t j = 0; j < D; ++j)
                {
                    pred[j] = 1.0 - 2.0 * p1[j];
                    const int bit = (target_token >> j) & 1;
                    const double tg = 1.0 - 2.0 * bit;
                    const double d = pred[j] - tg;
                    loss += d * d;
                    gz[j] = 2.0 * d / double(D);
                    if (((pred[j] < 0.0) ? 1 : 0) == bit) accuracy += 1.0;
                }
                loss /= double(D);
                accuracy /= double(D);

                std::vector<double> av(L, 0.0), bv(C, 0.0);
                for (size_t j = 0; j < D; ++j)
                {
                    const size_t q = bits[j];
                    if (q >= t)
                    {
                        const size_t b = q - t;
                        for (size_t k = 0; k < L; ++k)
                            av[k] += gz[j] * (((k >> b) & 1) ? -1.0 : 1.0);
                    }
                    else
                    {
                        for (size_t c = 0; c < C; ++c)
                            bv[c] += gz[j] * (((c >> q) & 1) ? -1.0 : 1.0);
                    }
                }
                const bool use_a = std::any_of(av.begin(), av.end(),
                                               [](double x) { return x != 0.0; });
                const bool use_b = std::any_of(bv.begin(), bv.end(),
                                               [](double x) { return x != 0.0; });
                FactorSet flam;
                for (size_t i = 0; i < fphi.r(); ++i)
                {
                    if (use_a)
                    {
                        std::vector<Complex> ua(L);
                        for (size_t k = 0; k < L; ++k) ua[k] = av[k] * fphi.U[i][k];
                        flam.push(std::move(ua), fphi.V[i], fphi.sigma[i]);
                    }
                    if (use_b)
                    {
                        std::vector<Complex> vb(C);
                        for (size_t c = 0; c < C; ++c) vb[c] = bv[c] * fphi.V[i][c];
                        flam.push(fphi.U[i], std::move(vb), fphi.sigma[i]);
                    }
                }
                if (flam.r() == 0)
                    flam.push(std::vector<Complex>(L, Complex(0, 0)),
                              std::vector<Complex>(C, Complex(0, 0)), 0.0);

                LowRankState lam_state;
                lam_state.set_dimension(N);
                lam_state.set_factors(flam.U, flam.V, flam.sigma, t, 0.0,
                                      std::max<size_t>(flam.r() * 2, 4),
                                      rc.epsilon, rc.power_iters, rc.seed);

                for (size_t kk = G; kk > 0; --kk)
                {
                    const size_t g = kk - 1;
                    const size_t expert = cfg.gate_expert ? cfg.gate_expert[g] : 0;
                    if (!gate_active(expert, cfg.moe_experts, cfg.active_expert)) continue;
                    const int type = gate_type[g];
                    const double a = (type == 2 || param[g] >= P)
                        ? 0.0 : theta[param[g]] * scale[g];
                    const bool r0 = q0[g] >= t;
                    FactorSet fc = extract(const_cast<const LowRankState &>(phi));
                    FactorSet fl = extract(const_cast<const LowRankState &>(lam_state));
                    double dot = 0.0;
                    if (type == 1)
                        dot = r0 ? dot_side<true>(fc, fl, 1, q0[g] - t)
                                 : dot_side<false>(fc, fl, 1, q0[g]);
                    else if (type == 0)
                        dot = r0 ? dot_side<true>(fc, fl, 0, q0[g] - t)
                                 : dot_side<false>(fc, fl, 0, q0[g]);
                    if (type != 2 && param[g] < P)
                    {
                        if (type == 1) grad[param[g]] += -2.0 * scale[g] * dot;
                        else grad[param[g]] += scale[g] * dot;
                    }
                    const double inv = -a;
                    if (type == 2)
                    {
                        phi.apply_cnot(q0[g], q1[g]);
                        lam_state.apply_cnot(q0[g], q1[g]);
                        continue;
                    }
                    else
                    {
                        if (type == 0)
                        {
                            phi.apply_ry(q0[g], inv);
                            lam_state.apply_ry(q0[g], inv);
                        }
                        else
                        {
                            phi.apply_rz(q0[g], inv);
                            lam_state.apply_rz(q0[g], inv);
                        }
                    }
                }

                if (report)
                {
                    report->rank = phi.rank();
                    report->lambda_rank = lam_state.rank();
                    const double total_delta = std::min(1.0, phi.delta());
                    report->skipped_mass = total_delta;
                    report->error_bound = 2.0 * std::sqrt(total_delta) + total_delta;
                    report->refactors = phi.refactor_count();
                }
                return loss;
            }
        }





        inline void ras_adjoint_batch_pod(
            const unsigned char *gate_type, const size_t *q0, const size_t *q1,
            const size_t *param, const double *scale, size_t G,
            const double *ket_batch, size_t B, size_t N,
            const double *theta, const size_t *readout_bits, size_t D,
            const uint8_t *targets,
            double *grad, size_t P, double *loss_out, double *acc_out,
            const qlm::AdjointRuntimeConfig *cfg)
        {
            qlm::AdjointRuntimeConfig local;
            if (cfg) local = *cfg;
            std::vector<double> g(P, 0.0), p(D, 0.0);
            const Complex *ket = reinterpret_cast<const Complex *>(ket_batch);
            double total_delta = 0.0;
            size_t max_rank = 0, max_lam = 0, refac = 0;
            for (size_t b = 0; b < B; ++b)
            {
                double acc = 0.0;
                Result rep;
                const double loss = sample_adjoint_ras(
                    gate_type, q0, q1, param, scale, G, ket + b * N, N,
                    theta, readout_bits, D, targets[b], P, local, g, p, acc, &rep);
                loss_out[b] = loss;
                acc_out[b] = acc;
                for (size_t i = 0; i < P; ++i) grad[i] += g[i] / double(B);
                total_delta = std::max(total_delta, rep.skipped_mass);
                max_rank = std::max(max_rank, rep.rank);
                max_lam = std::max(max_lam, rep.lambda_rank);
                refac = std::max(refac, rep.refactors);
            }
            if (cfg)
            {
                if (cfg->skipped_mass) *cfg->skipped_mass = total_delta;
                if (cfg->error_bound)
                    *cfg->error_bound = 2.0 * std::sqrt(total_delta) + total_delta;
                if (cfg->active_tiles) *cfg->active_tiles = max_rank;
                if (cfg->total_tiles) *cfg->total_tiles = max_lam;
            }
            (void)refac;
        }

    }
}