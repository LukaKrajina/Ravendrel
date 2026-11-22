#pragma once
#include "qhal/QuantumAccel.hpp"
#include "qhal/SparseAmplitude.hpp"
#include "qlm/QLM.hpp"
#include "qlm/SparseAdjoint.hpp"

#include <Kokkos_Core.hpp>

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

namespace rvdl
{
    namespace accel
    {
        namespace detail_batch
        {
            using GateOp = qlm::train_detail::GateOp;
            using Complex = qlm::train_detail::Complex;
            using DComplex = rvdl::accel::DComplex;
            using Exec = rvdl::accel::ExecSpace;
            using Mem = rvdl::accel::MemSpace;
            using BatchCView = Kokkos::View<DComplex **, Mem>;
            using BatchDView = Kokkos::View<double **, Mem>;
            using DView = Kokkos::View<double *, Mem>;
            using U8View = Kokkos::View<uint8_t *, Mem>;

            inline void apply_gate_batch(BatchCView sv, size_t B, size_t N,
                                         const GateOp &g, double angle, bool inverse)
            {
                const int type = g.type == GateOp::Type::RY ? 0
                               : g.type == GateOp::Type::RZ ? 1 : 2;
                const uint64_t mask = uint64_t(1) << g.q0;
                const uint64_t cmask = uint64_t(1) << g.q0;
                const uint64_t tmask = uint64_t(1) << g.q1;
                const double a = inverse ? -angle : angle;
                const double c = std::cos(a * 0.5), s = std::sin(a * 0.5);
                const double cr = std::cos(a), ci = std::sin(a);
                Kokkos::MDRangePolicy<Exec, Kokkos::Rank<2>> policy({0, 0}, {B, N});
                Kokkos::parallel_for("BAT::Gate", policy,
                    KOKKOS_LAMBDA(const size_t b, const size_t i) {
                        if (type == 0)
                        {
                            if (i & mask) return;
                            const size_t j = i | mask;
                            const DComplex x = sv(b, i), y = sv(b, j);
                            sv(b, i) = DComplex(c * x.real() - s * y.real(), c * x.imag() - s * y.imag());
                            sv(b, j) = DComplex(s * x.real() + c * y.real(), s * x.imag() + c * y.imag());
                        }
                        else if (type == 1)
                        {
                            if (!(i & mask)) return;
                            const DComplex x = sv(b, i);
                            sv(b, i) = DComplex(x.real() * cr - x.imag() * ci,
                                                x.real() * ci + x.imag() * cr);
                        }
                        else
                        {
                            if (!(i & cmask) || (i & tmask)) return;
                            const size_t j = i | tmask;
                            const DComplex t = sv(b, i);
                            sv(b, i) = sv(b, j);
                            sv(b, j) = t;
                        }
                    });
            }

            inline void apply_gate_both_batch(BatchCView a, BatchCView b, size_t B, size_t N,
                                              const GateOp &g, double angle)
            {
                const int type = g.type == GateOp::Type::RY ? 0
                               : g.type == GateOp::Type::RZ ? 1 : 2;
                const uint64_t mask = uint64_t(1) << g.q0;
                const uint64_t cmask = uint64_t(1) << g.q0;
                const uint64_t tmask = uint64_t(1) << g.q1;
                const double a_ang = -angle;
                const double c = std::cos(a_ang * 0.5), s = std::sin(a_ang * 0.5);
                const double cr = std::cos(a_ang), ci = std::sin(a_ang);
                Kokkos::MDRangePolicy<Exec, Kokkos::Rank<2>> policy({0, 0}, {B, N});
                Kokkos::parallel_for("BAT::InverseBoth", policy,
                    KOKKOS_LAMBDA(const size_t bb, const size_t i) {
                        if (type == 0)
                        {
                            if (i & mask) return;
                            const size_t j = i | mask;
                            const DComplex a0 = a(bb, i), a1 = a(bb, j);
                            const DComplex b0 = b(bb, i), b1 = b(bb, j);
                            a(bb, i) = DComplex(c * a0.real() - s * a1.real(), c * a0.imag() - s * a1.imag());
                            a(bb, j) = DComplex(s * a0.real() + c * a1.real(), s * a0.imag() + c * a1.imag());
                            b(bb, i) = DComplex(c * b0.real() - s * b1.real(), c * b0.imag() - s * b1.imag());
                            b(bb, j) = DComplex(s * b0.real() + c * b1.real(), s * b0.imag() + c * b1.imag());
                        }
                        else if (type == 1)
                        {
                            if (!(i & mask)) return;
                            const DComplex x = a(bb, i), y = b(bb, i);
                            a(bb, i) = DComplex(x.real() * cr - x.imag() * ci,
                                                x.real() * ci + x.imag() * cr);
                            b(bb, i) = DComplex(y.real() * cr - y.imag() * ci,
                                                y.real() * ci + y.imag() * cr);
                        }
                        else
                        {
                            if (!(i & cmask) || (i & tmask)) return;
                            const size_t j = i | tmask;
                            const DComplex ta = a(bb, i), tb = b(bb, i);
                            a(bb, i) = a(bb, j); a(bb, j) = ta;
                            b(bb, i) = b(bb, j); b(bb, j) = tb;
                        }
                    });
            }
        }

