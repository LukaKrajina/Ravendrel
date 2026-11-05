#pragma once
//
// qchain :: 账本层 —— 交易（Transaction）
//
// UTXO 模型：
//   • 地址由（后量子）公钥哈希派生，抵抗 Shor 算法逆推。
//   • 交易通过 ISignatureScheme（ML-DSA / SLH-DSA / Lamport / Merkle 等）签名。
//   • txid 为规范序列化的 SHA-256 摘要。
//
#include "../common.hpp"
#include "../primitives/hash.hpp"
#include "../primitives/pqc.hpp"

namespace qchain::ledger
{

    using qchain::Byte;
    using qchain::Bytes;
    using qchain::Hash256;
    using crypto::sha256;

    using Address = Hash256; // 量子安全地址

    // ─── 交易输入 / 输出 ─────────────────────────────────────────────
    struct TxInput
    {
        Hash256 prev_txid;         // 引用前序交易
        uint32_t output_index = 0; // 引用其第几个输出
        Bytes unlock_signature;    // 花费证明（对交易体的 PQC 签名）
    };

    struct TxOutput
    {
        Address address;      // 收款地址
        uint64_t amount = 0;  // 金额（最小单位）
    };

    // ─── 交易 ─────────────────────────────────────────────────────────
    struct Transaction
    {
        std::vector<TxInput> inputs;
        std::vector<TxOutput> outputs;
        uint64_t timestamp = 0;

        // 规范序列化（用于哈希 / 签名）。
        Bytes serialize() const
        {
            Bytes out;
            auto push32 = [&](const Hash256 &h) {
                out.insert(out.end(), h.begin(), h.end());
            };
            auto push32le = [&](uint32_t v) {
                for (int i = 0; i < 4; ++i)
                    out.push_back(Byte((v >> (8 * i)) & 0xFF));
            };
            auto push64le = [&](uint64_t v) {
                for (int i = 0; i < 8; ++i)
                    out.push_back(Byte((v >> (8 * i)) & 0xFF));
            };
            push32le(static_cast<uint32_t>(inputs.size()));
            for (const auto &in : inputs)
            {
                push32(in.prev_txid);
                push32le(in.output_index);
            }
            push32le(static_cast<uint32_t>(outputs.size()));
            for (const auto &o : outputs)
            {
                push32(o.address);
                push64le(o.amount);
            }
            push64le(timestamp);
            return out;
        }

        Hash256 txid() const { return sha256(serialize()); }
    };

    // ─── 带签名的交易（自包含：公钥随交易，验证不依赖外部映射）───────
    struct SignedTransaction
    {
        Transaction tx;
        Bytes signature;   // 对 tx.txid() 的 PQC 签名
        Bytes public_key;  // 签名者公钥（链上自包含）

        // 用给定后量子签名方案对交易签名（公钥随交易携带，供链上自包含验证）。
        void sign(const pqc::ISignatureScheme &scheme, const Bytes &secret_key, const Bytes &pk)
        {
            public_key = pk;
            signature = scheme.sign(secret_key, qchain::to_bytes(tx.txid()));
        }

        // 校验签名（自包含，无需外部公钥映射）。
        bool verify(const pqc::ISignatureScheme &scheme) const
        {
            if (public_key.empty() || signature.empty())
                return false;
            return scheme.verify(public_key, qchain::to_bytes(tx.txid()), signature);
        }
    };

}