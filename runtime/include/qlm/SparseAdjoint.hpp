#pragma once
#include "qhal/SparseAmplitude.hpp"
#include "qlm/StatePolicy.hpp"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstddef>
#include <vector>

namespace rvdl
{
    namespace sparse_adjoint
    {
        using Complex = rvdl::sparse::Complex;
        using SparseState = rvdl::sparse::SparseState;
        static constexpr size_t SHARED_GATE = static_cast<size_t>(-1);

        inline bool gate_active(size_t expert, size_t experts, size_t active)
        {
            return experts <= 1 || expert == SHARED_GATE || expert == active;
        }

        struct Result
        {
            double loss = 0.0;
            double accuracy = 0.0;
            double skipped_mass = 0.0;
            double error_bound = 0.0;
            size_t active_tiles = 0;
            size_t total_tiles = 0;
            std::vector<double> grad;
            std::vector<double> pred;
            std::vector<double> sample_loss;
            std::vector<double> sample_acc;
        };

        inline double sample_adjoint(
            const unsigned char *gate_type, const size_t *q0, const size_t *q1,
            const size_t *param, const double *scale, size_t G,
            const Complex *ket, size_t N,
            const double *theta, const size_t *readout_bits, size_t D,
            uint8_t target_token, size_t P,
            const qlm::AdjointRuntimeConfig &cfg,
            std::vector<double> &grad, std::vector<double> &pred,
            double &accuracy, Result *report = nullptr)
        {
            const size_t t = std::min<size_t>(cfg.sparse_tile_bits ? cfg.sparse_tile_bits : 6, 20);
            SparseState phi(std::vector<Complex>(ket, ket + N), t, cfg.sparse_epsilon,
                            cfg.sparse_epsilon > 0.0);
            SparseState lambda = phi.zero_like();
            const size_t C = phi.tiles();
            const size_t L = phi.tile_len();

            if (report)
            {
                report->skipped_mass = std::max(report->skipped_mass, phi.skipped_mass());
                report->error_bound = std::max(report->error_bound, phi.error_bound());
                report->active_tiles += phi.active_tiles();
                report->total_tiles += phi.tiles();
            }
            if (D == 0 || C == 0 || L == 0) return 0.0;

            for (size_t g = 0; g < G; ++g)
            {
                const size_t expert = cfg.gate_expert ? cfg.gate_expert[g] : 0;
                if (!gate_active(expert, cfg.moe_experts, cfg.active_expert)) continue;
                const double a = (gate_type[g] == 2 || param[g] >= P)
                    ? 0.0 : theta[param[g]] * scale[g];
                if (gate_type[g] == 0) phi.apply_ry(q0[g], a);
                else if (gate_type[g] == 1) phi.apply_rz(q0[g], a);
                else phi.apply_cnot(q0[g], q1[g]);
            }

            std::vector<size_t> bits(readout_bits, readout_bits + D);
            const std::vector<double> p1 = phi.probability_one(bits);
            pred.assign(D, 0.0);
            std::vector<double> gz(D, 0.0);
            double loss = 0.0;
            accuracy = 0.0;
            for (size_t j = 0; j < D; ++j)
            {
                pred[j] = 1.0 - 2.0 * p1[j];
                const int bit = (target_token >> j) & 1;
                const double target = 1.0 - 2.0 * bit;
                const double d = pred[j] - target;
                loss += d * d;
                gz[j] = 2.0 * d / static_cast<double>(D);
                if (((pred[j] < 0.0) ? 1 : 0) == bit) accuracy += 1.0;
            }
            loss /= static_cast<double>(D);
            accuracy /= static_cast<double>(D);

            for (size_t c : phi.active_tile_ids())
            {
                auto *pa = phi.tile_amps(c);
                auto *pl = lambda.tile_amps(c);
                if (!pa || !pl) continue;
                for (size_t k = 0; k < L && k < pa->size(); ++k)
                {
                    const size_t i = (k << t) | c;
                    double coef = 0.0;
                    for (size_t j = 0; j < D; ++j)
                        coef += gz[j] * (((i >> bits[j]) & 1) ? -1.0 : 1.0);
                    (*pl)[k] = coef * (*pa)[k];
                }
            }

            grad.assign(P, 0.0);
            const std::vector<size_t> ids = phi.active_tile_ids();
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
                for (size_t c : ids)
                {
                    auto *pa = phi.tile_amps(c);
                    auto *pl = lambda.tile_amps(c);
                    if (!pa || !pl) continue;
                    for (size_t k = 0; k < L && k < pa->size(); ++k)
                    {
                        const size_t i = (k << t) | c;
                        if (type == 1)
                        {
                            if (!(i & mask)) continue;
                            dot += (std::conj((*pl)[k]) * (*pa)[k]).imag();
                        }
                        else if (type == 0)
                        {
                            if (i & mask) continue;
                            const size_t j = i | mask;
                            const Complex *p1p = phi.at(j);
                            const Complex *l1p = lambda.at(j);
                            if (!p1p || !l1p) continue;
                            dot += (std::conj(*l1p) * (*pa)[k] -
                                    std::conj((*pl)[k]) * *p1p).real();
                            (void)cmask; (void)tmask;
                        }
                    }
                }
                if (type != 2 && param[g] < P)
                {
                    if (type == 1) grad[param[g]] += -2.0 * scale[g] * dot;
                    else if (type == 0) grad[param[g]] += scale[g] * dot;
                }

                const double inv = -a;
                if (type == 0) { phi.apply_ry(q0[g], inv); lambda.apply_ry(q0[g], inv); }
                else if (type == 1) { phi.apply_rz(q0[g], inv); lambda.apply_rz(q0[g], inv); }
                else { phi.apply_cnot(q0[g], q1[g]); lambda.apply_cnot(q0[g], q1[g]); }
            }
            return loss;
        }