        struct BatchAdjointResult
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






        inline BatchAdjointResult device_adjoint_batch(
            const std::vector<detail_batch::GateOp> &gates,
            const detail_batch::Complex *ket_data, size_t B, size_t N,
            const double *theta,
            const std::vector<size_t> &readout_bits,
            const uint8_t *targets,
            size_t param_count,
            size_t tile_bits,
            double tile_epsilon)
        {
            using namespace detail_batch;
            BatchAdjointResult out;
            const size_t D = readout_bits.size();
            if (B == 0 || N == 0 || D == 0 || gates.empty()) return out;

            size_t tb = std::min<size_t>(tile_bits, 12);
            while (tb > 0 && (size_t(1) << tb) > N) --tb;
            const size_t C = size_t(1) << tb;
            const size_t tile_len = N / C;
            const double eps2 = tile_epsilon * tile_epsilon;
            const bool prune = tile_epsilon > 0.0;

            BatchCView phi("BAT::phi", B, N), lambda("BAT::lambda", B, N);
            {
                auto h_phi = Kokkos::create_mirror_view(phi);
                for (size_t b = 0; b < B; ++b)
                    for (size_t i = 0; i < N; ++i)
                        h_phi(b, i) = DComplex(ket_data[b * N + i].real(), ket_data[b * N + i].imag());
                Kokkos::deep_copy(phi, h_phi);
            }

            for (const auto &g : gates)
                apply_gate_batch(phi, B, N, g, theta[g.param] * g.scale, false);
            Kokkos::fence();

            U8View d_bits("BAT::bits", D);
            {
                auto hb = Kokkos::create_mirror_view(d_bits);
                for (size_t j = 0; j < D; ++j) hb(j) = static_cast<uint8_t>(readout_bits[j]);
                Kokkos::deep_copy(d_bits, hb);
            }
            BatchDView d_pred("BAT::pred", B, D);
            Kokkos::deep_copy(d_pred, 0.0);
            {
                auto sv = phi;
                Kokkos::MDRangePolicy<Exec, Kokkos::Rank<2>> policy({0, 0}, {B, N});
                Kokkos::parallel_for("BAT::P1", policy,
                    KOKKOS_LAMBDA(const size_t b, const size_t i) {
                        const DComplex x = sv(b, i);
                        const double p = x.real() * x.real() + x.imag() * x.imag();
                        if (p == 0.0) return;
                        for (size_t j = 0; j < D; ++j)
                            if (i & (size_t(1) << d_bits(j)))
                                Kokkos::atomic_add(&d_pred(b, j), p);
                    });
                Kokkos::fence();
                auto hp = Kokkos::create_mirror_view_and_copy(Kokkos::HostSpace(), d_pred);
                for (size_t b = 0; b < B; ++b)
                    for (size_t j = 0; j < D; ++j)
                        hp(b, j) = 1.0 - 2.0 * hp(b, j);
                Kokkos::deep_copy(d_pred, hp);
            }

            U8View d_targets("BAT::targets", B);
            {
                auto ht = Kokkos::create_mirror_view(d_targets);
                for (size_t b = 0; b < B; ++b) ht(b) = targets[b];
                Kokkos::deep_copy(d_targets, ht);
            }
            {
                auto sv = phi;
                auto lam = lambda;
                Kokkos::MDRangePolicy<Exec, Kokkos::Rank<2>> policy({0, 0}, {B, N});
                Kokkos::parallel_for("BAT::Lambda", policy,
                    KOKKOS_LAMBDA(const size_t b, const size_t i) {
                        double c = 0.0;
                        for (size_t j = 0; j < D; ++j)
                        {
                            const int bit = (d_targets(b) >> j) & 1;
                            const double target = 1.0 - 2.0 * bit;
                            const double gz = 2.0 * (d_pred(b, j) - target) / double(D);
                            const double sign = ((i >> d_bits(j)) & 1U) ? -1.0 : 1.0;
                            c += gz * sign;
                        }
                        const DComplex x = sv(b, i);
                        lam(b, i) = DComplex(c * x.real(), c * x.imag());
                    });
                Kokkos::fence();
            }

            BatchDView d_tile_mass("BAT::tileMass", B, C);
            Kokkos::deep_copy(d_tile_mass, 0.0);
            U8View d_active("BAT::active", B * C);
            Kokkos::deep_copy(d_active, uint8_t(1));
            if (prune)
            {
                auto sv = phi;
                auto lam = lambda;
                Kokkos::MDRangePolicy<Exec, Kokkos::Rank<2>> policy({0, 0}, {B, C});
                Kokkos::parallel_for("BAT::TileMass", policy,
                    KOKKOS_LAMBDA(const size_t b, const size_t c) {
                        double m = 0.0;
                        for (size_t k = 0; k < tile_len; ++k)
                        {
                            const size_t i = (k << tb) | c;
                            const DComplex x = sv(b, i), y = lam(b, i);
                            m += x.real() * x.real() + x.imag() * x.imag()
                               + y.real() * y.real() + y.imag() * y.imag();
                        }
                        d_tile_mass(b, c) = m;
                        d_active(b * C + c) = (m >= eps2) ? 1 : 0;
                    });
                Kokkos::fence();
            }

            const size_t G = gates.size();
            DView d_dots("BAT::dots", G);
            Kokkos::deep_copy(d_dots, 0.0);
            for (size_t k = G; k > 0; --k)
            {
                const GateOp &g = gates[k - 1];
                if (g.type != GateOp::Type::CNOT && g.param < param_count)
                {
                    const int type = g.type == GateOp::Type::RZ ? 1 : 2;
                    const uint64_t mask = uint64_t(1) << g.q0;
                    auto sv = phi;
                    auto lam = lambda;
                    Kokkos::MDRangePolicy<Exec, Kokkos::Rank<2>> policy({0, 0}, {B, C});
                    Kokkos::parallel_for("BAT::GradTile", policy,
                        KOKKOS_LAMBDA(const size_t b, const size_t c) {
                            if (!d_active(b * C + c)) return;
                            double local = 0.0;
                            for (size_t kk = 0; kk < tile_len; ++kk)
                            {
                                const size_t i = (kk << tb) | c;
                                if (type == 1)
                                {
                                    if (!(i & mask)) continue;
                                    const DComplex l = lam(b, i), p = sv(b, i);
                                    local += l.real() * p.imag() - l.imag() * p.real();
                                }
                                else
                                {
                                    if (i & mask) continue;
                                    const size_t j = i | mask;
                                    const DComplex l1 = lam(b, j), p0 = sv(b, i);
                                    const DComplex l0 = lam(b, i), p1 = sv(b, j);
                                    local += (l1.real() * p0.real() + l1.imag() * p0.imag())
                                           - (l0.real() * p1.real() + l0.imag() * p1.imag());
                                }
                            }
                            Kokkos::atomic_add(&d_dots(k - 1), local);
                        });
                    Kokkos::fence();
                }
                apply_gate_both_batch(phi, lambda, B, N, g, theta[g.param] * g.scale);
            }
            Kokkos::fence();

            auto h_dots = Kokkos::create_mirror_view_and_copy(Kokkos::HostSpace(), d_dots);
            auto h_pred = Kokkos::create_mirror_view_and_copy(Kokkos::HostSpace(), d_pred);
            out.grad.assign(param_count, 0.0);
            out.pred.assign(B * D, 0.0);
            out.sample_loss.assign(B, 0.0);
            out.sample_acc.assign(B, 0.0);
            for (size_t j = 0; j < G; ++j)
            {
                const GateOp &g = gates[j];
                if (g.type == GateOp::Type::CNOT || g.param >= param_count) continue;
                const double mul = (g.type == GateOp::Type::RZ) ? -2.0 : 1.0;
                out.grad[g.param] += mul * (g.scale / double(B)) * h_dots(j);
            }
            double loss_sum = 0.0, acc_sum = 0.0;
            for (size_t b = 0; b < B; ++b)
            {
                double lb = 0.0;
                size_t correct = 0;
                for (size_t j = 0; j < D; ++j)
                {
                    const int bit = (targets[b] >> j) & 1;
                    const double want = bit ? -1.0 : 1.0;
                    const double got = h_pred(b, j);
                    lb += (got - want) * (got - want);
                    if ((got >= 0) == (want >= 0)) ++correct;
                    out.pred[b * D + j] = got;
                }
                out.sample_loss[b] = lb / double(D);
                out.sample_acc[b] = double(correct) / double(D);
                loss_sum += out.sample_loss[b];
                acc_sum += out.sample_acc[b];
            }
            out.loss = loss_sum / double(B);
            out.accuracy = acc_sum / double(B);

            if (prune)
            {
                auto hm = Kokkos::create_mirror_view_and_copy(Kokkos::HostSpace(), d_tile_mass);
                auto ha = Kokkos::create_mirror_view_and_copy(Kokkos::HostSpace(), d_active);
                double skipped = 0.0;
                size_t active = 0;
                for (size_t b = 0; b < B; ++b)
                    for (size_t c = 0; c < C; ++c)
                    {
                        if (ha(b * C + c)) ++active;
                        else skipped += hm(b, c);
                    }
                out.skipped_mass = skipped / double(B);
                out.active_tiles = active;
                out.total_tiles = B * C;
                const double delta = out.skipped_mass;
                out.error_bound = 4.0 * (2.0 * std::sqrt(delta) + delta);
            }
            else
            {
                out.active_tiles = B * C;
                out.total_tiles = B * C;
            }
            return out;
        }

