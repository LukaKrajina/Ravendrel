#pragma once
//
// RetrocausalCTC.hpp —— 快子 KK 双空间下的嘈杂封闭类时曲线（noisy P-CTC）通信
//
// 把 RetrocausalCapacity.hpp 里抽象的噪声参数 p 改为由快子场在 KK 额外维
// （DD 双维度）中的传播物理地产生，并把逆因果容量推广到 n 个 DD 维度与多拓扑。
//
// 物理图景（结合现有 spacetime 基础设施）：
//   • 快子（TachyonField：m²_eff = −μ² < 0，超光速）沿类时闭合路径传播即形成
//     封闭类时曲线（CTC）；其传播噪声即 P-CTC 的噪声来源。
//   • KK 空间（KaluzaKlein）：额外维紧化为 S¹，KK 动量量子化 n/R；Wilson line θ
//     破坏 ±n 模式质量对称（现有 causality.hpp 用它校验「额外维异常扰动」）。
//   • O(D,D) 双空间（GeneralizedMetric）：「DD 维度」= 双维度（doubled dimension），
//     每个空间维度配一个对偶维度；n 个额外维 = n 个 DD 维度。
//   • @layer 标签的多维 coord：每个 coord 分量对应一个额外维（DD 维度），
//     coord 维数 = 额外维数 n，构成「多拓扑下的 n 个 DD 维度」。
//
// 噪声映射（几何 → 噪声，把抽象 p 替换为 KK 几何 + 快子势的物理量）：
//   • 退相 dephasing       p = asym / (1 + asym)，asym = Σ_i |θ_i|（Wilson line 破坏 ±n 对称）
//   • 去极化 depolarizing  p = n / (n + 1)（额外维的「维度扩散」）
//   • 振幅阻尼 damping     γ = μ² / (μ² + λ)（快子势 V = −½μ²φ² + ¼λφ⁴ 的真空不稳定性）
//
// 纯 numqk + spacetime 依赖（跨层），后端无关。
//
#include "RetrocausalCapacity.hpp"
#include "KaluzaKlein.hpp"
#include <vector>
#include <cmath>
#include <algorithm>
#include <stdexcept>

namespace rvdl::spacetime
{

    // ─────────────────────────────────────────────────────────────
    // n 个额外维（DD 双维度）的 KK 空间
    //   每个额外维是半径 R_i 的紧致圆 S¹，Wilson line θ_i 破坏该维 ±n 模式对称。
    //   KK 动量 p_i(n) = n/R_i + θ_i，质量 m² = m0² + Σ_i p_i²。
    // ─────────────────────────────────────────────────────────────
    struct KaluzaKleinND
    {
        std::vector<double> radii;        // 每个额外维的紧化半径 R_i
        std::vector<double> wilson_lines; // 每个额外维的 Wilson line θ_i

        KaluzaKleinND() = default;
        KaluzaKleinND(std::vector<double> R, std::vector<double> theta)
            : radii(std::move(R)), wilson_lines(std::move(theta)) {}

        // 从现有单额外维 KaluzaKlein 退化构造
        static KaluzaKleinND from_kk(const KaluzaKlein &kk)
        {
            return KaluzaKleinND({kk.R}, {kk.theta});
        }

        size_t num_extra() const { return radii.size(); }

        // 第 i 个额外维的 KK 动量 p_i(n) = n/R_i + θ_i
        double kk_momentum(size_t i, int n) const
        {
            return static_cast<double>(n) / radii[i] + wilson_lines[i];
        }

        // 模式质量平方 m² = m0² + Σ_i p_i(n_i)²（n 为各额外维的 KK 量子数）
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

        // 快子质量平方：m²_eff = −μ² + Σ_i p_i²（μ² > 0 时最低模式 m²_eff < 0，即快子）
        double tachyon_mass_sq(const std::vector<int> &n, double mu2) const
        {
            return -mu2 + mode_mass_sq(n, 0.0);
        }

        // Wilson line 不对称性（退相噪声的几何来源）：Σ_i |θ_i|
        double asymmetry() const
        {
            double s = 0.0;
            for (double th : wilson_lines)
                s += std::abs(th);
            return s;
        }

