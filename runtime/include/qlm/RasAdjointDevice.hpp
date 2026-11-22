#pragma once
#include "qhal/LowRankState.hpp"
#include "qhal/QuantumAccel.hpp"
#include "qlm/RasAdjoint.hpp"
#include "qlm/StatePolicy.hpp"

#include <Kokkos_Core.hpp>

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <vector>

namespace rvdl
{
    namespace ras_device
    {
        using DC = Kokkos::complex<double>;
        using Exec = rvdl::accel::ExecSpace;
        using Mem = rvdl::accel::MemSpace;
        using CView2 = Kokkos::View<DC **, Mem>;
        using DView1 = Kokkos::View<double *, Mem>;
        using U8View = Kokkos::View<uint8_t *, Mem>;

        inline CView2 upload_factors(const std::vector<std::vector<std::complex<double>>> &f,
                                     size_t rank, size_t axis)
        {
            CView2 v("RAS::factor", rank, axis);
            auto h = Kokkos::create_mirror_view(v);
            for (size_t i = 0; i < rank; ++i)
                for (size_t x = 0; x < axis; ++x)
                    h(i, x) = DC(f[i][x].real(), f[i][x].imag());
            Kokkos::deep_copy(v, h);
            return v;
        }
        inline DView1 upload_sigma(const std::vector<double> &s)
        {
            DView1 v("RAS::sigma", s.size());
            auto h = Kokkos::create_mirror_view(v);
            for (size_t i = 0; i < s.size(); ++i) h(i) = s[i];
            Kokkos::deep_copy(v, h);
            return v;
        }




        struct DevFactors
        {
            CView2 U, V;
            DView1 sigma;
            size_t rank = 0;
        };

        inline void apply_gate(DevFactors &a, DevFactors *b,
                               int type, size_t q0, size_t q1,
                               double angle_inverse, bool row_domain, bool both)
        {
            if (a.rank == 0) return;
            const size_t axis = row_domain ? a.U.extent(1) : a.V.extent(1);
            if (axis == 0) return;
            const size_t idx = row_domain ? q0 - 0 : q0;
            const size_t m = size_t(1) << idx;
            const size_t m2 = size_t(1) << (row_domain ? q1 : q1);
            const double c = (type == 2) ? 0.0 : std::cos(angle_inverse * 0.5);
            const double s = (type == 2) ? 0.0 : std::sin(angle_inverse * 0.5);
            const DC ph(std::cos(angle_inverse), std::sin(angle_inverse));
            const DC vph = Kokkos::conj(ph);

            auto AU = a.U, AV = a.V;
            const size_t RA = a.rank;
            CView2 BU, BV;
            size_t RB = 0;
            if (both && b) { BU = b->U; BV = b->V; RB = b->rank; }

            Kokkos::parallel_for(
                "RAS::Gate",
                Kokkos::MDRangePolicy<Exec, Kokkos::Rank<2>>({0, 0}, {RA, axis}),
                KOKKOS_LAMBDA(const size_t i, const size_t x) {
                    auto A = row_domain ? AU : AV;
                    auto B = row_domain ? BU : BV;
                    (void)B;
                    if (type == 0)
                    {
                        if (x & m) return;
                        const DC p = A(i, x), q = A(i, x | m);
                        A(i, x) = DC(c * p.real() - s * q.real(), c * p.imag() - s * q.imag());
                        A(i, x | m) = DC(s * p.real() + c * q.real(), s * p.imag() + c * q.imag());
                    }
                    else if (type == 1)
                    {
                        if (!(x & m)) return;
                        A(i, x) = row_domain ? A(i, x) * ph : A(i, x) * vph;
                    }
                    else
                    {
                        if (!(x & m) || (x & m2)) return;
                        const DC tmp = A(i, x);
                        A(i, x) = A(i, x | m2);
                        A(i, x | m2) = tmp;
                    }
                });

            if (both && b && RB)
            {
                Kokkos::parallel_for(
                    "RAS::GateB",
                    Kokkos::MDRangePolicy<Exec, Kokkos::Rank<2>>({0, 0}, {RB, axis}),
                    KOKKOS_LAMBDA(const size_t i, const size_t x) {
                        auto A = row_domain ? BU : BV;
                        if (type == 0)
                        {
                            if (x & m) return;
                            const DC p = A(i, x), q = A(i, x | m);
                            A(i, x) = DC(c * p.real() - s * q.real(), c * p.imag() - s * q.imag());
                            A(i, x | m) = DC(s * p.real() + c * q.real(), s * p.imag() + c * q.imag());
                        }
                        else if (type == 1)
                        {
                            if (!(x & m)) return;
                            A(i, x) = row_domain ? A(i, x) * ph : A(i, x) * vph;
                        }
                        else
                        {
                            if (!(x & m) || (x & m2)) return;
                            const DC tmp = A(i, x);
                            A(i, x) = A(i, x | m2);
                            A(i, x | m2) = tmp;
                        }
                    });
            }
        }




