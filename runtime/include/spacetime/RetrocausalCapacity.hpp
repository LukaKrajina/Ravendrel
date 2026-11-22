#pragma once
#include "../numqk/ComplexTensor.hpp"
#include "../numqk/QuantumGeometry.hpp"
#include <vector>
#include <complex>
#include <cmath>
#include <algorithm>
#include <stdexcept>
#include <functional>

namespace rvdl::spacetime
{

    constexpr double RC_PI = 3.14159265358979323846;

    enum class RetrocausalChannelKind
    {
        Depolarizing = 0,
        Dephasing = 1,
        BitFlip = 2,
        AmplitudeDamping = 3,
        UnitaryHadamard = 4,
        BitPhaseFlip = 5,
    };

    struct QuantumChannel
    {
        size_t dim_in = 2;
        size_t dim_out = 2;
        std::vector<numqk::CTensor> kraus;
        int kind = -1;
        double param = 0.0;

        static QuantumChannel from_kraus(std::vector<numqk::CTensor> Ks,
                                         size_t din, size_t dout)
        {
            QuantumChannel c;
            c.dim_in = din;
            c.dim_out = dout;
            c.kraus = std::move(Ks);
            c.kind = -1;
            return c;
        }

        static QuantumChannel unitary(const numqk::CTensor &U)
        {
            return from_kraus({U}, 2, 2);
        }

        static QuantumChannel depolarizing(double p)
        {
            p = clamp01(p);
            std::vector<numqk::CTensor> Ks;
            Ks.push_back(scale(pauli_I(), std::sqrt(1.0 - 0.75 * p)));
            Ks.push_back(scale(numqk::pauli_x(), std::sqrt(p / 4.0)));
            Ks.push_back(scale(numqk::pauli_y(), std::sqrt(p / 4.0)));
            Ks.push_back(scale(numqk::pauli_z(), std::sqrt(p / 4.0)));
            QuantumChannel c = from_kraus(std::move(Ks), 2, 2);
            c.kind = static_cast<int>(RetrocausalChannelKind::Depolarizing);
            c.param = p;
            return c;
        }

        static QuantumChannel dephasing(double p)
        {
            p = clamp01(p);
            std::vector<numqk::CTensor> Ks;
            Ks.push_back(scale(pauli_I(), std::sqrt(1.0 - p)));
            Ks.push_back(scale(numqk::pauli_z(), std::sqrt(p)));
            QuantumChannel c = from_kraus(std::move(Ks), 2, 2);
            c.kind = static_cast<int>(RetrocausalChannelKind::Dephasing);
            c.param = p;
            return c;
        }

        static QuantumChannel bit_flip(double p)
        {
            p = clamp01(p);
            std::vector<numqk::CTensor> Ks;
            Ks.push_back(scale(pauli_I(), std::sqrt(1.0 - p)));
            Ks.push_back(scale(numqk::pauli_x(), std::sqrt(p)));
            QuantumChannel c = from_kraus(std::move(Ks), 2, 2);
            c.kind = static_cast<int>(RetrocausalChannelKind::BitFlip);
            c.param = p;
            return c;
        }

        static QuantumChannel amplitude_damping(double gamma)
        {
            gamma = clamp01(gamma);
            numqk::CTensor K0({2, 2}, false), K1({2, 2}, false);
            K0.data()[0] = numqk::Complex(1.0, 0.0);
            K0.data()[3] = numqk::Complex(std::sqrt(1.0 - gamma), 0.0);
            K1.data()[1] = numqk::Complex(std::sqrt(gamma), 0.0);
            QuantumChannel c = from_kraus({K0, K1}, 2, 2);
            c.kind = static_cast<int>(RetrocausalChannelKind::AmplitudeDamping);
            c.param = gamma;
            return c;
        }

        static QuantumChannel bit_phase_flip(double p)
        {
            p = clamp01(p);
            std::vector<numqk::CTensor> Ks;
            Ks.push_back(scale(pauli_I(), std::sqrt(1.0 - p)));
            Ks.push_back(scale(numqk::pauli_y(), std::sqrt(p)));
            QuantumChannel c = from_kraus(std::move(Ks), 2, 2);
            c.kind = static_cast<int>(RetrocausalChannelKind::BitPhaseFlip);
            c.param = p;
            return c;
        }

