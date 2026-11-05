#pragma once
//
// RetrocausalCapacity.hpp —— 量子通道的逆因果容量（回程能力）
//
// 理论来源（结合 arXiv 论文）：
//   • arXiv:2509.08965 —— Retrocausal Capacity of a Quantum Channel:
//     通过有噪后选择闭合类时曲线（P-CTC）从未来向过去通信。核心量：
//        I_max(N) = log2 λ_max(J)             （max-information）
//        I_doe(N) = log2 d_in − log2 λ_min⁺(J)（Doeblin information）
//        Q_retro  = ½ (I_max + I_doe^∞)        （渐近逆因果量子容量）
//        C_retro  = I_max + I_doe^∞            （渐近逆因果经典容量）
//     其中 J = Σ_k vec(K_k) vec(K_k)† 是信道的 Choi 矩阵。
//   • arXiv:2609.00168 —— 量子开关 / 量子时间翻转的纠缠能力（时间不对称性 →
//     「回程增益」G = Q_retro / Q_forward）。
//   • arXiv:2609.29831 —— q-变形数 / 极限集（「q-变形逆因果容量」，q 参数化因果强度）。
//   • arXiv:2609.31619 —— 元认知置信度信号（后选择成功概率 = 「逆因果置信度」）。
//
// 设计法则（对应 TQNF 的第四支柱「因果」）：
//   把「逆因果容量」升格为时空框架的因果资源度量，与几何/拓扑/耗散三支柱并列；
//   与 rvdl::spacetime 的快子场/叶状结构（Foliation）互补 —— 哨兵验证因果律
//   不被破坏，容量度量「回程通信还能携带多少信息」。
//
// 纯 numqk 依赖（跨层），后端无关。
//
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

    // ─────────────────────────────────────────────────────────────
    // 预置信道枚举（rvdl 语言层 kind 参数映射）
    // ─────────────────────────────────────────────────────────────
    enum class RetrocausalChannelKind
    {
        Depolarizing = 0,   // 单比特去极化，p = 去极化概率
        Dephasing = 1,      // 单比特退相，p = 退相概率
        BitFlip = 2,        // 单比特翻转（Pauli X），p = X 概率
        AmplitudeDamping = 3, // 单比特振幅阻尼，p = γ（衰变率）
        UnitaryHadamard = 4,  // 无噪声幺正（Hadamard），p 忽略
        BitPhaseFlip = 5,   // 单比特比特-相位翻转（Pauli Y），p = Y 概率
    };

    // ─────────────────────────────────────────────────────────────
    // 量子通道：Kraus 表示 N(ρ) = Σ_k K_k ρ K_k†，K_k : dim_in → dim_out
    // ─────────────────────────────────────────────────────────────
    struct QuantumChannel
    {
        size_t dim_in = 2;
        size_t dim_out = 2;
        std::vector<numqk::CTensor> kraus;
        int kind = -1;       // 预置信道类型（-1 = 通用 Kraus）
        double param = 0.0;  // 信道参数 p（供参数灵敏度 / 容量流使用）

        // 从 Kraus 算子集合构造
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

        // 单比特幺正（单个 Kraus 算子 = U）
        static QuantumChannel unitary(const numqk::CTensor &U)
        {
            return from_kraus({U}, 2, 2);
        }

        // 去极化 N(ρ) = (1−p)ρ + p I/2，Kraus = {√(1−3p/4) I, √(p/4) X, √(p/4) Y, √(p/4) Z}
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

        // 退相 N(ρ) = (1−p)ρ + p Z ρ Z
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

        // 比特翻转 N(ρ) = (1−p)ρ + p X ρ X
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

        // 振幅阻尼 K0 = [[1,0],[0,√(1−γ)]], K1 = [[0,√γ],[0,0]]
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

        // 比特-相位翻转（Pauli Y）：N(ρ) = (1−p)ρ + p Y ρ Y
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

        // 幺正 Hadamard（无噪声）
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

        // 按 kind 构造预置信道（供 rvdl 语言层 / 容量流参数扰动使用）
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

        // 标量乘法（numqk::Tensor 无内置 scale，直接操作 data）
        static numqk::CTensor scale(const numqk::CTensor &M, double s)
        {
            numqk::CTensor out(M.get_shape(), false);
            for (size_t i = 0; i < M.size(); ++i)
                out.data()[i] = M.data()[i] * s;
            return out;
        }
    };

    // ─────────────────────────────────────────────────────────────
    // Choi 矩阵：J(N) = Σ_k vec(K_k) vec(K_k)†
    //   vec(K) 是 K 的行优先展平（d_out·d_in 维列向量），J 为 d_out·d_in 方阵。
    //   对保迹信道 Tr(J) = d_in；J 半正定（Hermitian）。
    // ─────────────────────────────────────────────────────────────
    inline numqk::CTensor choi_matrix(const QuantumChannel &N)
    {
        const size_t D = N.dim_out * N.dim_in;
        numqk::CTensor J({D, D}, false);
        for (const auto &K : N.kraus)
        {
            numqk::CTensor v({D}, false);
            for (size_t i = 0; i < D; ++i)
                v.data()[i] = K.data()[i]; // 行优先展平 = vec(K)
            numqk::CTensor vv = numqk::outer(v, v);
            for (size_t idx = 0; idx < D * D; ++idx)
                J.data()[idx] += vv.data()[idx];
        }
        return J;
    }

    // ─────────────────────────────────────────────────────────────
    // 逆因果信息量（论文 1 的核心量）
    //   imax     = log2 λ_max(J)
    //   idoe     = log2 d_in − log2 λ_min⁺(J)（λ_min⁺ = 支持上最小正特征值）
    //   spectrum = Choi 特征值（降序）→ 「逆因果容量谱」（信道因果指纹）
    //   purity   = Tr(J²)/Tr(J)²（逆因果意义上的纯净度）
    //   flatness = λ_min⁺/λ_max（回程通道的「噪声白度」）
    // ─────────────────────────────────────────────────────────────
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
        const std::vector<double> evals = numqk::hermitian_eigenvalues(J); // 升序
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

        info.spectrum.assign(evals.rbegin(), evals.rend()); // 降序

        const double trJ = numqk::trace(J).real();
        const double trJ2 = numqk::purity(J); // Tr(J²)
        info.purity = (std::abs(trJ) > 1e-12) ? trJ2 / (trJ * trJ) : 0.0;
        info.flatness = (lam_max > 1e-12) ? lam_min_pos / lam_max : 0.0;
        return info;
    }

    // ─────────────────────────────────────────────────────────────
    // 信道的张量积 / 张量幂（供 Doeblin information 正则化）
    //   (N_A ⊗ N_B) 的 Kraus = {K_i ⊗ L_j}
    // ─────────────────────────────────────────────────────────────
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

    // Doeblin information 正则化：I_doe^∞ = lim_{n→∞} I_doe(N^⊗n) / n。
    //   I_doe 一般不可加但超可加；对协变信道（去极化/退相/比特翻转/振幅阻尼）可加，
    //   此时 I_doe^∞ = I_doe。通过 n=2 检查可加性，一般信道继续升幂逼近。
    inline double idoe_regularized(const QuantumChannel &N, size_t max_n = 4)
    {
        const double idoe1 = retrocausal_info(N).idoe;
        const size_t base = N.dim_in * N.dim_out;

        double best = idoe1;
        for (size_t n = 2; n <= max_n; ++n)
        {
            // Choi 维度保护：N^⊗n 的 Choi 矩阵维度 = base^n，超过 4096 时
            // Hermitian 特征分解成本过高（O(D³)），停止升幂。
            size_t dim = 1;
            for (size_t i = 0; i < n; ++i)
                dim *= base;
            if (dim > 4096)
                break;

            const QuantumChannel Nn = tensor_power(N, n);
            const double idoe_n = retrocausal_info(Nn).idoe / static_cast<double>(n);
            best = std::max(best, idoe_n); // 超可加 → 取上确界
            if (std::abs(idoe_n - idoe1) < 1e-6)
                break; // 可加：已收敛
        }
        return best;
    }

    // ─────────────────────────────────────────────────────────────
    // 前向经典容量（Holevo 容量，供「回程增益」做前向基准）
    //   预置信道用精确的解析 / 半解析公式（非网格近似）：
    //     去极化              χ = 1 − h2(p/2)
    //     退相 / 比特翻转 / Y 翻转 χ = 1 − h2(p)
    //     振幅阻尼            χ = max_p [ h2((1−p)(1−γ)) − (1−p)·h2(γ) ]（一维优化）
    //     幺正 Hadamard       χ = 1
    //   通用 Kraus 退化为 Bloch 球网格数值扫描（2 个正交纯态 ensemble）。
    // ─────────────────────────────────────────────────────────────
    // 二元熵 h2(x) = −x log2 x − (1−x) log2(1−x)
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
        // ── 预置信道：解析 / 半解析 Holevo 容量 ──
        switch (static_cast<RetrocausalChannelKind>(N.kind))
        {
        case RetrocausalChannelKind::Depolarizing:
            // qubit 去极化：χ = 1 − h2(p/2)（|0⟩/|1⟩ 等概率 ensemble 达到）
            return 1.0 - binary_entropy(N.param / 2.0);
        case RetrocausalChannelKind::Dephasing:
        case RetrocausalChannelKind::BitFlip:
        case RetrocausalChannelKind::BitPhaseFlip:
            // 退相 / 比特翻转 / 比特-相位翻转：χ = 1 − h2(p)
            return 1.0 - binary_entropy(N.param);
        case RetrocausalChannelKind::AmplitudeDamping:
        {
            // 振幅阻尼：最优 ensemble 为 |0⟩/|1⟩ 基，χ = max_p [ h2((1−p)(1−γ)) − (1−p)·h2(γ) ]
            const double gamma = N.param;
            const double hg = binary_entropy(gamma);
            double best = 0.0;
            const int steps = 512;
            for (int i = 0; i <= steps; ++i)
            {
                const double p = static_cast<double>(i) / static_cast<double>(steps); // P(input=0)
                const double r = (1.0 - p) * (1.0 - gamma);                            // P(output=1)
                const double chi = binary_entropy(r) - (1.0 - p) * hg;
                best = std::max(best, chi);
            }
            return best;
        }
        case RetrocausalChannelKind::UnitaryHadamard:
            return 1.0; // 幺正信道 Holevo 容量 = 1 bit（qubit）
        default:
            break;
        }

        // ── 通用 Kraus：Bloch 球网格数值扫描（2 个正交纯态等概率 ensemble）──
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

    // ─────────────────────────────────────────────────────────────
    // 逆因果容量（论文 1 核心结果）
    //   q_asymptotic = ½ (I_max + I_doe^∞)
    //   c_asymptotic = I_max + I_doe^∞
    //   q_one_shot   = log2⌊√(ε/(1−ε)·2^(I_max+I_doe) + 1)⌋
    //   c_one_shot   = log2⌊(ε/(1−ε)·2^(I_max+I_doe) + 1)⌋
    //   gain         = Q_retro / Q_forward（回程增益，时间不对称性）
    // ─────────────────────────────────────────────────────────────
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

        // 单次容量（用非正则化 I_max + I_doe）
        const double e = (eps > 0.0 && eps < 1.0) ? eps / (1.0 - eps) : 0.0;
        const double two_sum = std::pow(2.0, info.imax + info.idoe);
        cap.q_one_shot = std::log2(std::max(1.0, std::floor(std::sqrt(e * two_sum + 1.0))));
        cap.c_one_shot = std::log2(std::max(1.0, std::floor(e * two_sum + 1.0)));

        // 回程增益：逆因果量子容量 / 前向经典容量
        const double fwd = forward_classical_capacity(N);
        cap.gain = (fwd > 1e-12) ? cap.q_asymptotic / fwd : 0.0;
        return cap;
    }

    // ─────────────────────────────────────────────────────────────
    // q-变形逆因果容量（借鉴 arXiv:2609.29831 的 q-变形数思想）
    //   在「有效维度」域做 q-凸组合，q ∈ [−1, 1] 参数化因果强度：
    //     C_q = log2( (1+q)/2 · 2^{C_retro} + (1−q)/2 · 2^{C_forward} )
    //   q = −1 → 纯前向；q = +1 → 纯回程；q 连续扫出「因果相图」。
    // ─────────────────────────────────────────────────────────────
    inline double q_retrocausal_capacity(const QuantumChannel &N, double q)
    {
        q = std::max(-1.0, std::min(1.0, q));
        const double c_retro = retrocausal_capacity(N).c_asymptotic;
        const double c_forward = forward_classical_capacity(N);
        const double wp = 0.5 * (1.0 + q); // 回程权重
        const double wf = 0.5 * (1.0 - q); // 前向权重
        const double eff_dim = wp * std::pow(2.0, c_retro) + wf * std::pow(2.0, c_forward);
        return std::log2(std::max(eff_dim, 1.0));
    }

    // ─────────────────────────────────────────────────────────────
    // 后选择放大协议（amplified probabilistic teleportation，论文 1 的最优策略）
    //   女儿留下量子记忆 → 父亲 Bell 测量 → 有噪 P-CTC 非线性放大成功概率。
    //   真实密度矩阵模拟：构造归一化最大纠缠态 |Φ⁺⟩，投影到归一化 Choi 态，
    //   精确计算后选择概率、放大倍数（相对无 CTC 理想概率 1/d²）、传输保真度。
    //   fidelity           = 后选择理想结果（|Φ⁺⟩）占比 = 传输保真度
    //   postselection_prob = ⟨Φ⁺| ρ_choi |Φ⁺⟩（归一化 Choi 态投影）
    //   amplification      = p_succ / (1/d²)（相对无 CTC 的放大倍数）
    //   confidence         = 「逆因果置信度」= p_succ · amplification（结合 arXiv:2609.31619）
    // ─────────────────────────────────────────────────────────────
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

        // 1. 归一化最大纠缠态 |Φ⁺⟩ = (1/√d) Σ |ii⟩
        numqk::CTensor phi({d * d}, false);
        for (size_t i = 0; i < d; ++i)
            phi.data()[i * d + i] = numqk::Complex(1.0 / std::sqrt(static_cast<double>(d)), 0.0);
        numqk::CTensor Phi = numqk::pure_state_density(phi); // |Φ⁺⟩⟨Φ⁺|

        // 2. 归一化 Choi 态 ρ_choi = J / Tr(J)
        const numqk::CTensor J = choi_matrix(N);
        const double trJ = numqk::trace(J).real();
        numqk::CTensor rho_choi({J.get_shape()[0], J.get_shape()[0]}, false);
        for (size_t i = 0; i < J.size(); ++i)
            rho_choi.data()[i] = J.data()[i] / trJ;

        // 3. 后选择概率 p_succ = ⟨Φ⁺| ρ_choi |Φ⁺⟩ = Tr(Φ · ρ_choi)
        const double p_succ = numqk::trace(Phi.matmul(rho_choi)).real();

        // 4. 无 CTC 标准隐形传态理想 Bell 结果概率 = 1/d²，放大倍数
        const double p_noctc = 1.0 / static_cast<double>(d * d);
        const double amplification = p_succ / std::max(p_noctc, 1e-30);

        // 5. 传输保真度 = 理想结果（|Φ⁺⟩）占比 = 后选择概率
        const double fidelity = std::max(0.0, std::min(1.0, p_succ));

        // 6. 逆因果置信度（元认知信号，结合 arXiv:2609.31619）：
        //    用 logistic 归一化放大倍数，映射到 [0,1] 且保持区分度
        //    （放大倍数越大，回程通信越可靠，置信度越高）。
        const double confidence = amplification / (1.0 + amplification);

        ProtocolResult r;
        r.fidelity = fidelity;
        r.postselection_prob = p_succ;
        r.amplification = amplification;
        r.confidence = confidence;
        return r;
    }

    // ─────────────────────────────────────────────────────────────
    // 回程容量流（逆因果信息沿时间反向流动的速率）
    //   对信道参数 p 做数值微分得到容量灵敏度 dQ/dp、dC/dp，乘以参数演化率 dp_dt：
    //     q_flow = dQ_retro/dp · dp_dt，c_flow = dC_retro/dp · dp_dt
    //   可与 Foliation 的时间反向演化率挂钩（dp_dt 由调用方传入）。
    // ─────────────────────────────────────────────────────────────
    struct RetrocausalFlow
    {
        double dq_dp = 0.0;
        double dc_dp = 0.0;
        double q_flow = 0.0;
        double c_flow = 0.0;
    };

    // 通用参数化信道族的容量流（支持 kind<0 的通用 Kraus 信道）：
    //   family(p) 返回参数为 p 的信道，数值微分得到容量对 p 的灵敏度。
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
            return RetrocausalFlow{}; // 通用 Kraus 无参数化：需用 retrocausal_flow_perturbed
        return retrocausal_flow_perturbed(
            [kind = N.kind](double p) { return QuantumChannel::make(kind, p); },
            N.param, dp_dt, h);
    }

}