        inline DView1 batch_expectation(const DevFactors &f, const std::vector<size_t> &bits,
                                        size_t L, size_t C, size_t tile_bits)
        {
            DView1 out("RAS::p1", bits.size());
            Kokkos::deep_copy(out, 0.0);
            if (f.rank == 0 || bits.empty()) return out;

            U8View d_bits("RAS::bits", bits.size());
            {
                auto h = Kokkos::create_mirror_view(d_bits);
                for (size_t j = 0; j < bits.size(); ++j)
                    h(j) = static_cast<uint8_t>(bits[j]);
                Kokkos::deep_copy(d_bits, h);
            }
            const auto U = f.U; const auto V = f.V; const auto SG = f.sigma;
            const size_t R = f.rank;
            const size_t D = bits.size();

            CView2 gu("RAS::gu", R, R), gv("RAS::gv", R, R);
            Kokkos::deep_copy(gu, DC(0, 0));
            Kokkos::deep_copy(gv, DC(0, 0));
            Kokkos::parallel_for(
                "RAS::GramU",
                Kokkos::MDRangePolicy<Exec, Kokkos::Rank<3>>({0, 0, 0}, {R, R, L}),
                KOKKOS_LAMBDA(const size_t a, const size_t b, const size_t k) {
                    Kokkos::atomic_add(&gu(a, b), Kokkos::conj(U(a, k)) * U(b, k));
                });
            Kokkos::parallel_for(
                "RAS::GramV",
                Kokkos::MDRangePolicy<Exec, Kokkos::Rank<3>>({0, 0, 0}, {R, R, C}),
                KOKKOS_LAMBDA(const size_t a, const size_t b, const size_t c) {
                    Kokkos::atomic_add(&gv(a, b), Kokkos::conj(V(a, c)) * V(b, c));
                });
            Kokkos::fence();

            Kokkos::View<DC **, Mem> partial("RAS::partial", D, R * R);
            Kokkos::deep_copy(partial, DC(0, 0));
            Kokkos::parallel_for(
                "RAS::Partial",
                Kokkos::MDRangePolicy<Exec, Kokkos::Rank<2>>({0, 0}, {D, R * R}),
                KOKKOS_LAMBDA(const size_t j, const size_t ab) {
                    const size_t a = ab / R, b = ab % R;
                    const size_t q = d_bits(j);
                    DC s(0, 0);
                    if (q >= tile_bits)
                    {
                        const size_t m = size_t(1) << (q - tile_bits);
                        for (size_t k = 0; k < L; ++k)
                            if (k & m) s += U(a, k) * Kokkos::conj(U(b, k));
                    }
                    else
                    {
                        const size_t m = size_t(1) << q;
                        for (size_t c = 0; c < C; ++c)
                            if (c & m) s += Kokkos::conj(V(a, c)) * V(b, c);
                    }
                    partial(j, ab) = s;
                });
            Kokkos::fence();

            Kokkos::parallel_for(
                "RAS::P1",
                Kokkos::RangePolicy<Exec>(0, D),
                KOKKOS_LAMBDA(const size_t j) {
                    const bool row = d_bits(j) >= tile_bits;
                    double sum = 0.0;
                    for (size_t a = 0; a < R; ++a)
                        for (size_t b = 0; b < R; ++b)
                        {
                            const double w = SG(a) * SG(b);
                            const DC g = row ? gv(a, b) : gu(b, a);
                            sum += (w * partial(j, a * R + b) * g).real();
                        }
                    out(j) = sum;
                });
            Kokkos::fence();
            return out;
        }