        namespace detail_sparse_dev
        {
            using DComplex = rvdl::accel::DComplex;
            using Exec = rvdl::accel::ExecSpace;
            using Mem = rvdl::accel::MemSpace;
            using SView = Kokkos::View<size_t *, Mem>;
            using CFlat = Kokkos::View<DComplex *, Mem>;
            using DFlat = Kokkos::View<double *, Mem>;
            using U8Flat = Kokkos::View<uint8_t *, Mem>;
            using BatchD = Kokkos::View<double **, Mem>;
            static constexpr size_t NO_SLOT = static_cast<size_t>(-1);

            struct SparseLayout
            {
                size_t B = 0, C = 1, L = 0, tb = 0, N = 0, nslots = 0;
                SView slot_b, slot_c, tile_slot;
                CFlat phi, lambda;
                DFlat dots;
                double skipped_mass = 0.0, error_bound = 0.0;
                size_t active_tiles = 0, total_tiles = 0;
            };

            inline SparseLayout build_layout(const qlm::train_detail::Complex *ket_data,
                                             size_t B, size_t N, size_t tb_in, double eps)
            {
                using qlm::train_detail::Complex;
                SparseLayout lay;
                lay.B = B; lay.N = N;
                lay.tb = std::min<size_t>(tb_in ? tb_in : 6, 12);
                while (lay.tb > 0 && (size_t(1) << lay.tb) > N) --lay.tb;
                lay.C = size_t(1) << lay.tb;
                lay.L = N / lay.C;
                const bool prune = eps > 0.0;

                std::vector<size_t> h_slot_b, h_slot_c;
                std::vector<size_t> h_tile_slot(static_cast<size_t>(B) * lay.C, NO_SLOT);
                std::vector<DComplex> h_phi;
                h_phi.reserve(static_cast<size_t>(B) * lay.C * lay.L);
                double skipped = 0.0, bound = 0.0;
                size_t active = 0, total = 0;
                for (size_t b = 0; b < B; ++b)
                {
                    std::vector<Complex> ket(ket_data + b * N, ket_data + (b + 1) * N);
                    rvdl::sparse::SparseState sp(ket, lay.tb, eps, prune);
                    skipped = std::max(skipped, sp.skipped_mass());
                    bound = std::max(bound, sp.error_bound());
                    active += sp.active_tiles();
                    total += sp.tiles();
                    for (size_t c : sp.active_tile_ids())
                    {
                        const size_t slot = h_slot_b.size();
                        h_slot_b.push_back(b);
                        h_slot_c.push_back(c);
                        h_tile_slot[b * lay.C + c] = slot;
                        const auto *amps = sp.tile_amps(c);
                        h_phi.resize(h_phi.size() + lay.L, DComplex(0.0, 0.0));
                        if (amps)
                            for (size_t k = 0; k < lay.L && k < amps->size(); ++k)
                                h_phi[(h_slot_b.size() - 1) * lay.L + k] =
                                    DComplex((*amps)[k].real(), (*amps)[k].imag());
                    }
                }
                lay.nslots = h_slot_b.size();
                lay.skipped_mass = skipped;
                lay.error_bound = bound;
                lay.active_tiles = active;
                lay.total_tiles = total;
                lay.slot_b = SView("SBA::slot_b", lay.nslots);
                lay.slot_c = SView("SBA::slot_c", lay.nslots);
                lay.tile_slot = SView("SBA::tile_slot", static_cast<size_t>(B) * lay.C);
                lay.phi = CFlat("SBA::phi", lay.nslots * lay.L);
                lay.lambda = CFlat("SBA::lambda", lay.nslots * lay.L);
                lay.dots = DFlat("SBA::dots", 1);

                auto hb = Kokkos::create_mirror_view(lay.slot_b);
                auto hc = Kokkos::create_mirror_view(lay.slot_c);
                auto ht = Kokkos::create_mirror_view(lay.tile_slot);
                auto hp = Kokkos::create_mirror_view(lay.phi);
                for (size_t s = 0; s < lay.nslots; ++s) { hb(s) = h_slot_b[s]; hc(s) = h_slot_c[s]; }
                for (size_t x = 0; x < h_tile_slot.size(); ++x) ht(x) = h_tile_slot[x];
                for (size_t x = 0; x < h_phi.size(); ++x) hp(x) = h_phi[x];
                Kokkos::deep_copy(lay.slot_b, hb);
                Kokkos::deep_copy(lay.slot_c, hc);
                Kokkos::deep_copy(lay.tile_slot, ht);
                Kokkos::deep_copy(lay.phi, hp);
                Kokkos::deep_copy(lay.lambda, DComplex(0.0, 0.0));
                return lay;
            }