        static QuantumChannel unitary_hadamard()
        {
            numqk::CTensor H({2, 2}, false);
            const double inv = 1.0 / std::sqrt(2.0);
            H.data()[0] = numqk::Complex(inv, 0.0);
            H.data()[1] = numqk::Complex(inv, 0.0);
            H.data()[2] = numqk::Complex(inv, 0.0);
            H.data()[3] = numqk::Complex(-inv, 0.0);
            QuantumChannel c = unitary(H);
            c.kind = static_cast<int>(RetrocausalChannelKind::UnitaryHadamard);
            return c;
        }

        static QuantumChannel make(int kind, double p)
        {
            switch (static_cast<RetrocausalChannelKind>(kind))
            {
            case RetrocausalChannelKind::Depolarizing:
                return depolarizing(p);
            case RetrocausalChannelKind::Dephasing:
                return dephasing(p);
            case RetrocausalChannelKind::BitFlip:
                return bit_flip(p);
            case RetrocausalChannelKind::AmplitudeDamping:
                return amplitude_damping(p);
            case RetrocausalChannelKind::BitPhaseFlip:
                return bit_phase_flip(p);
            case RetrocausalChannelKind::UnitaryHadamard:
            default:
                return unitary_hadamard();
            }
        }

    private:
        static double clamp01(double x)
        {
            return std::max(0.0, std::min(1.0, x));
        }

        static numqk::CTensor pauli_I()
        {
            numqk::CTensor I({2, 2}, false);
            I.data()[0] = numqk::Complex(1.0, 0.0);
            I.data()[3] = numqk::Complex(1.0, 0.0);
            return I;
        }

        static numqk::CTensor scale(const numqk::CTensor &M, double s)
        {
            numqk::CTensor out(M.get_shape(), false);
            for (size_t i = 0; i < M.size(); ++i)
                out.data()[i] = M.data()[i] * s;
            return out;
        }
    };

    inline numqk::CTensor choi_matrix(const QuantumChannel &N)
    {
        const size_t D = N.dim_out * N.dim_in;
        numqk::CTensor J({D, D}, false);
        for (const auto &K : N.kraus)
        {
            numqk::CTensor v({D}, false);
            for (size_t i = 0; i < D; ++i)
                v.data()[i] = K.data()[i];
            numqk::CTensor vv = numqk::outer(v, v);
            for (size_t idx = 0; idx < D * D; ++idx)
                J.data()[idx] += vv.data()[idx];
        }
        return J;
    }

    struct RetrocausalInfo
    {
        double imax = 0.0;
        double idoe = 0.0;
        double idoe_reg = 0.0;
        std::vector<double> spectrum;
        double purity = 0.0;
        double flatness = 0.0;
    };

    inline RetrocausalInfo retrocausal_info(const QuantumChannel &N)
    {
        const numqk::CTensor J = choi_matrix(N);
        const std::vector<double> evals = numqk::hermitian_eigenvalues(J);
        const double lam_max = evals.empty() ? 0.0 : evals.back();
        double lam_min_pos = lam_max;
        for (double l : evals)
            if (l > 1e-12 && l < lam_min_pos)
                lam_min_pos = l;
        if (lam_min_pos < 1e-12)
            lam_min_pos = 1e-12;

        RetrocausalInfo info;
        info.imax = std::log2(std::max(lam_max, 1e-12));
        info.idoe = std::log2(static_cast<double>(N.dim_in)) - std::log2(lam_min_pos);

        info.spectrum.assign(evals.rbegin(), evals.rend());

        const double trJ = numqk::trace(J).real();
        const double trJ2 = numqk::purity(J);
        info.purity = (std::abs(trJ) > 1e-12) ? trJ2 / (trJ * trJ) : 0.0;
        info.flatness = (lam_max > 1e-12) ? lam_min_pos / lam_max : 0.0;
        return info;
    }

    inline QuantumChannel tensor_product(const QuantumChannel &A, const QuantumChannel &B)
    {
        std::vector<numqk::CTensor> Ks;
        Ks.reserve(A.kraus.size() * B.kraus.size());
        for (const auto &Ka : A.kraus)
            for (const auto &Kb : B.kraus)
                Ks.push_back(numqk::kron(Ka, Kb));
        QuantumChannel c = QuantumChannel::from_kraus(std::move(Ks),
                                                      A.dim_in * B.dim_in,
                                                      A.dim_out * B.dim_out);
        return c;
    }