        inline Result batch(
            const unsigned char *gate_type, const size_t *q0, const size_t *q1,
            const size_t *param, const double *scale, size_t G,
            const double *ket_batch, size_t B, size_t N,
            const double *theta, const size_t *readout_bits, size_t D,
            const uint8_t *targets, size_t P,
            const qlm::AdjointRuntimeConfig &cfg)
        {
            Result out;
            if (B == 0 || N == 0 || D == 0 || G == 0) return out;
            const Complex *ket = reinterpret_cast<const Complex *>(ket_batch);
            out.grad.assign(P, 0.0);
            out.pred.assign(B * D, 0.0);
            out.sample_loss.assign(B, 0.0);
            out.sample_acc.assign(B, 0.0);
            for (size_t b = 0; b < B; ++b)
            {
                std::vector<double> g;
                std::vector<double> p;
                double acc = 0.0;
                const double loss = sample_adjoint(
                    gate_type, q0, q1, param, scale, G, ket + b * N, N,
                    theta, readout_bits, D, targets[b], P, cfg, g, p, acc, &out);
                out.sample_loss[b] = loss;
                out.sample_acc[b] = acc;
                out.loss += loss;
                out.accuracy += acc;
                for (size_t j = 0; j < P && j < g.size(); ++j) out.grad[j] += g[j];
                for (size_t j = 0; j < D && j < p.size(); ++j) out.pred[b * D + j] = p[j];
            }
            for (double &x : out.grad) x /= static_cast<double>(B);
            out.loss /= static_cast<double>(B);
            out.accuracy /= static_cast<double>(B);
            return out;
        }

        inline void sparse_adjoint_batch_pod(
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
            const Result r = batch(gate_type, q0, q1, param, scale, G, ket_batch, B, N,
                                   theta, readout_bits, D, targets, P, local);
            std::copy(r.grad.begin(), r.grad.end(), grad);
            for (size_t b = 0; b < B; ++b)
            {
                loss_out[b] = b < r.sample_loss.size() ? r.sample_loss[b] : r.loss;
                acc_out[b] = b < r.sample_acc.size() ? r.sample_acc[b] : r.accuracy;
            }
            if (cfg)
            {
                if (cfg->skipped_mass) *cfg->skipped_mass = r.skipped_mass;
                if (cfg->error_bound) *cfg->error_bound = r.error_bound;
                if (cfg->active_tiles) *cfg->active_tiles = r.active_tiles;
                if (cfg->total_tiles) *cfg->total_tiles = r.total_tiles;
            }
        }
    }
}