            inline void apply_gate(SparseLayout &lay, int type, size_t q0, size_t q1,
                                   double angle, bool inverse, bool both)
            {
                if (lay.nslots == 0 || lay.L == 0) return;
                const double a = inverse ? -angle : angle;
                const double c = std::cos(a * 0.5), s = std::sin(a * 0.5);
                const double cr = std::cos(a), ci = std::sin(a);
                const size_t C = lay.C, L = lay.L, tb = lay.tb;
                auto sb = lay.slot_b; auto sc = lay.slot_c; auto ts = lay.tile_slot;
                auto phi = lay.phi; auto lam = lay.lambda;
                const uint64_t mask = uint64_t(1) << q0;
                const uint64_t cmask = uint64_t(1) << q0;
                const uint64_t tmask = uint64_t(1) << q1;
                Kokkos::MDRangePolicy<Exec, Kokkos::Rank<2>> pol({0, 0}, {lay.nslots, L});
                Kokkos::parallel_for("SBA::Gate", pol,
                    KOKKOS_LAMBDA(const size_t sl, const size_t k) {
                        const size_t b = sb(sl), cc = sc(sl);
                        const size_t i = (k << tb) | cc;
                        const size_t base = sl * L + k;
                        if (type == 0)
                        {
                            if (i & mask) return;
                            const size_t j = i | mask;
                            const size_t c2 = j & (C - 1);
                            const size_t k2 = j >> tb;
                            const size_t sl2 = ts(b * C + c2);
                            if (sl2 == NO_SLOT) return;
                            const size_t base2 = sl2 * L + k2;
                            const DComplex x = phi(base), y = phi(base2);
                            phi(base) = DComplex(c * x.real() - s * y.real(), c * x.imag() - s * y.imag());
                            phi(base2) = DComplex(s * x.real() + c * y.real(), s * x.imag() + c * y.imag());
                            if (both)
                            {
                                const DComplex lx = lam(base), ly = lam(base2);
                                lam(base) = DComplex(c * lx.real() - s * ly.real(), c * lx.imag() - s * ly.imag());
                                lam(base2) = DComplex(s * lx.real() + c * ly.real(), s * lx.imag() + c * ly.imag());
                            }
                        }
                        else if (type == 1)
                        {
                            if (!(i & mask)) return;
                            const DComplex x = phi(base);
                            phi(base) = DComplex(x.real() * cr - x.imag() * ci,
                                                 x.real() * ci + x.imag() * cr);
                            if (both)
                            {
                                const DComplex y = lam(base);
                                lam(base) = DComplex(y.real() * cr - y.imag() * ci,
                                                     y.real() * ci + y.imag() * cr);
                            }
                        }
                        else
                        {
                            if (!(i & cmask) || (i & tmask)) return;
                            const size_t j = i | tmask;
                            const size_t c2 = j & (C - 1);
                            const size_t k2 = j >> tb;
                            const size_t sl2 = ts(b * C + c2);
                            if (sl2 == NO_SLOT) return;
                            const size_t base2 = sl2 * L + k2;
                            const DComplex x = phi(base);
                            phi(base) = phi(base2); phi(base2) = x;
                            if (both)
                            {
                                const DComplex y = lam(base);
                                lam(base) = lam(base2); lam(base2) = y;
                            }
                        }
                    });
            }

