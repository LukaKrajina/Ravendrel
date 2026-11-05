#pragma once
//
// qchain :: 量子密钥分发（QKD）原语
//
// 对齐量子密钥分发的三大主流协议：
//   • BB84       —— 制备-测量型（Bennett–Brassard 1984），四种偏振态 / 两套基。
//   • E91        —— 纠缠型（Ekert 1991），利用 Bell 不等式的违背检测窃听。
//   • DecoyBB84  —— 诱骗态 BB84，抵抗光子数分离（PNS）攻击（Hwang 2003 / Lo et al. 2005）。
//
// 所有协议在 qhal::IQuantumBackend 抽象之上实现：接入 QVM（本地仿真）或 QM（真实
// 超导/离子阱/中性原子后端）即得可执行会话。为契合真实 QKD 的「逐光子发送」物理
// 事实，仿真按单比特/单对纠缠逐轮进行，避免大规模态矢量指数爆炸。
//
#include "../common.hpp"
#include "hash.hpp"
#include "../../qhal/IQuantumBackend.hpp"
#include <algorithm>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace qchain::qkd
{

    using qchain::Byte;
    using qchain::Bytes;
    using qchain::Rng;
    using crypto::sha256;

    enum class QkdProtocol
    {
        BB84,
        E91,
        DecoyBB84
    };

    // QKD 会话结果：包含筛后密钥、误码率、最终（隐私放大后）密钥等。
    struct QkdResult
    {
        Bytes sifted_key;       // 基匹配后的筛后密钥（隐私放大前）
        Bytes shared_key;       // 最终共享密钥（隐私放大后，32 字节）
        double error_rate = 0.0; // 量子比特误码率 QBER
        double key_rate = 0.0;   // 有效密钥率（筛后比特 / 原始比特）
        size_t raw_bits = 0;
        size_t sifted_bits = 0;
    };

    namespace detail
    {
        // 将单比特复位到 |0>（坍缩测量 + 条件翻转）。
        inline void reset_qubit(qhal::IQuantumBackend *backend, size_t q)
        {
            if (backend->measure(q) == 1)
                backend->apply_x(q);
        }

        // 测量沿 XZ 平面角度 θ 的观测量 O(θ) = cosθ·Z + sinθ·X。
        // 等价于施加 R_y(-θ) 旋转后测 Z：R_y(-θ) = Rz(-π/2)·H·Rz(-θ)·H·Rz(π/2)。
        inline int measure_angle(qhal::IQuantumBackend *backend, size_t q, double theta)
        {
            backend->apply_rz(q, -M_PI / 2.0);
            backend->apply_h(q);
            backend->apply_rz(q, -theta);
            backend->apply_h(q);
            backend->apply_rz(q, M_PI / 2.0);
            return backend->measure(q);
        }
    } // namespace detail

    // ══════════════════════════════════════════════════════════════════
    // BB84 —— 制备-测量型 QKD
    // ══════════════════════════════════════════════════════════════════
    class BB84
    {
    public:
        //
        // 在共享后端上运行一次 BB84 会话（逐比特）。
        //   backend    : 量子后端（QVM 或 QM）
        //   num_rounds : 原始传送比特数
        //   noise      : 信道误码率（仿真用，对筛后密钥随机翻转）
        //
        static QkdResult run(qhal::IQuantumBackend *backend, size_t num_rounds, double noise = 0.0)
        {
            Rng rng;
            // 在共享后端上追加分配一个真实 id（旧实现硬编码 id 0，会与共享后端上
            // 其他组件（rvdl / qchain 其他功能）占用的 qubit 0 冲突：制备会砸掉
            // 别人的量子态，末尾的 release 又会把别人的比特标记为空闲）。
            const size_t q = backend->get_num_qubits();
            backend->allocate_qubits(q + 1);

            std::vector<bool> alice_bits(num_rounds), alice_basis(num_rounds), bob_basis(num_rounds);
            std::vector<int> outcomes(num_rounds);

            for (size_t i = 0; i < num_rounds; ++i)
            {
                alice_bits[i] = rng.random_bit();
                alice_basis[i] = rng.random_bit(); // false=Z, true=X
                bob_basis[i] = rng.random_bit();

                // Alice 制备：先编码比特，再按基旋转。
                detail::reset_qubit(backend, q);
                if (alice_bits[i])
                    backend->apply_x(q);
                if (alice_basis[i])
                    backend->apply_h(q);

                // Bob 测量（X 基则先施加 H）。
                if (bob_basis[i])
                    backend->apply_h(q);
                outcomes[i] = backend->measure(q);
            }
            backend->release_qubit(q);

            // 筛：保留基匹配的轮次。
            Bytes sifted;
            for (size_t i = 0; i < num_rounds; ++i)
                if (alice_basis[i] == bob_basis[i])
                    sifted.push_back(Byte(alice_bits[i] ? 1 : 0));

            // 仿真信道噪声：对筛后密钥随机翻转比特。
            Bytes received = sifted;
            if (noise > 0.0)
                for (auto &b : received)
                    if (rng.uniform_real() < noise)
                        b ^= 1;

            // 误码率估计（仿真中可精确比对 Alice 原比特）。
            size_t errors = 0;
            for (size_t i = 0; i < sifted.size(); ++i)
                if (sifted[i] != received[i])
                    ++errors;
            double qber = sifted.empty() ? 0.0 : double(errors) / double(sifted.size());

            QkdResult res;
            res.raw_bits = num_rounds;
            res.sifted_bits = sifted.size();
            res.sifted_key = received;
            res.error_rate = qber;
            res.key_rate = num_rounds == 0 ? 0.0 : double(sifted.size()) / double(num_rounds);
            // 隐私放大：对（筛后密钥 ‖ 基信息）哈希，压缩并消除窃听者可能掌握的部分信息。
            Bytes material = received;
            for (size_t i = 0; i < num_rounds; ++i)
                material.push_back(Byte(alice_basis[i] ? 1 : 0));
            Hash256 sk = sha256(material);
            res.shared_key = Bytes(sk.begin(), sk.end());
            return res;
        }
    };

    // ══════════════════════════════════════════════════════════════════
    // E91 —— 纠缠型 QKD
    // ══════════════════════════════════════════════════════════════════
    class E91
    {
    public:
        //
        // 基于 Bell 对（|00>+|11>）的纠缠 QKD：Alice / Bob 各自测量纠缠对的一半，
        // 基匹配的轮次贡献密钥；用 CHSH 相关函数 S 值估计窃听（|S|≤2 经典界，
        // 2√2 为量子上限，无窃听时 |S| 接近 2√2）。
        //
        static QkdResult run(qhal::IQuantumBackend *backend, size_t num_rounds)
        {
            Rng rng;
            // 追加分配两个真实 id（旧实现硬编码 qa=0/qb=1，与共享后端上其他组件冲突）。
            const size_t qa = backend->get_num_qubits();
            backend->allocate_qubits(qa + 2);
            const size_t qb = qa + 1;
            std::vector<bool> alice_basis(num_rounds), bob_basis(num_rounds);
            std::vector<int> a_out(num_rounds), b_out(num_rounds);

            for (size_t i = 0; i < num_rounds; ++i)
            {
                // 制备 Bell 对：H(0); CNOT(0,1)。
                detail::reset_qubit(backend, qa);
                detail::reset_qubit(backend, qb);
                backend->apply_h(qa);
                backend->apply_cnot(qa, qb);

                alice_basis[i] = rng.random_bit();
                bob_basis[i] = rng.random_bit();

                if (alice_basis[i])
                    backend->apply_h(qa);
                if (bob_basis[i])
                    backend->apply_h(qb);

                a_out[i] = backend->measure(qa);
                b_out[i] = backend->measure(qb);
            }
            backend->release_qubit(qa);
            backend->release_qubit(qb);

            // 筛：基匹配轮次贡献密钥（Alice 与 Bob 比特相同）。
            Bytes sifted;
            size_t agree = 0;
            for (size_t i = 0; i < num_rounds; ++i)
            {
                if (alice_basis[i] == bob_basis[i])
                {
                    sifted.push_back(Byte(a_out[i] & 1));
                    if (a_out[i] == b_out[i])
                        ++agree;
                }
            }

            QkdResult res;
            res.raw_bits = num_rounds;
            res.sifted_bits = sifted.size();
            res.sifted_key = sifted;
            // 误码率：基匹配轮次中比特不一致的比例。
            res.error_rate = sifted.empty() ? 0.0 : 1.0 - double(agree) / double(sifted.size());
            res.key_rate = num_rounds == 0 ? 0.0 : double(sifted.size()) / double(num_rounds);
            Hash256 sk = sha256(sifted);
            res.shared_key = Bytes(sk.begin(), sk.end());
            return res;
        }

        //
        // CHSH 相关函数 S = E(a,b) + E(a,b') + E(a',b) - E(a',b')。
        // 使用最优测量角 a=0, a'=π/2, b=π/4, b'=-π/4：经典局域实在论界 |S| ≤ 2，
        // 量子无窃听时 |S| → 2√2 ≈ 2.828（Bell 不等式被违背 ⇒ 存在纠缠 ⇒ 可检测窃听）。
        //
        static double chsh_estimate(qhal::IQuantumBackend *backend, size_t num_rounds)
        {
            const size_t qa = backend->get_num_qubits();
            backend->allocate_qubits(qa + 2);
            const size_t qb = qa + 1;
            auto corr = [&](double a_ang, double b_ang) -> double {
                int same = 0, total = 0;
                for (size_t i = 0; i < num_rounds; ++i)
                {
                    detail::reset_qubit(backend, qa);
                    detail::reset_qubit(backend, qb);
                    backend->apply_h(qa);
                    backend->apply_cnot(qa, qb); // 制备 Bell 对 |Φ+>
                    int a = detail::measure_angle(backend, qa, a_ang);
                    int b = detail::measure_angle(backend, qb, b_ang);
                    ++total;
                    if (a == b)
                        ++same;
                }
                return 2.0 * double(same) / double(total) - 1.0;
            };
            const double a0 = 0.0, a1 = M_PI / 2.0;
            const double b0 = M_PI / 4.0, b1 = -M_PI / 4.0;
            double E00 = corr(a0, b0);
            double E01 = corr(a0, b1);
            double E10 = corr(a1, b0);
            double E11 = corr(a1, b1);
            backend->release_qubit(qa);
            backend->release_qubit(qb);
            return E00 + E01 + E10 - E11;
        }
    };

    // ══════════════════════════════════════════════════════════════════
    // DecoyBB84 —— 诱骗态 BB84
    // ══════════════════════════════════════════════════════════════════
    //
    // 通过随机插入「诱骗态」（不同平均光子数）检测光子数分离（PNS）攻击：若窃听者
    // 对诱骗态与信号态区别对待，其增益/误码统计将偏离合法信道，从而暴露。
    class DecoyBB84
    {
    public:
        struct DecoyResult : QkdResult
        {
            size_t decoy_rounds = 0;
            double pns_attack_probability = 0.0; // 估计的 PNS 攻击概率
        };

        //
        //   backend    : 量子后端
        //   num_rounds : 总轮数
        //   decoy_ratio: 诱骗态占比（典型 0.2~0.5）
        //   pns_rate   : 仿真注入的 PNS 攻击比例（用于演示检测能力）
        //
        static DecoyResult run(qhal::IQuantumBackend *backend, size_t num_rounds,
                               double decoy_ratio = 0.3, double pns_rate = 0.0)
        {
            Rng rng;
            // 复用 BB84 的制备-测量管线，但逐轮打标（信号 / 诱骗）。
            const size_t q = backend->get_num_qubits();
            backend->allocate_qubits(q + 1);

            std::vector<bool> alice_bits(num_rounds), alice_basis(num_rounds), bob_basis(num_rounds);
            std::vector<bool> is_decoy(num_rounds);
            std::vector<int> outcomes(num_rounds);
            size_t decoy_count = 0;

            for (size_t i = 0; i < num_rounds; ++i)
            {
                alice_bits[i] = rng.random_bit();
                alice_basis[i] = rng.random_bit();
                bob_basis[i] = rng.random_bit();
                is_decoy[i] = rng.uniform_real() < decoy_ratio;
                if (is_decoy[i])
                    ++decoy_count;

                detail::reset_qubit(backend, q);
                if (alice_bits[i])
                    backend->apply_x(q);
                if (alice_basis[i])
                    backend->apply_h(q);

                // 仿真 PNS 攻击：窃听者对非诱骗态做「先测量再重发」，引入额外误码。
                if (!is_decoy[i] && rng.uniform_real() < pns_rate)
                    if (rng.random_bit())
                        backend->apply_x(q); // 测量-重发导致比特翻转

                if (bob_basis[i])
                    backend->apply_h(q);
                outcomes[i] = backend->measure(q);
            }
            backend->release_qubit(q);

            // 筛 + 统计诱骗态与信号态误码差异。
            Bytes sifted;
            size_t decoy_errors = 0, decoy_sifted = 0;
            size_t signal_errors = 0, signal_sifted = 0;
            for (size_t i = 0; i < num_rounds; ++i)
            {
                if (alice_basis[i] != bob_basis[i])
                    continue;
                int expected = alice_bits[i] ? 1 : 0;
                bool err = (outcomes[i] != expected);
                sifted.push_back(Byte(expected));
                if (is_decoy[i])
                {
                    ++decoy_sifted;
                    if (err)
                        ++decoy_errors;
                }
                else
                {
                    ++signal_sifted;
                    if (err)
                        ++signal_errors;
                }
            }

            double decoy_qber = decoy_sifted ? double(decoy_errors) / double(decoy_sifted) : 0.0;
            double signal_qber = signal_sifted ? double(signal_errors) / double(signal_sifted) : 0.0;

            DecoyResult res;
            res.raw_bits = num_rounds;
            res.sifted_bits = sifted.size();
            res.sifted_key = sifted;
            res.decoy_rounds = decoy_count;
            res.error_rate = decoy_qber;
            // PNS 攻击概率估计：信号态误码显著高于诱骗态时判定存在测量-重发攻击。
            res.pns_attack_probability = std::max(0.0, signal_qber - decoy_qber);
            res.key_rate = num_rounds == 0 ? 0.0 : double(sifted.size()) / double(num_rounds);
            Hash256 sk = sha256(sifted);
            res.shared_key = Bytes(sk.begin(), sk.end());
            return res;
        }
    };

}