        inline DView1 grad_dots(const DevFactors &phi, const DevFactors &lam,
                                int type, size_t idx, bool row_domain,
                                size_t axis, size_t other)
        {
            DView1 dots("RAS::dots", phi.rank * lam.rank);
            Kokkos::deep_copy(dots, 0.0);
            if (!phi.rank || !lam.rank) return dots;
            const size_t m = size_t(1) << idx;
            const size_t RP = phi.rank, RL = lam.rank;
            const CView2 PA = row_domain ? phi.U : phi.V;
            const CView2 LA = row_domain ? lam.U : lam.V;
            const CView2 PB = row_domain ? phi.V : phi.U;
            const CView2 LB = row_domain ? lam.V : lam.U;
            const auto SP = phi.sigma; const auto SL = lam.sigma;

            Kokkos::parallel_for(
                "RAS::Dot",
                Kokkos::MDRangePolicy<Exec, Kokkos::Rank<2>>({0, 0}, {RP, RL}),
                KOKKOS_LAMBDA(const size_t a, const size_t b) {
                    DC g1(0, 0), g2(0, 0), g3(0, 0), h(0, 0);
                    for (size_t x = 0; x < axis; ++x)
                    {
                        if (type == 1)
                        {
                            if (!(x & m)) continue;
                            g1 += Kokkos::conj(LA(b, x)) * PA(a, x);
                        }
                        else
                        {
                            if (x & m) continue;
                            g2 += Kokkos::conj(LA(b, x | m)) * PA(a, x);
                            g3 += Kokkos::conj(LA(b, x)) * PA(a, x | m);
                        }
                    }
                    for (size_t y = 0; y < other; ++y)
                        h += Kokkos::conj(PB(a, y)) * LB(b, y);
                    const double w = SP(a) * SL(b);
                    const DC v = (type == 1) ? w * g1 * h : w * (g2 - g3) * h;
                    dots(a * RL + b) = type == 1 ? v.imag() : v.real();
                });
            Kokkos::fence();
            return dots;
        }

        inline DevFactors build_lambda(const DevFactors &phi, const DView1 &av, const DView1 &bv,
                                       bool use_a, bool use_b)
        {
            const size_t R = phi.rank;
            const size_t L = phi.U.extent(1), C = phi.V.extent(1);
            const size_t RL = (use_a ? R : 0) + (use_b ? R : 0);
            DevFactors out;
            out.rank = RL;
            out.U = CView2("RAS::lamU", RL, L);
            out.V = CView2("RAS::lamV", RL, C);
            out.sigma = DView1("RAS::lamS", RL);
            const auto PU = phi.U; const auto PV = phi.V; const auto PS = phi.sigma;
            const auto OU = out.U; const auto OV = out.V; const auto OS = out.sigma;
            Kokkos::parallel_for(
                "RAS::Lambda",
                Kokkos::MDRangePolicy<Exec, Kokkos::Rank<2>>({0, 0}, {R, L}),
                KOKKOS_LAMBDA(const size_t i, const size_t k) {
                    if (use_a) OU(i, k) = av(k) * PU(i, k);
                    if (use_b) OU(i + R, k) = PU(i, k);
                });
            Kokkos::parallel_for(
                "RAS::LambdaV",
                Kokkos::MDRangePolicy<Exec, Kokkos::Rank<2>>({0, 0}, {R, C}),
                KOKKOS_LAMBDA(const size_t i, const size_t c) {
                    if (use_a) OV(i, c) = PV(i, c);
                    if (use_b) OV(i + R, c) = bv(c) * PV(i, c);
                    if (use_a) OS(i) = PS(i);
                    if (use_b) OS(i + R) = PS(i);
                });
            Kokkos::fence();
            return out;
        }

        struct DeviceResult
        {
            double loss = 0.0, accuracy = 0.0, error_bound = 0.0;
            size_t rank = 0, lambda_rank = 0;
            std::vector<double> grad, pred;
        };