            inline BatchD probability_one(SparseLayout &lay,
                                          const std::vector<size_t> &bits)
            {
                const size_t D = bits.size();
                BatchD pred("SBA::pred", lay.B, D);
                Kokkos::deep_copy(pred, 0.0);
                U8Flat d_bits("SBA::bits", D);
                {
                    auto hb = Kokkos::create_mirror_view(d_bits);
                    for (size_t j = 0; j < D; ++j) hb(j) = static_cast<uint8_t>(bits[j]);
                    Kokkos::deep_copy(d_bits, hb);
                }
                if (lay.nslots == 0 || lay.L == 0) return pred;
                const size_t C = lay.C, L = lay.L, tb = lay.tb, D2 = D;
                auto sb = lay.slot_b; auto sc = lay.slot_c; auto phi = lay.phi;
                auto dbs = d_bits;
                Kokkos::MDRangePolicy<Exec, Kokkos::Rank<2>> pol({0, 0}, {lay.nslots, L});
                Kokkos::parallel_for("SBA::P1", pol,
                    KOKKOS_LAMBDA(const size_t sl, const size_t k) {
                        const size_t b = sb(sl), cc = sc(sl);
                        const size_t i = (k << tb) | cc;
                        const DComplex x = phi(sl * L + k);
                        const double p = x.real() * x.real() + x.imag() * x.imag();
                        if (p == 0.0) return;
                        for (size_t j = 0; j < D2; ++j)
                            if (i & (size_t(1) << dbs(j)))
                                Kokkos::atomic_add(&pred(b, j), p);
                    });
                Kokkos::fence();
                return pred;
            }

