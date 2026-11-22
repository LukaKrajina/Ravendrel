#pragma once
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
        Hash256 prev_hash{};
        Hash256 merkle_root{};
        uint64_t timestamp = 0;
        uint64_t nonce = 0;
        Hash256 qkd_entropy{};
        Hash256 entanglement_root{};
        Hash256 causal_root{};

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

        Hash256 hash() const { return sha256(header.serialize()); }

        void finalize() { header.merkle_root = compute_merkle_root(); }
    };
}