        inline DeviceResult sample_device(
            const unsigned char *gt, const size_t *q0, const size_t *q1,
            const size_t *prm, const double *sc, size_t G,
            const std::complex<double> *ket, size_t N,
            const double *theta, const size_t *readout_bits, size_t D,
            uint8_t target, size_t P, const qlm::AdjointRuntimeConfig &cfg)
        {
            DeviceResult out;
            out.grad.assign(P, 0.0);
            out.pred.assign(D, 0.0);

            rvdl::lowrank::RasConfig rc;
            rc.tile_bits = cfg.sparse_tile_bits ? cfg.sparse_tile_bits : 6;
            rc.max_rank = cfg.ras_rank ? cfg.ras_rank
                                       : std::max<size_t>(2, std::min<size_t>(16, N / 64));
            rc.epsilon = cfg.ras_epsilon > 0.0 ? cfg.ras_epsilon : cfg.sparse_epsilon;
            rc.power_iters = 7;
            rc.seed = cfg.ras_seed;

            const std::vector<std::complex<double>> kv(ket, ket + N);
            rvdl::lowrank::LowRankState st(kv, rc);
            if (st.exact())
            {
                std::vector<double> acc_one(1, 0.0), tmp(1, 0.0);
                rvdl::ras_adjoint::Result rep;
                const double loss = rvdl::ras_adjoint::sample_adjoint_ras(
                    gt, q0, q1, prm, sc, G, ket, N, theta, readout_bits, D,
                    target, P, cfg, out.grad, out.pred, acc_one[0], &rep);
                out.loss = loss;
                out.accuracy = acc_one[0];
                return out;
            }

            const size_t t = st.tile_bits();
            const size_t L = st.rows(), C = st.cols();
            DevFactors phi;
            phi.U = upload_factors(st.U(), st.rank(), L);
            phi.V = upload_factors(st.V(), st.rank(), C);
            phi.sigma = upload_sigma(st.sigma());
            phi.rank = st.rank();

            for (size_t g = 0; g < G; ++g)
            {
                const size_t expert = cfg.gate_expert ? cfg.gate_expert[g] : 0;
                if (!rvdl::ras_adjoint::gate_active(expert, cfg.moe_experts, cfg.active_expert)) continue;
                const int type = gt[g];
                const double a = (type == 2 || prm[g] >= P) ? 0.0 : theta[prm[g]] * sc[g];
                const bool r0 = q0[g] >= t, r1 = q1[g] >= t;
                if (type == 2 && r0 != r1)
                {
                    Kokkos::fence();
                    rvdl::lowrank::LowRankState nb(st);
                    nb.apply_cnot(q0[g], q1[g]);
                    st = std::move(nb);
                    phi.U = upload_factors(st.U(), st.rank(), L);
                    phi.V = upload_factors(st.V(), st.rank(), C);
                    phi.sigma = upload_sigma(st.sigma());
                    phi.rank = st.rank();
                    continue;
                }
                const bool row = r0;
                const size_t local = row ? q0[g] - t : q0[g];
                const size_t local2 = (type == 2) ? (r1 ? q1[g] - t : q1[g]) : 0;
                apply_gate(phi, nullptr, type, local, local2, a, row, false);
            }
            Kokkos::fence();

            std::vector<size_t> bits(readout_bits, readout_bits + D);
            const auto d_p1 = batch_expectation(phi, bits, L, C, t);
            auto h_p1 = Kokkos::create_mirror_view_and_copy(Kokkos::HostSpace(), d_p1);
            std::vector<double> gz(D, 0.0);
            out.loss = 0.0;
            out.accuracy = 0.0;
            for (size_t j = 0; j < D; ++j)
            {
                out.pred[j] = 1.0 - 2.0 * h_p1(j);
                const int bit = (target >> j) & 1;
                const double tg = 1.0 - 2.0 * bit;
                const double d = out.pred[j] - tg;
                out.loss += d * d;
                gz[j] = 2.0 * d / double(D);
                if (((out.pred[j] < 0.0) ? 1 : 0) == bit) out.accuracy += 1.0;
            }
            out.loss /= double(D);
            out.accuracy /= double(D);

            std::vector<double> av(L, 0.0), bv(C, 0.0);
            for (size_t j = 0; j < D; ++j)
            {
                const size_t q = bits[j];
                if (q >= t)
                {
                    const size_t b = q - t;
                    for (size_t k = 0; k < L; ++k) av[k] += gz[j] * (((k >> b) & 1) ? -1.0 : 1.0);
                }
                else
                {
                    for (size_t c = 0; c < C; ++c) bv[c] += gz[j] * (((c >> q) & 1) ? -1.0 : 1.0);
                }
            }
            const bool use_a = std::any_of(av.begin(), av.end(), [](double x) { return x != 0.0; });
            const bool use_b = std::any_of(bv.begin(), bv.end(), [](double x) { return x != 0.0; });
            DView1 dav("RAS::av", L), dbv("RAS::bv", C);
            {
                auto ha = Kokkos::create_mirror_view(dav), hb = Kokkos::create_mirror_view(dbv);
                for (size_t k = 0; k < L; ++k) ha(k) = av[k];
                for (size_t c = 0; c < C; ++c) hb(c) = bv[c];
                Kokkos::deep_copy(dav, ha);
                Kokkos::deep_copy(dbv, hb);
            }
            DevFactors lam = build_lambda(phi, dav, dbv, use_a, use_b);
            out.lambda_rank = lam.rank;
            out.rank = phi.rank;
            out.error_bound = st.error_bound();

            for (size_t kk = G; kk > 0; --kk)
            {
                const size_t g = kk - 1;
                const size_t expert = cfg.gate_expert ? cfg.gate_expert[g] : 0;
                if (!rvdl::ras_adjoint::gate_active(expert, cfg.moe_experts, cfg.active_expert)) continue;
                const int type = gt[g];
                const bool row = q0[g] >= t;
                const bool r1 = q1[g] >= t;
                if (type == 2 && row != r1)
                {
                    Kokkos::fence();
                    rvdl::lowrank::LowRankState nb(st);
                    nb.apply_cnot(q0[g], q1[g]);
                    st = std::move(nb);
                    phi.U = upload_factors(st.U(), st.rank(), L);
                    phi.V = upload_factors(st.V(), st.rank(), C);
                    phi.sigma = upload_sigma(st.sigma());
                    phi.rank = st.rank();
                    lam = build_lambda(phi, dav, dbv, use_a, use_b);
                    continue;
                }
                if (type != 2)
                {
                    const size_t local = row ? q0[g] - t : q0[g];
                    const auto d = grad_dots(phi, lam, type, local, row,
                                             row ? L : C, row ? C : L);
                    auto hd = Kokkos::create_mirror_view_and_copy(Kokkos::HostSpace(), d);
                    double dot = 0.0;
                    for (size_t i = 0; i < hd.extent(0); ++i) dot += hd(i);
                    if (prm[g] < P)
                    {
                        if (type == 1) out.grad[prm[g]] += -2.0 * sc[g] * dot;
                        else out.grad[prm[g]] += sc[g] * dot;
                    }
                }
                const double a = (type == 2 || prm[g] >= P) ? 0.0 : theta[prm[g]] * sc[g];
                const size_t local2 = (type == 2) ? (r1 ? q1[g] - t : q1[g]) : 0;
                apply_gate(phi, &lam, type, row ? q0[g] - t : q0[g], local2,
                           -a, row, true);
            }
            Kokkos::fence();
            return out;
        }

