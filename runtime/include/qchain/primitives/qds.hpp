#pragma once
//
// qchain :: 量子数字签名（QDS）原语
//
// 参考 Gottesman–Chuang 量子数字签名的核心思想：
//   • 量子单向函数 QOWF：经典密钥 k → 量子态 |f(k)>，难以反演且不可克隆。
//   • 公钥 = 量子态 |f(k)> 的多个副本（分发给验证者，每次验证消耗一份）。
//   • 签名 = 揭示部分经典密钥（子集揭示）；验证者测量量子公钥副本并与揭示值比对。
//
// 【子集揭示设计】签名只揭示由消息摘要所决定的位置子集 S(message) 上的比特值，
// 而不是全量比特。单个签名不会泄露 |f(k)> 在其余位置的信息：攻击者想用一个已有
// 签名伪造另一条消息，必须猜出未被揭示位置的值，成功率随未揭示位数指数下降。
// （此前的实现把签名做成「全量值 ⊕ 消息摘要」——任意一个有效签名都能反推出完整的
// f(k)，从而可以伪造任意消息的签名；该安全性缺陷已修正。）
//
// 与经典（后量子）签名不同，QDS 的不可伪造性源自量子不可克隆定理，而非计算困难
// 假设。这儿给出可在 qhal::IQuantumBackend 上制备/测量的参考模型；
// 生产环境的 QDS 需量子存储器与测量设备无关（MDI）扩展。
//
// 密钥规模建议：m ≥ 128（本参考模型的安全参数由未揭示位数决定，m=64 时单次伪造
// 成功率的量级约为 (3/4)^m ≈ 1e-8）。
//
#include "../common.hpp"
#include "hash.hpp"
#include "../../qhal/IQuantumBackend.hpp"
#include <iostream>

namespace qchain::qds
{

    using qchain::Byte;
    using qchain::Bytes;
    using qchain::Rng;
    using crypto::sha256;

    // 量子态制备描述：m 个量子比特，每个由 (基, 值) 决定。
    //   basis=false → Z 基（|0>/|1>），true → X 基（|+>/|->）。
    struct PreparationSpec
    {
        std::vector<bool> basis;
        std::vector<bool> value;
        size_t size() const { return basis.size(); }
    };

    // ─── 量子单向函数（QOWF）──────────────────────────────────────────
    //
    // 由经典密钥经 SHA-256 扩展出 2m 比特，作为 m 个比特的 (基, 值)。
    // 这是「经典 → 量子态制备描述」的可复现映射；真实 QDS 的 QOWF 应使用更严格的
    // 量子单向构造，此处以哈希扩展作为可运行的参考模型。
    class QuantumOneWayFunction
    {
    public:
        // 计数器迭代哈希扩展到至少 needed_bits 位的比特流。
        static Bytes stream(const Bytes &key, size_t needed_bits)
        {
            Bytes out;
            for (uint64_t c = 0; out.size() * 8 < needed_bits; ++c)
            {
                Bytes block = key;
                for (int i = 0; i < 8; ++i)
                    block.push_back(Byte((c >> (8 * i)) & 0xFF));
                Hash256 h = sha256(block);
                out.insert(out.end(), h.begin(), h.end());
            }
            return out;
        }

        static PreparationSpec evaluate(const Bytes &key, size_t m)
        {
            Bytes s = stream(key, 2 * m);
            PreparationSpec spec;
            spec.basis.resize(m);
            spec.value.resize(m);
            for (size_t i = 0; i < m; ++i)
            {
                // stream 返回字节流：第 b 位 = s[b/8] 的第 b%8 位。
                spec.basis[i] = ((s[(2 * i) / 8] >> ((2 * i) % 8)) & 1) != 0;
                spec.value[i] = ((s[(2 * i + 1) / 8] >> ((2 * i + 1) % 8)) & 1) != 0;
            }
            return spec;
        }
    };

    // ─── 量子数字签名 ─────────────────────────────────────────────────
    class QuantumDigitalSignature
    {
    public:
        struct SigningKey
        {
            Bytes secret;                // 经典秘密（仅签名者持有）
            PreparationSpec public_spec; // 量子公钥的制备描述（值编码在量子态中）
        };

