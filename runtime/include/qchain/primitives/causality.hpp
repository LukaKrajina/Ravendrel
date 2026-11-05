#pragma once
//
// 快子场因果哨兵（防范超时空手段）
//
// 物理依据：快子（tachyon）是超光速粒子，一旦进入因果链会破坏因果律。若有人用
// 「超时空手段」篡改区块链，其物理表现必然落在：
//   1) 快子场能量不再守恒（能量凭空注入 / 泄露，违背 Noether 定理）；
//   2) 额外维的 KK 质量谱失去 ±n 对称（来自额外维 / 高维的扰动）。
//
// 因此把 spacetime 的快子场（TachyonField）作为链上的「因果哨兵」：
//   • 每个区块出块时，快子场推进一片（Foliation::step()），使「因果链」与「区块链」同频演化；
//   • EnergyMonitor 校验离散哈密顿量守恒（辛积分保证无伪漂移）；
//   • KaluzaKlein 校验额外维质量谱对称性；
//   • phi 场哈希作为因果指纹（causal_root）写入区块头，使链具备时空一致性。
//
#include <array>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <vector>
#include "../../spacetime/Foliation.hpp"       // 已含 EnergyMonitor
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
        double mu2 = 1.0, lambda = 1.0;   // 快子势 V = -½μ²φ² + ¼λφ⁴
        int dim = 1;
        std::array<int, 3> n = {64, 1, 1};
        std::array<double, 3> length = {20.0, 1.0, 1.0};
        double dt = 0.01;
        double noise_amplitude = 1e-4;
        uint64_t seed = 42;
        double drift_tolerance = 1e-6;       // 能量相对漂移容忍度
        double kk_asymmetry_tolerance = 1e-9; // KK 质量谱对称容忍度
        double wilson_line = 0.0;             // 额外维磁通（基准为 0；被高维扰动时非零）
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

        // 推进一步快子场（出块时调用，因果链与区块链同频演化）。
        void step()
        {
            fol_.step();
            ++steps_;
        }

        double energy() const { return fol_.energy(); }
        double drift() const { return std::abs(energy() - baseline_energy_); }
        size_t steps() const { return steps_; }

        // 因果律校验: 快子场能量守恒（Noether）。
        bool verify_energy() const { return drift() < cfg_.drift_tolerance; }

        // 因果律校验：额外维 KK 质量谱对称（检测高维扰动）。
        // 用实际 Wilson line（额外维磁通）计算谱：θ ≠ 0 时 ±n 模式分裂，
        // 分裂幅度超过容忍度即判定额外维被异常扰动。
        bool verify_kaluza_klein() const
        {
            rvdl::spacetime::KaluzaKlein kk(1.0, cfg_.wilson_line);
            auto spec = kk.mass_spectrum(5);
            double asym = 0.0;
            for (int i = 1; i <= 5; ++i)
                asym += std::abs(spec[5 + i] - spec[5 - i]); // ±n 模式质量应对称
            return asym < cfg_.kk_asymmetry_tolerance;
        }

        // 综合因果律校验：两项都通过才能判定无超时空干预。
        bool verify() const { return verify_energy() && verify_kaluza_klein(); }

        // 因果指纹：phi 场哈希（纳入区块头 causal_root）。
        Hash256 fingerprint() const
        {
            const auto &phi = fol_.field().phi;
            Bytes bytes(phi.size() * sizeof(double));
            std::memcpy(bytes.data(), phi.data(), bytes.size());
            return sha256(bytes);
        }
    };
}