        inline void ras_device_batch_pod(
            const unsigned char *gt, const size_t *q0, const size_t *q1,
            const size_t *prm, const double *sc, size_t G,
            const double *ket_batch, size_t B, size_t N,
            const double *theta, const size_t *bits, size_t D,
            const uint8_t *targets,
            double *grad, size_t P, double *loss_out, double *acc_out,
            const qlm::AdjointRuntimeConfig *cfg)
        {
            qlm::AdjointRuntimeConfig local = cfg ? *cfg : qlm::AdjointRuntimeConfig{};
            const auto *kp = reinterpret_cast<const std::complex<double> *>(ket_batch);
            double bound = 0.0;
            size_t max_rank = 0, max_lam = 0;
            for (size_t b = 0; b < B; ++b)
            {
                const DeviceResult r = sample_device(gt, q0, q1, prm, sc, G, kp + b * N, N,
                                                     theta, bits, D, targets[b], P, local);
                loss_out[b] = r.loss;
                acc_out[b] = r.accuracy;
                for (size_t i = 0; i < P; ++i) grad[i] += r.grad[i] / double(B);
                bound = std::max(bound, r.error_bound);
                max_rank = std::max(max_rank, r.rank);
                max_lam = std::max(max_lam, r.lambda_rank);
            }
            if (cfg)
            {
                if (cfg->error_bound) *cfg->error_bound = bound;
                if (cfg->skipped_mass) *cfg->skipped_mass = bound;
                if (cfg->active_tiles) *cfg->active_tiles = max_rank;
                if (cfg->total_tiles) *cfg->total_tiles = max_lam;
            }
        }

    }
}