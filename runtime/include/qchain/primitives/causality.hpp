#pragma once
#include <array>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <vector>
#include "../../spacetime/Foliation.hpp"
#include "../../spacetime/InitialConditions.hpp"
#include "../../spacetime/KaluzaKlein.hpp"
#include "../common.hpp"
#include "hash.hpp"

namespace qchain::causality
{

    using qchain::Byte;
    using qchain::Bytes;
    using qchain::Hash256;
    using qchain::crypto::sha256;

    struct CausalityConfig
    {
        double mu2 = 1.0, lambda = 1.0;
        int dim = 1;
        std::array<int, 3> n = {64, 1, 1};
        std::array<double, 3> length = {20.0, 1.0, 1.0};
        double dt = 0.01;
        double noise_amplitude = 1e-4;
        uint64_t seed = 42;
        double drift_tolerance = 1e-6;
        double kk_asymmetry_tolerance = 1e-9;
        double wilson_line = 0.0;
    };

    class CausalityGuard
    {
    private:
        CausalityConfig cfg_;
        rvdl::spacetime::Foliation fol_;
        double baseline_energy_ = 0.0;
        size_t steps_ = 0;

        static rvdl::spacetime::TachyonConfig to_tachyon(const CausalityConfig &c)
        {
            rvdl::spacetime::TachyonConfig tc;
            tc.mu2 = c.mu2;
            tc.lambda = c.lambda;
            tc.dim = c.dim;
            tc.n = c.n;
            tc.length = c.length;
            tc.dt = c.dt;
            tc.integrator = rvdl::spacetime::IntegratorKind::ForestRuth4;
            tc.diff_order = 4;
            tc.seed = c.seed;
            tc.noise_amplitude = c.noise_amplitude;
            return tc;
        }
        
    public:
        explicit CausalityGuard(const CausalityConfig &c = CausalityConfig{})
            : cfg_(c), fol_(to_tachyon(c))
        {
            rvdl::spacetime::init_gaussian_noise(fol_.field(), cfg_.noise_amplitude);
            baseline_energy_ = fol_.energy();
        }

        void step()
        {
            fol_.step();
            ++steps_;
        }

        double energy() const { return fol_.energy(); }
        double drift() const { return std::abs(energy() - baseline_energy_); }
        size_t steps() const { return steps_; }

        bool verify_energy() const { return drift() < cfg_.drift_tolerance; }

        bool verify_kaluza_klein() const
        {
            rvdl::spacetime::KaluzaKlein kk(1.0, cfg_.wilson_line);
            auto spec = kk.mass_spectrum(5);
            double asym = 0.0;
            for (int i = 1; i <= 5; ++i)
                asym += std::abs(spec[5 + i] - spec[5 - i]);
            return asym < cfg_.kk_asymmetry_tolerance;
        }

        bool verify() const { return verify_energy() && verify_kaluza_klein(); }

        Hash256 fingerprint() const
        {
            const auto &phi = fol_.field().phi;
            Bytes bytes(phi.size() * sizeof(double));
            std::memcpy(bytes.data(), phi.data(), bytes.size());
            return sha256(bytes);
        }
    };
}