#pragma once
#include "qhal/QuantumAccel.hpp"
#include "qlm/QLM.hpp"

#include <Kokkos_Core.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <complex>
#include <cstdint>
#include <vector>

namespace rvdl
{
    namespace accel
    {
        inline std::atomic<long long> &adjoint_device_us() { static std::atomic<long long> v{0}; return v; }
        inline std::atomic<size_t> &adjoint_device_calls() { static std::atomic<size_t> v{0}; return v; }

        namespace detail_adj
        {
            using Complex = std::complex<double>;
            using GateOp = qlm::train_detail::GateOp;
            using DComplex = Kokkos::complex<double>;
            using MemSpace = rvdl::accel::MemSpace;
            using CView = Kokkos::View<DComplex *, MemSpace>;
            using DView = Kokkos::View<double *, MemSpace>;
            using U64View = Kokkos::View<uint64_t *, MemSpace>;

            inline void apply_forward(StateVectorDevice &sv, const GateOp &g, const double *theta)
            {
                const double a = theta[g.param] * g.scale;
                if (g.type == GateOp::Type::RY) sv.apply_ry(g.q0, a);
                else if (g.type == GateOp::Type::RZ) sv.apply_rz(g.q0, a);
                else sv.apply_cnot(g.q0, g.q1);
            }
            [[maybe_unused]] inline void apply_inverse(StateVectorDevice &sv, const GateOp &g, const double *theta)
            {
                const double a = -theta[g.param] * g.scale;
                if (g.type == GateOp::Type::RY) sv.apply_ry(g.q0, a);
                else if (g.type == GateOp::Type::RZ) sv.apply_rz(g.q0, a);
                else sv.apply_cnot(g.q0, g.q1);
            }

            inline void apply_inverse_both(StateVectorDevice &a, StateVectorDevice &b,
                                           const GateOp &g, const double *theta)
            {
                const double angle = -theta[g.param] * g.scale;
                const uint64_t mask = uint64_t(1) << g.q0;
                const uint64_t cmask = uint64_t(1) << g.q0;
                const uint64_t tmask = uint64_t(1) << g.q1;
                CView av = a.view(), bv = b.view();
                const size_t N = a.size();
                const int type = g.type == GateOp::Type::RY ? 0
                               : g.type == GateOp::Type::RZ ? 1 : 2;
                const double c = std::cos(angle * 0.5), s = std::sin(angle * 0.5);
                const double cr = std::cos(angle), ci = std::sin(angle);

                Kokkos::parallel_for("QA::InverseBoth",
                    Kokkos::RangePolicy<rvdl::accel::ExecSpace>(0, N),
                    KOKKOS_LAMBDA(const size_t i) {
                        if (type == 0)
                        {
                            if (i & mask) return;
                            const size_t j = i | mask;
                            const DComplex a0 = av(i), a1 = av(j), b0 = bv(i), b1 = bv(j);
                            av(i) = DComplex(c * a0.real() - s * a1.real(), c * a0.imag() - s * a1.imag());
                            av(j) = DComplex(s * a0.real() + c * a1.real(), s * a0.imag() + c * a1.imag());
                            bv(i) = DComplex(c * b0.real() - s * b1.real(), c * b0.imag() - s * b1.imag());
                            bv(j) = DComplex(s * b0.real() + c * b1.real(), s * b0.imag() + c * b1.imag());
                        }
                        else if (type == 1)
                        {
                            if (!(i & mask)) return;
                            const DComplex x = av(i), y = bv(i);
                            av(i) = DComplex(x.real() * cr - x.imag() * ci,
                                             x.real() * ci + x.imag() * cr);
                            bv(i) = DComplex(y.real() * cr - y.imag() * ci,
                                             y.real() * ci + y.imag() * cr);
                        }
                        else
                        {
                            if (!(i & cmask) || (i & tmask)) return;
                            const size_t j = i | tmask;
                            const DComplex ta = av(i), tb = bv(i);
                            av(i) = av(j); av(j) = ta;
                            bv(i) = bv(j); bv(j) = tb;
                        }
                    });
            }
        }

