#pragma once
#include "RetrocausalCapacity.hpp"
#include "KaluzaKlein.hpp"
#include <vector>
#include <cmath>
#include <algorithm>
#include <stdexcept>

namespace rvdl::spacetime
{

    struct KaluzaKleinND
    {
        std::vector<double> radii;
        std::vector<double> wilson_lines;

        KaluzaKleinND() = default;
        KaluzaKleinND(std::vector<double> R, std::vector<double> theta)
            : radii(std::move(R)), wilson_lines(std::move(theta)) {}

        static KaluzaKleinND from_kk(const KaluzaKlein &kk)
        {
            return KaluzaKleinND({kk.R}, {kk.theta});
        }

        size_t num_extra() const { return radii.size(); }

        double kk_momentum(size_t i, int n) const
        {
            return static_cast<double>(n) / radii[i] + wilson_lines[i];
        }

        double mode_mass_sq(const std::vector<int> &n, double m0 = 0.0) const
        {
            double p2 = 0.0;
            for (size_t i = 0; i < num_extra(); ++i)
            {
                const double pi = kk_momentum(i, (i < n.size()) ? n[i] : 0);
                p2 += pi * pi;
            }
            return m0 * m0 + p2;
        }

        double mode_mass(const std::vector<int> &n, double m0 = 0.0) const
        {
            return std::sqrt(std::max(mode_mass_sq(n, m0), 0.0));
        }

        double tachyon_mass_sq(const std::vector<int> &n, double mu2) const
        {
            return -mu2 + mode_mass_sq(n, 0.0);
        }

        double asymmetry() const
        {
            double s = 0.0;
            for (double th : wilson_lines)
                s += std::abs(th);
            return s;
        }

        double mode_asymmetry() const
        {
            double s = 0.0;
            for (size_t i = 0; i < num_extra(); ++i)
            {
                const double mp = std::abs(kk_momentum(i, 1));
                const double mm = std::abs(kk_momentum(i, -1));
                s += std::abs(mp - mm);
            }
            return s;
        }
    };

    struct TachyonCTCNoise
    {
        double dephasing = 0.0;
        double depolarizing = 0.0;
        double damping = 0.0;
    };

    inline TachyonCTCNoise tachyon_ctc_noise(const KaluzaKleinND &kk, double mu2, double lambda)
    {
        TachyonCTCNoise noise;
        const double asym = kk.asymmetry();
        noise.dephasing = asym / (1.0 + asym);
        const double n = static_cast<double>(kk.num_extra());
        noise.depolarizing = n / (n + 1.0);
        const double mu2c = std::max(mu2, 0.0);
        const double lamc = std::max(lambda, 1e-12);
        noise.damping = mu2c / (mu2c + lamc);
        return noise;
    }

    inline QuantumChannel compose_channels(const QuantumChannel &N1, const QuantumChannel &N2)
    {
        std::vector<numqk::CTensor> Ks;
        Ks.reserve(N1.kraus.size() * N2.kraus.size());
        for (const auto &K : N1.kraus)
            for (const auto &L : N2.kraus)
                Ks.push_back(L.matmul(K));
        QuantumChannel c = QuantumChannel::from_kraus(std::move(Ks), N1.dim_in, N2.dim_out);
        return c;
    }

    inline QuantumChannel tachyon_ctc_channel(const KaluzaKleinND &kk, double mu2, double lambda)
    {
        const TachyonCTCNoise noise = tachyon_ctc_noise(kk, mu2, lambda);

        QuantumChannel N = QuantumChannel::amplitude_damping(noise.damping);
        if (noise.dephasing > 0.0)
            N = compose_channels(N, QuantumChannel::dephasing(noise.dephasing));
        if (noise.depolarizing > 0.0)
            N = compose_channels(N, QuantumChannel::depolarizing(noise.depolarizing));
        N.kind = -1;
        return N;
    }

    struct MultiTopologyRetrocausal
    {
        KaluzaKleinND kk;
        double mu2 = 1.0;
        double lambda = 1.0;

        size_t coord_dim() const { return kk.num_extra(); }

        TachyonCTCNoise noise() const { return tachyon_ctc_noise(kk, mu2, lambda); }

        QuantumChannel channel() const { return tachyon_ctc_channel(kk, mu2, lambda); }

        RetrocausalCapacity capacity(double eps = 1e-3) const
        {
            return retrocausal_capacity(channel(), eps);
        }

        std::vector<double> per_dimension_capacity(double eps = 1e-3) const
        {
            std::vector<double> caps;
            caps.reserve(kk.num_extra());
            for (size_t i = 0; i < kk.num_extra(); ++i)
            {
                KaluzaKleinND single({kk.radii[i]}, {kk.wilson_lines[i]});
                caps.push_back(
                    retrocausal_capacity(tachyon_ctc_channel(single, mu2, lambda), eps).q_asymptotic);
            }
            return caps;
        }

        std::vector<double> capacity_vs_dimension(double eps = 1e-3) const
        {
            std::vector<double> curve;
            curve.reserve(kk.num_extra());
            for (size_t m = 1; m <= kk.num_extra(); ++m)
            {
                KaluzaKleinND sub(std::vector<double>(kk.radii.begin(), kk.radii.begin() + static_cast<long>(m)),
                                  std::vector<double>(kk.wilson_lines.begin(), kk.wilson_lines.begin() + static_cast<long>(m)));
                curve.push_back(
                    retrocausal_capacity(tachyon_ctc_channel(sub, mu2, lambda), eps).q_asymptotic);
            }
            return curve;
        }
    };
}