        // 生成一次性签名密钥：m 比特量子公钥。
        static SigningKey keygen(size_t m)
        {
            if (m == 0)
                throw std::invalid_argument("qchain::qds: keygen requires m > 0");
            Rng rng;
            SigningKey kp;
            kp.secret = rng.random_bytes(32);
            kp.public_spec = QuantumOneWayFunction::evaluate(kp.secret, m);
            return kp;
        }

        // 揭示位置子集 S(message) = { i : mask[i] == 1 }，掩码由消息的哈希流产生。
        // 支持任意 m（不再像旧实现那样对超过 256 比特的 m 越界读取 32 字节摘要）。
        static std::vector<bool> reveal_mask(const Bytes &message, size_t m)
        {
            Hash256 dg = sha256(message);
            Bytes key(dg.begin(), dg.end());
            Bytes mask_stream = QuantumOneWayFunction::stream(key, m);
            std::vector<bool> mask(m);
            for (size_t i = 0; i < m; ++i)
                mask[i] = ((mask_stream[i / 8] >> (i % 8)) & 1) != 0;
            return mask;
        }

        // 制备量子公钥副本：分配 m 个量子比特并逐比特编码为 |f(k)>_i，
        // 返回本次分配的量子比特 id 列表（真实 id；旧实现恒返回硬编码的 0）。
        // 分配追加在后端现有 id 之后（get_num_qubits 语义 = 已存在的最高 id + 1），
        // 避免与共享后端上其他组件（rvdl / qchain 其他功能）的 id 冲突。
        static std::vector<size_t> prepare_public_state(qhal::IQuantumBackend *backend,
                                                        const PreparationSpec &spec)
        {
            std::vector<size_t> ids;
            const size_t m = spec.size();
            if (m == 0)
                return ids;
            const size_t base = backend->get_num_qubits();
            backend->allocate_qubits(base + m);
            ids.reserve(m);
            for (size_t i = 0; i < m; ++i)
            {
                ids.push_back(base + i);
                if (spec.value[i])
                    backend->apply_x(base + i);
                if (spec.basis[i])
                    backend->apply_h(base + i);
            }
            return ids;
        }

        // 签名：只在 S(message) 上揭示 |f(k)> 的比特值，其余位置不泄露。
        // 签名长度 = |S(message)|。签名者使用自己的古典秘密扩展出的制备描述
        // （kp.public_spec），即签名者制备量子公钥时同源的信息。
        static Bytes sign(const SigningKey &kp, const Bytes &message)
        {
            const size_t m = kp.public_spec.size();
            std::vector<bool> mask = reveal_mask(message, m);
            Bytes sig;
            sig.reserve(m / 2 + 1);
            for (size_t i = 0; i < m; ++i)
                if (mask[i])
                    sig.push_back(Byte(kp.public_spec.value[i] ? 1 : 0));
            return sig;
        }

        // 验证：在量子后端上制备 |f(k)> 的副本（验证方持有的量子公钥），
        // 对 S(message) 的每个位置按其制备基测量，与签名揭示的比特逐一比对；
        // 全部一致才接受（理想后端无噪声）。
        // 注意：验证只需要量子公钥的制备描述，不需要也不接受签名者的经典秘密
        //（旧接口传入含 secret 的 SigningKey，验证方能读到秘密，语义错误）。
        static bool verify(qhal::IQuantumBackend *backend, const PreparationSpec &public_spec,
                           const Bytes &message, const Bytes &signature)
        {
            const size_t m = public_spec.size();
            if (m == 0)
                return false;
            std::vector<bool> mask = reveal_mask(message, m);
            size_t revealed = 0;
            for (bool b : mask)
                if (b)
                    ++revealed;
            if (signature.size() != revealed)
            {
                std::cerr << "[qchain::qds] verify rejected: signature reveals " << signature.size()
                          << " bit(s), message expects " << revealed << ".\n";
                return false;
            }

            std::vector<size_t> ids = prepare_public_state(backend, public_spec);
            size_t idx = 0;
            bool ok = true;
            for (size_t i = 0; i < m; ++i)
            {
                if (!mask[i])
                    continue;
                // X 基测量：先施加 H 换到 Z 基（制备时也是 H 后编码）。
                if (public_spec.basis[i])
                    backend->apply_h(ids[i]);
                int outcome = backend->measure(ids[i]);
                if (outcome != static_cast<int>(signature[idx]))
                    ok = false;
                ++idx;
            }
            for (size_t id : ids)
                backend->release_qubit(id);
            return ok;
        }
    };

}
