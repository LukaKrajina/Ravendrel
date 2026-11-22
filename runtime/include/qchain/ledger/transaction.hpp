#pragma once
#include "../common.hpp"
#include "../primitives/hash.hpp"
#include "../primitives/pqc.hpp"

namespace qchain::ledger
{

    using qchain::Byte;
    using qchain::Bytes;
    using qchain::Hash256;
    using crypto::sha256;

    using Address = Hash256;

    struct TxInput
    {
        Hash256 prev_txid;
        uint32_t output_index = 0;
        Bytes unlock_signature;
    };

    struct TxOutput
    {
        Address address;
        uint64_t amount = 0;
    };

    struct Transaction
    {
        std::vector<TxInput> inputs;
        std::vector<TxOutput> outputs;
        uint64_t timestamp = 0;

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

    struct SignedTransaction
    {
        Transaction tx;
        Bytes signature;
        Bytes public_key;

        void sign(const pqc::ISignatureScheme &scheme, const Bytes &secret_key, const Bytes &pk)
        {
            public_key = pk;
            signature = scheme.sign(secret_key, qchain::to_bytes(tx.txid()));
        }

        bool verify(const pqc::ISignatureScheme &scheme) const
        {
            if (public_key.empty() || signature.empty())
                return false;
            return scheme.verify(public_key, qchain::to_bytes(tx.txid()), signature);
        }
    };

}