    inline QuantumChannel tensor_power(const QuantumChannel &N, size_t n)
    {
        QuantumChannel acc = N;
        for (size_t i = 1; i < n; ++i)
            acc = tensor_product(acc, N);
        return acc;
    }

    inline double idoe_regularized(const QuantumChannel &N, size_t max_n = 4)
    {
        const double idoe1 = retrocausal_info(N).idoe;
        const size_t base = N.dim_in * N.dim_out;

        double best = idoe1;
        for (size_t n = 2; n <= max_n; ++n)
        {
            size_t dim = 1;
            for (size_t i = 0; i < n; ++i)
                dim *= base;
            if (dim > 4096)
                break;

            const QuantumChannel Nn = tensor_power(N, n);
            const double idoe_n = retrocausal_info(Nn).idoe / static_cast<double>(n);
            best = std::max(best, idoe_n);
            if (std::abs(idoe_n - idoe1) < 1e-6)
                break;
        }
        return best;
    }

    inline double binary_entropy(double x)
    {
        if (x <= 0.0 || x >= 1.0)
            return 0.0;
        return -x * std::log2(x) - (1.0 - x) * std::log2(1.0 - x);
    }

    inline numqk::CTensor bloch_pure(double theta, double phi)
    {
        numqk::CTensor psi({2}, false);
        psi.data()[0] = numqk::Complex(std::cos(theta / 2.0), 0.0);
        psi.data()[1] = numqk::Complex(std::cos(phi) * std::sin(theta / 2.0),
                                       std::sin(phi) * std::sin(theta / 2.0));
        return psi;
    }

    inline double forward_classical_capacity(const QuantumChannel &N, int nt = 32, int np = 64)
    {
        switch (static_cast<RetrocausalChannelKind>(N.kind))
        {
        case RetrocausalChannelKind::Depolarizing:
            return 1.0 - binary_entropy(N.param / 2.0);
        case RetrocausalChannelKind::Dephasing:
        case RetrocausalChannelKind::BitFlip:
        case RetrocausalChannelKind::BitPhaseFlip:
            return 1.0 - binary_entropy(N.param);
        case RetrocausalChannelKind::AmplitudeDamping:
        {
            const double gamma = N.param;
            const double hg = binary_entropy(gamma);
            double best = 0.0;
            const int steps = 512;
            for (int i = 0; i <= steps; ++i)
            {
                const double p = static_cast<double>(i) / static_cast<double>(steps);
                const double r = (1.0 - p) * (1.0 - gamma);
                const double chi = binary_entropy(r) - (1.0 - p) * hg;
                best = std::max(best, chi);
            }
            return best;
        }
        case RetrocausalChannelKind::UnitaryHadamard:
            return 1.0;
        default:
            break;
        }

        if (N.dim_in != 2 || N.dim_out != 2)
            return std::log2(static_cast<double>(N.dim_in));

        double best = 0.0;
        for (int it = 0; it <= nt; ++it)
        {
            const double theta = RC_PI * it / nt;
            for (int ip = 0; ip < np; ++ip)
            {
                const double phi = 2.0 * RC_PI * ip / np;
                const numqk::CTensor psi = bloch_pure(theta, phi);
                const numqk::CTensor psi_orth = bloch_pure(theta + RC_PI, phi);
                const numqk::CTensor rho0 = numqk::pure_state_density(psi);
                const numqk::CTensor rho1 = numqk::pure_state_density(psi_orth);
                const numqk::CTensor out0 = numqk::apply_kraus(rho0, N.kraus);
                const numqk::CTensor out1 = numqk::apply_kraus(rho1, N.kraus);

                numqk::CTensor avg({2, 2}, false);
                for (int i = 0; i < 4; ++i)
                    avg.data()[i] = 0.5 * (out0.data()[i] + out1.data()[i]);

                const double chi = numqk::von_neumann_entropy(avg) -
                                   0.5 * (numqk::von_neumann_entropy(out0) +
                                          numqk::von_neumann_entropy(out1));
                best = std::max(best, chi);
            }
        }
        return best;
    }

    struct RetrocausalCapacity
    {
        double q_asymptotic = 0.0;
        double c_asymptotic = 0.0;
        double q_one_shot = 0.0;
        double c_one_shot = 0.0;
        double gain = 0.0;
    };