            inline void build_lambda(SparseLayout &lay, BatchD gz,
                                     const std::vector<size_t> &bits)
            {
                if (lay.nslots == 0 || lay.L == 0) return;
                const size_t D = bits.size(), L = lay.L, tb = lay.tb;
                U8Flat d_bits("SBA::lambda_bits", D);
                {
                    auto hb = Kokkos::create_mirror_view(d_bits);
                    for (size_t j = 0; j < D; ++j) hb(j) = static_cast<uint8_t>(bits[j]);
                    Kokkos::deep_copy(d_bits, hb);
                }
                const size_t D2 = D;
                auto sb = lay.slot_b; auto sc = lay.slot_c; auto phi = lay.phi; auto lam = lay.lambda;
                auto dbs = d_bits;
                (void)tb;
                Kokkos::MDRangePolicy<Exec, Kokkos::Rank<2>> pol({0, 0}, {lay.nslots, L});
                Kokkos::parallel_for("SBA::Lambda", pol,
                    KOKKOS_LAMBDA(const size_t sl, const size_t k) {
                        const size_t b = sb(sl), cc = sc(sl);
                        const size_t i = (k << tb) | cc;
                        double coef = 0.0;
                        for (size_t j = 0; j < D2; ++j)
                            coef += gz(b, j) * (((i >> dbs(j)) & 1U) ? -1.0 : 1.0);
                        const DComplex x = phi(sl * L + k);
                        lam(sl * L + k) = DComplex(coef * x.real(), coef * x.imag());
                    });
                Kokkos::fence();
            }

            inline double gate_dot(SparseLayout &lay, int type, size_t q0,
                                   const std::vector<size_t> &)
            {
                if (lay.nslots == 0 || lay.L == 0) return 0.0;
                const size_t C = lay.C, L = lay.L, tb = lay.tb;
                const uint64_t mask = uint64_t(1) << q0;
                auto sb = lay.slot_b; auto sc = lay.slot_c; auto ts = lay.tile_slot;
                auto phi = lay.phi; auto lam = lay.lambda;
                auto total = lay.dots;
                Kokkos::deep_copy(total, 0.0);
                Kokkos::MDRangePolicy<Exec, Kokkos::Rank<2>> pol({0, 0}, {lay.nslots, L});
                Kokkos::parallel_for("SBA::Grad", pol,
                    KOKKOS_LAMBDA(const size_t sl, const size_t k) {
                        const size_t b = sb(sl), cc = sc(sl);
                        const size_t i = (k << tb) | cc;
                        const size_t base = sl * L + k;
                        double local = 0.0;
                        if (type == 1)
                        {
                            if (!(i & mask)) return;
                            const DComplex l = lam(base), p = phi(base);
                            local = (l.real() * p.imag() - l.imag() * p.real());
                        }
                        else if (type == 0)
                        {
                            if (i & mask) return;
                            const size_t j = i | mask;
                            const size_t c2 = j & (C - 1);
                            const size_t k2 = j >> tb;
                            const size_t sl2 = ts(b * C + c2);
                            if (sl2 == NO_SLOT) return;
                            const size_t base2 = sl2 * L + k2;
                            const DComplex p0 = phi(base), p1 = phi(base2);
                            const DComplex l0 = lam(base), l1 = lam(base2);
                            local = (l1.real() * p0.real() + l1.imag() * p0.imag())
                                  - (l0.real() * p1.real() + l0.imag() * p1.imag());
                        }
                        else return;
                        Kokkos::atomic_add(&total(0), local);
                    });
                Kokkos::fence();
                auto h = Kokkos::create_mirror_view_and_copy(Kokkos::HostSpace(), total);
                return h(0);
            }