        // ±n 模式质量不对称度（含半径，供因果哨兵/噪声强度使用）：
        //   对每个额外维，比较 +1 与 −1 模式质量差，累加归一化。
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

    // ─────────────────────────────────────────────────────────────
    // 快子 CTC 噪声（几何 → 噪声映射的结果）
    // ─────────────────────────────────────────────────────────────
    struct TachyonCTCNoise
    {
        double dephasing = 0.0;     // 退相概率 p（Wilson line 不对称性）
        double depolarizing = 0.0;  // 去极化概率 p（额外维「维度扩散」）
        double damping = 0.0;       // 振幅阻尼 γ（快子真空不稳定性）
    };

    // 从 KK 几何 + 快子势计算 CTC 噪声三参量。
    //   dephasing    = asym / (1 + asym)，asym = Σ_i |θ_i|
    //   depolarizing = n / (n + 1)，n = 额外维数
    //   damping      = μ² / (μ² + λ)（真空 φ_vac² = μ²/λ，μ² 大 → 真空越不稳定）
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

    // 信道串行组合：(N2 ∘ N1)(ρ) = N2(N1(ρ))，Kraus = { L_j · K_i }（先 N1 后 N2）。
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

    // 快子 CTC 噪声信道：退相 ∘ 去极化 ∘ 振幅阻尼（几何 → 完整 P-CTC 信道）。
    //   顺序：先振幅阻尼（真空不稳定），再退相（Wilson line），最后去极化（维度扩散）。
    inline QuantumChannel tachyon_ctc_channel(const KaluzaKleinND &kk, double mu2, double lambda)
    {
        const TachyonCTCNoise noise = tachyon_ctc_noise(kk, mu2, lambda);

        QuantumChannel N = QuantumChannel::amplitude_damping(noise.damping);
        if (noise.dephasing > 0.0)
            N = compose_channels(N, QuantumChannel::dephasing(noise.dephasing));
        if (noise.depolarizing > 0.0)
            N = compose_channels(N, QuantumChannel::depolarizing(noise.depolarizing));
        N.kind = -1; // 组合信道非预置信道（几何来源）
        return N;
    }

    // ─────────────────────────────────────────────────────────────
    // 多拓扑逆因果通信（结合 @layer 标签的多维 coord）
    //   @layer(time, thread, coord=(c0,c1,...,c_{n-1})) 的 coord 维度 n 即额外维数，
    //   每个 coord 分量对应一个 DD 维度（额外维 S¹）。据此计算该拓扑下的
    //   「n 个 DD 维度的嘈杂封闭类时曲线」逆因果容量。
    // ─────────────────────────────────────────────────────────────
    struct MultiTopologyRetrocausal
    {
        KaluzaKleinND kk;              // n 个额外维（DD 维度，对应 coord 分量）
        double mu2 = 1.0;              // 快子质量参数（m²_eff = −μ²）
        double lambda = 1.0;           // 快子势自耦合

        // @layer coord 维度 = 额外维数
        size_t coord_dim() const { return kk.num_extra(); }

        // 该拓扑下的 CTC 噪声三参量
        TachyonCTCNoise noise() const { return tachyon_ctc_noise(kk, mu2, lambda); }

        // 该拓扑下的 P-CTC 噪声信道
        QuantumChannel channel() const { return tachyon_ctc_channel(kk, mu2, lambda); }

        // 该拓扑下的逆因果容量
        RetrocausalCapacity capacity(double eps = 1e-3) const
        {
            return retrocausal_capacity(channel(), eps);
        }

        // 每个额外维（DD 维度）独立构成的单维 CTC 量子容量（各维 Wilson line 不同）。
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

        // 维度权衡（dimension trade-off）：逐次增加一个额外维时的 CTC 容量序列。
        //   额外维是「维度扩散噪声源」（去极化 ∝ n/(n+1)），故容量随维数单调递减；
        //   但逆因果容量通过 P-CTC 后选择归一化对噪声高度鲁棒（对比前向容量的
        //   快速衰减，见 gain）。返回 {容量(1 维), ..., 容量(n 维)}。
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