        inline double device_adjoint(const std::vector<detail_adj::GateOp> &gates,
                                     const detail_adj::Complex *ket_data, size_t ket_size,
                                     const double *theta,
                                     const std::vector<size_t> &readout_bits,
                                     uint8_t target_token,
                                     std::vector<double> &grad,
                                     std::vector<double> &pred,
                                     std::vector<detail_adj::Complex> & ,
                                     std::vector<detail_adj::Complex> & ,
                                     double &accuracy,
                                     size_t moe_experts = 1,
                                     size_t active_expert = 0)
        {
            using namespace detail_adj;
            const size_t D = readout_bits.size();
            const size_t N = ket_size;
            if (D == 0 || N == 0)
            {
                pred.clear();
                accuracy = 0.0;
                return 0.0;
            }

            const auto t_start = std::chrono::steady_clock::now();
            StateVectorDevice phi_dev(N), lambda_dev(N);
            phi_dev.upload(ket_data, N);
            lambda_dev.resize(N);

            for (const auto &g : gates)
                if (qlm::train_detail::gate_active(g, moe_experts, active_expert))
                    apply_forward(phi_dev, g, theta);

            pred = phi_dev.batch_expectation_z(readout_bits);

            std::vector<double> gz(D, 0.0);
            double loss = 0.0;
            accuracy = 0.0;
            for (size_t j = 0; j < D; ++j)
            {
                const int bit = (target_token >> j) & 1;
                const double target = 1.0 - 2.0 * bit;
                const double d = pred[j] - target;
                loss += d * d;
                gz[j] = 2.0 * d / static_cast<double>(D);
                if (((pred[j] < 0.0) ? 1 : 0) == bit) accuracy += 1.0;
            }
            loss /= static_cast<double>(D);
            accuracy /= static_cast<double>(D);

            DView d_gz("QA::gz", D);
            U64View d_bits("QA::bits", D);
            {
                auto h_gz = Kokkos::create_mirror_view(d_gz);
                auto h_bits = Kokkos::create_mirror_view(d_bits);
                for (size_t j = 0; j < D; ++j) { h_gz(j) = gz[j]; h_bits(j) = readout_bits[j]; }
                Kokkos::deep_copy(d_gz, h_gz);
                Kokkos::deep_copy(d_bits, h_bits);
            }
            CView phi_v = phi_dev.view();
            CView lam_v = lambda_dev.view();
            Kokkos::parallel_for("QA::BuildLambda",
                Kokkos::RangePolicy<rvdl::accel::ExecSpace>(0, N),
                KOKKOS_LAMBDA(const size_t s) {
                    double c = 0.0;
                    for (size_t j = 0; j < D; ++j)
                        c += d_gz(j) * (((s >> d_bits(j)) & 1U) ? -1.0 : 1.0);
                    const DComplex p = phi_v(s);
                    lam_v(s) = DComplex(c * p.real(), c * p.imag());
                });

            const size_t G = gates.size();
            DView d_dots("QA::dots", G);
            Kokkos::deep_copy(d_dots, 0.0);
            const size_t block = 256;
            const size_t blocks = (N + block - 1) / block;

            for (size_t k = G; k > 0; --k)
            {
                const size_t idx = k - 1;
                const GateOp &g = gates[idx];
                if (!qlm::train_detail::gate_active(g, moe_experts, active_expert)) continue;
                const uint64_t mask = uint64_t(1) << g.q0;
                const int type = (g.type == GateOp::Type::RZ) ? 1
                               : (g.type == GateOp::Type::RY) ? 2 : 0;
                if (type != 0)
                {
                    Kokkos::parallel_for("QA::GradDot",
                        Kokkos::RangePolicy<rvdl::accel::ExecSpace>(0, blocks),
                        KOKKOS_LAMBDA(const size_t b) {
                            const size_t begin = b * block;
                            const size_t end = (begin + block < N) ? begin + block : N;
                            double local = 0.0;
                            for (size_t s = begin; s < end; ++s)
                            {
                                if (type == 1)
                                {
                                    if (!(s & mask)) continue;
                                    const DComplex l = lam_v(s), p = phi_v(s);
                                    local += l.real() * p.imag() - l.imag() * p.real();
                                }
                                else
                                {
                                    if (s & mask) continue;
                                    const size_t j = s | mask;
                                    const DComplex l1 = lam_v(j), p0 = phi_v(s);
                                    const DComplex l0 = lam_v(s), p1 = phi_v(j);
                                    local += (l1.real() * p0.real() + l1.imag() * p0.imag())
                                           - (l0.real() * p1.real() + l0.imag() * p1.imag());
                                }
                            }
                            if (local != 0.0) Kokkos::atomic_add(&d_dots(idx), local);
                        });
                }
                apply_inverse_both(phi_dev, lambda_dev, g, theta);
            }
            Kokkos::fence();
            auto h_dots = Kokkos::create_mirror_view_and_copy(Kokkos::HostSpace(), d_dots);
            for (size_t idx = 0; idx < G; ++idx)
            {
                const GateOp &g = gates[idx];
                if (g.type == GateOp::Type::RZ) grad[g.param] += -2.0 * g.scale * h_dots(idx);
                else if (g.type == GateOp::Type::RY) grad[g.param] += g.scale * h_dots(idx);
            }
            adjoint_device_calls().fetch_add(1, std::memory_order_relaxed);
            adjoint_device_us().fetch_add(
                static_cast<long long>(std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - t_start).count()),
                std::memory_order_relaxed);
            return loss;
        }

        inline double device_adjoint_pod(
            const unsigned char *gate_type, const size_t *q0, const size_t *q1,
            const size_t *param, const double *scale, size_t gate_count,
            const double *ket, size_t dim, const double *theta,
            const size_t *readout_bits, size_t readout_count, uint8_t target_token,
            double *grad, size_t grad_count, double *pred, double *accuracy,
            size_t moe_experts, size_t active_expert, const size_t *gate_expert)
        {
            using namespace detail_adj;
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
                g.expert = gate_expert ? gate_expert[i] : 0;
            }
            const std::vector<size_t> bits(readout_bits, readout_bits + readout_count);
            std::vector<double> gradv(grad, grad + grad_count);
            std::vector<double> predv(readout_count, 0.0);
            std::vector<Complex> phi, lambda;
            double acc = *accuracy;
            const Complex *ketp = reinterpret_cast<const Complex *>(ket);
            const double loss = device_adjoint(gates, ketp, dim, theta, bits, target_token,
                                               gradv, predv, phi, lambda, acc,
                                               moe_experts, active_expert);
            std::copy(gradv.begin(), gradv.end(), grad);
            std::copy(predv.begin(), predv.begin() + readout_count, pred);
            *accuracy = acc;
            return loss;
        }
    }
}