            inline std::vector<double> reverse(
                SparseLayout &lay,
                const unsigned char *gate_type, const size_t *q0, const size_t *q1,
                const size_t *param, const double *scale, size_t G,
                const double *theta, size_t P,
                const qlm::AdjointRuntimeConfig &cfg)
            {
                std::vector<double> grad(P, 0.0);
                for (size_t k = G; k > 0; --k)
                {
                    const size_t g = k - 1;
                    const size_t expert = cfg.gate_expert ? cfg.gate_expert[g] : 0;
                    if (!rvdl::sparse_adjoint::gate_active(expert, cfg.moe_experts, cfg.active_expert))
                    {
                        continue;
                    }
                    const int type = gate_type[g];
                    const bool has_grad = type != 2 && param[g] < P;
                    const double dot = has_grad ? gate_dot(lay, type, q0[g], {}) : 0.0;
                    if (has_grad)
                    {
                        if (type == 1) grad[param[g]] += -2.0 * (scale[g] / double(lay.B)) * dot;
                        else if (type == 0) grad[param[g]] += (scale[g] / double(lay.B)) * dot;
                    }
                    const double ang = (type == 2 || param[g] >= P)
                        ? 0.0 : theta[param[g]] * scale[g];
                    apply_gate(lay, type, q0[g], q1[g], ang, true, true);
                }
                return grad;
            }
        }

        inline BatchAdjointResult device_adjoint_batch_sparse(
            const std::vector<detail_batch::GateOp> &gates,
            const detail_batch::Complex *ket_data, size_t B, size_t N,
            const double *theta,
            const std::vector<size_t> &readout_bits,
            const uint8_t *targets,
            size_t param_count,
            const qlm::AdjointRuntimeConfig &cfg)
        {
            using namespace detail_sparse_dev;
            using detail_batch::DComplex;
            BatchAdjointResult out;
            const size_t D = readout_bits.size(), G = gates.size();
            if (B == 0 || N == 0 || D == 0 || G == 0) return out;

            std::vector<unsigned char> gt(G);
            std::vector<size_t> q0(G), q1(G), prm(G), ge(G);
            std::vector<double> sc(G);
            for (size_t g = 0; g < G; ++g)
            {
                gt[g] = gates[g].type == detail_batch::GateOp::Type::RY ? 0
                      : gates[g].type == detail_batch::GateOp::Type::RZ ? 1 : 2;
                q0[g] = gates[g].q0; q1[g] = gates[g].q1;
                prm[g] = gates[g].param; sc[g] = gates[g].scale;
                ge[g] = gates[g].expert;
            }

            SparseLayout lay = build_layout(ket_data, B, N, cfg.sparse_tile_bits, cfg.sparse_epsilon);
            if (lay.nslots == 0)
            {
                qlm::AdjointRuntimeConfig local = cfg;
                local.gate_expert = ge.data();
                const auto host = rvdl::sparse_adjoint::batch(
                    gt.data(), q0.data(), q1.data(), prm.data(), sc.data(), G,
                    reinterpret_cast<const double *>(ket_data), B, N, theta,
                    readout_bits.data(), D, targets, param_count, local);
                out.grad = host.grad;
                out.pred = host.pred;
                out.sample_loss = host.sample_loss;
                out.sample_acc = host.sample_acc;
                out.loss = host.loss;
                out.accuracy = host.accuracy;
                out.skipped_mass = host.skipped_mass;
                out.error_bound = host.error_bound;
                out.active_tiles = host.active_tiles;
                out.total_tiles = host.total_tiles;
                return out;
            }

            for (size_t g = 0; g < G; ++g)
            {
                const size_t expert = cfg.gate_expert ? cfg.gate_expert[g] : 0;
                if (!rvdl::sparse_adjoint::gate_active(expert, cfg.moe_experts, cfg.active_expert)) continue;
                const double ang = (gt[g] == 2) ? 0.0 : theta[prm[g]] * sc[g];
                apply_gate(lay, gt[g], q0[g], q1[g], ang, false, false);
            }
            Kokkos::fence();

            auto pred_view = probability_one(lay, readout_bits);
            auto hp = Kokkos::create_mirror_view_and_copy(Kokkos::HostSpace(), pred_view);
            out.pred.assign(B * D, 0.0);
            std::vector<double> gz(B * D, 0.0);
            out.sample_loss.assign(B, 0.0);
            out.sample_acc.assign(B, 0.0);
            for (size_t b = 0; b < B; ++b)
            {
                double loss = 0.0, acc = 0.0;
                for (size_t j = 0; j < D; ++j)
                {
                    const double p = 1.0 - 2.0 * hp(b, j);
                    out.pred[b * D + j] = p;
                    const int bit = (targets[b] >> j) & 1;
                    const double target = 1.0 - 2.0 * bit;
                    const double d = p - target;
                    loss += d * d;
                    gz[b * D + j] = 2.0 * d / double(D);
                    if (((p < 0.0) ? 1 : 0) == bit) acc += 1.0;
                }
                out.sample_loss[b] = loss / double(D);
                out.sample_acc[b] = acc / double(D);
                out.loss += out.sample_loss[b];
                out.accuracy += out.sample_acc[b];
            }
            out.loss /= double(B);
            out.accuracy /= double(B);

            detail_sparse_dev::BatchD d_gz("SBA::gz", B, D);
            {
                auto hgz = Kokkos::create_mirror_view(d_gz);
                for (size_t b = 0; b < B; ++b)
                    for (size_t j = 0; j < D; ++j) hgz(b, j) = gz[b * D + j];
                Kokkos::deep_copy(d_gz, hgz);
            }
            build_lambda(lay, d_gz, readout_bits);
            qlm::AdjointRuntimeConfig local = cfg;
            local.gate_expert = ge.data();
            out.grad = reverse(lay, gt.data(), q0.data(), q1.data(), prm.data(), sc.data(), G,
                               theta, param_count, local);
            out.skipped_mass = lay.skipped_mass;
            out.error_bound = lay.error_bound;
            out.active_tiles = lay.active_tiles;
            out.total_tiles = lay.total_tiles;
            return out;
        }

