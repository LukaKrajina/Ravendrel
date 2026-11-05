#pragma once
//
// qchain :: 账本层 —— 区块（Block）
//
// 区块头采用「哈希链接 + 量子纠缠链接 + 因果链接」三重防篡改：
//   • prev_hash     —— 经典抗量子哈希指针（SHA-256）。
//   • merkle_root   —— 交易默克尔根（支持 SPV 轻节点）。
//   • qkd_entropy   —— 由 QKD 会话产生的量子熵（量子随机性注入，增强不可预测性）。
//   • entanglement_root —— 量子纠缠链接标记（GHZ 态指纹），篡改区块会破坏纠缠一致性。
//   • causal_root   —— 快子场因果指纹（防超时空：篡改会破坏能量守恒/额外维对称）。
//
#include "../common.hpp"
#include "../primitives/hash.hpp"
#include "transaction.hpp"

namespace qchain::ledger
{

    using qchain::Byte;
    using qchain::Bytes;
    using qchain::Hash256;
    using crypto::sha256;
    using crypto::MerkleTree;

    struct BlockHeader
    {
        uint32_t version = 1;
        Hash256 prev_hash{};        // 前一区块哈希（创世块为全零）
        Hash256 merkle_root{};      // 交易默克尔根
        uint64_t timestamp = 0;
        uint64_t nonce = 0;         // 共识随机数（量子 PoW/PoS）
        Hash256 qkd_entropy{};      // QKD 量子熵（量子随机性注入）
        Hash256 entanglement_root{}; // 量子纠缠链接指纹（GHZ 态）
        Hash256 causal_root{};       // 快子场因果指纹（防超时空）

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
            push32le(version);
            push32(prev_hash);
            push32(merkle_root);
            push64le(timestamp);
            push64le(nonce);
            push32(qkd_entropy);
            push32(entanglement_root);
            push32(causal_root);
            return out;
        }
    };

    struct Block
    {
        BlockHeader header;
        std::vector<SignedTransaction> transactions;

        // 计算交易默克尔根。
        Hash256 compute_merkle_root() const
        {
            if (transactions.empty())
                return Hash256{};
            std::vector<Hash256> leaves;
            leaves.reserve(transactions.size());
            for (const auto &st : transactions)
                leaves.push_back(st.tx.txid());
            return MerkleTree(leaves).root();
        }

        // 区块哈希 = SHA-256(序列化头)。
        Hash256 hash() const { return sha256(header.serialize()); }

        // 创建区块时填充 merkle_root 并返回。
        void finalize() { header.merkle_root = compute_merkle_root(); }
    };
}