    inline RetrocausalCapacity retrocausal_capacity(const QuantumChannel &N, double eps = 1e-3)
    {
        const RetrocausalInfo info = retrocausal_info(N);
        const double idoe_reg = idoe_regularized(N);
        const double sum = info.imax + idoe_reg;

        RetrocausalCapacity cap;
        cap.q_asymptotic = 0.5 * sum;
        cap.c_asymptotic = sum;

        const double e = (eps > 0.0 && eps < 1.0) ? eps / (1.0 - eps) : 0.0;
        const double two_sum = std::pow(2.0, info.imax + info.idoe);
        cap.q_one_shot = std::log2(std::max(1.0, std::floor(std::sqrt(e * two_sum + 1.0))));
        cap.c_one_shot = std::log2(std::max(1.0, std::floor(e * two_sum + 1.0)));

        const double fwd = forward_classical_capacity(N);
        cap.gain = (fwd > 1e-12) ? cap.q_asymptotic / fwd : 0.0;
        return cap;
    }

    inline double q_retrocausal_capacity(const QuantumChannel &N, double q)
    {
        q = std::max(-1.0, std::min(1.0, q));
        const double c_retro = retrocausal_capacity(N).c_asymptotic;
        const double c_forward = forward_classical_capacity(N);
        const double wp = 0.5 * (1.0 + q);
        const double wf = 0.5 * (1.0 - q);
        const double eff_dim = wp * std::pow(2.0, c_retro) + wf * std::pow(2.0, c_forward);
        return std::log2(std::max(eff_dim, 1.0));
    }

    struct ProtocolResult
    {
        double fidelity = 0.0;
        double postselection_prob = 0.0;
        double amplification = 0.0;
        double confidence = 0.0;
    };

    inline ProtocolResult amplified_teleportation(const QuantumChannel &N, size_t msg_dim)
    {
        const size_t d = (msg_dim > 0) ? msg_dim : N.dim_in;

        numqk::CTensor phi({d * d}, false);
        for (size_t i = 0; i < d; ++i)
            phi.data()[i * d + i] = numqk::Complex(1.0 / std::sqrt(static_cast<double>(d)), 0.0);
        numqk::CTensor Phi = numqk::pure_state_density(phi);

        const numqk::CTensor J = choi_matrix(N);
        const double trJ = numqk::trace(J).real();
        numqk::CTensor rho_choi({J.get_shape()[0], J.get_shape()[0]}, false);
        for (size_t i = 0; i < J.size(); ++i)
            rho_choi.data()[i] = J.data()[i] / trJ;

        const double p_succ = numqk::trace(Phi.matmul(rho_choi)).real();

        const double p_noctc = 1.0 / static_cast<double>(d * d);
        const double amplification = p_succ / std::max(p_noctc, 1e-30);

        const double fidelity = std::max(0.0, std::min(1.0, p_succ));

        const double confidence = amplification / (1.0 + amplification);

        ProtocolResult r;
        r.fidelity = fidelity;
        r.postselection_prob = p_succ;
        r.amplification = amplification;
        r.confidence = confidence;
        return r;
    }

    struct RetrocausalFlow
    {
        double dq_dp = 0.0;
        double dc_dp = 0.0;
        double q_flow = 0.0;
        double c_flow = 0.0;
    };

    inline RetrocausalFlow retrocausal_flow_perturbed(
        const std::function<QuantumChannel(double)> &family,
        double p, double dp_dt, double h = 1e-4)
    {
        const RetrocausalCapacity cap_p = retrocausal_capacity(family(p + h));
        const RetrocausalCapacity cap_m = retrocausal_capacity(family(p - h));
        const double inv2h = 1.0 / (2.0 * h);

        RetrocausalFlow flow;
        flow.dq_dp = (cap_p.q_asymptotic - cap_m.q_asymptotic) * inv2h;
        flow.dc_dp = (cap_p.c_asymptotic - cap_m.c_asymptotic) * inv2h;
        flow.q_flow = flow.dq_dp * dp_dt;
        flow.c_flow = flow.dc_dp * dp_dt;
        return flow;
    }

    inline RetrocausalFlow retrocausal_flow(const QuantumChannel &N, double dp_dt, double h = 1e-4)
    {
        if (N.kind < 0)
            return RetrocausalFlow{};
        return retrocausal_flow_perturbed(
            [kind = N.kind](double p) { return QuantumChannel::make(kind, p); },
            N.param, dp_dt, h);
    }

}