        inline void device_adjoint_sparse_batch_pod(
            const unsigned char *gate_type, const size_t *q0, const size_t *q1,
            const size_t *param, const double *scale, size_t G,
            const double *ket_batch, size_t B, size_t N,
            const double *theta, const size_t *readout_bits, size_t D,
            const uint8_t *targets,
            double *grad, size_t P, double *loss_out, double *acc_out,
            const qlm::AdjointRuntimeConfig *cfg)
        {
            using namespace detail_batch;
            qlm::AdjointRuntimeConfig local;
            if (cfg) local = *cfg;
            std::vector<GateOp> gates(G);
            for (size_t g = 0; g < G; ++g)
            {
                GateOp &x = gates[g];
                x.type = gate_type[g] == 0 ? GateOp::Type::RY
                       : gate_type[g] == 1 ? GateOp::Type::RZ : GateOp::Type::CNOT;
                x.q0 = q0[g]; x.q1 = q1[g]; x.param = param[g]; x.scale = scale[g];
                x.expert = (cfg && cfg->gate_expert) ? cfg->gate_expert[g] : 0;
            }
            const std::vector<size_t> bits(readout_bits, readout_bits + D);
            const Complex *ketp = reinterpret_cast<const Complex *>(ket_batch);
            const BatchAdjointResult r = device_adjoint_batch_sparse(gates, ketp, B, N, theta, bits,
                                                                     targets, P, local);
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

        inline void device_adjoint_batch_pod(
            const unsigned char *gate_type, const size_t *q0, const size_t *q1,
            const size_t *param, const double *scale, size_t gate_count,
            const double *ket_batch, size_t B, size_t N,
            const double *theta, const size_t *readout_bits, size_t D,
            const uint8_t *targets,
            double *grad, size_t P, double *loss_out, double *acc_out,
            size_t tile_bits, double tile_epsilon,
            double *skipped_mass, double *error_bound)
        {
            using namespace detail_batch;
            std::vector<GateOp> gates(gate_count);
            for (size_t i = 0; i < gate_count; ++i)
            {
                GateOp &g = gates[i];
                g.type = gate_type[i] == 0 ? GateOp::Type::RY
                       : gate_type[i] == 1 ? GateOp::Type::RZ
                                           : GateOp::Type::CNOT;
                g.q0 = q0[i];
                g.q1 = q1[i];
                g.param = param[i];
                g.scale = scale[i];
                g.expert = 0;
            }
            const std::vector<size_t> bits(readout_bits, readout_bits + D);
            const Complex *ketp = reinterpret_cast<const Complex *>(ket_batch);
            const BatchAdjointResult r = device_adjoint_batch(
                gates, ketp, B, N, theta, bits, targets, P, tile_bits, tile_epsilon);
            std::copy(r.grad.begin(), r.grad.end(), grad);
            for (size_t b = 0; b < B; ++b)
            {
                loss_out[b] = b < r.sample_loss.size() ? r.sample_loss[b] : r.loss;
                acc_out[b] = b < r.sample_acc.size() ? r.sample_acc[b] : r.accuracy;
            }
            if (skipped_mass) *skipped_mass = r.skipped_mass;
            if (error_bound) *error_bound = r.error_bound;
        }
    }
}