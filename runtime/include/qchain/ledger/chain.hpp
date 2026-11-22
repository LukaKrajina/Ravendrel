#pragma once
#include "../common.hpp"
#include "../primitives/hash.hpp"
#include "block.hpp"
#include <algorithm>
#include <iostream>
#include <functional>
#include <map>
#include <set>
#include <utility>
#include <vector>

namespace qchain::ledger
{

    using qchain::Byte;
    using qchain::Bytes;
    using qchain::Hash256;
    using crypto::sha256;

    class QChain
    {
    public:
        using TxVerifier = std::function<bool(const SignedTransaction &)>;

        struct OutPoint
        {
            Hash256 txid{};
            uint32_t index = 0;
            bool operator<(const OutPoint &o) const
            {
                if (txid != o.txid)
                    return txid < o.txid;
                return index < o.index;
            }
            bool operator==(const OutPoint &) const = default;
        };

        struct OutputRecord
        {
            Address address{};
            uint64_t amount = 0;
            bool spent = false;
            bool operator==(const OutputRecord &) const = default;
        };

    private:
        std::vector<Block> blocks;
        std::map<OutPoint, OutputRecord> outputs;
        std::set<Hash256> confirmed_txids;

        bool apply_block(const Block &block, const TxVerifier &verifier,
                         std::map<OutPoint, OutputRecord> &view,
                         std::set<Hash256> &view_txids) const
        {
            bool coinbase_seen = false;
            for (size_t ti = 0; ti < block.transactions.size(); ++ti)
            {
                const SignedTransaction &st = block.transactions[ti];
                const Transaction &tx = st.tx;
                const Hash256 txid = tx.txid();

                if (view_txids.count(txid) != 0)
                {
                    std::cerr << "[qchain] append rejected: duplicate transaction.\n";
                    return false;
                }

                if (tx.inputs.empty())
                {
                    if (coinbase_seen || ti != 0)
                    {
                        std::cerr << "[qchain] append rejected: coinbase must be the first and only coinbase tx.\n";
                        return false;
                    }
                    coinbase_seen = true;
                }
                else
                {
                    if (verifier && !verifier(st))
                    {
                        std::cerr << "[qchain] append rejected: transaction signature invalid.\n";
                        return false;
                    }
                }

                uint64_t in_sum = 0;
                const Address spender = crypto::derive_address(st.public_key);
                for (const TxInput &in : tx.inputs)
                {
                    OutPoint op{in.prev_txid, in.output_index};
                    auto it = view.find(op);
                    if (it == view.end())
                    {
                        std::cerr << "[qchain] append rejected: input references non-existent output.\n";
                        return false;
                    }
                    if (it->second.spent)
                    {
                        std::cerr << "[qchain] append rejected: double-spend detected.\n";
                        return false;
                    }
                    if (!(it->second.address == spender))
                    {
                        std::cerr << "[qchain] append rejected: input not owned by transaction signer.\n";
                        return false;
                    }
                    it->second.spent = true;
                    in_sum += it->second.amount;
                }

                uint64_t out_sum = 0;
                for (const TxOutput &o : tx.outputs)
                    out_sum += o.amount;
                if (!tx.inputs.empty() && out_sum > in_sum)
                {
                    std::cerr << "[qchain] append rejected: value conservation violated (outputs > inputs).\n";
                    return false;
                }

                for (uint32_t oi = 0; oi < static_cast<uint32_t>(tx.outputs.size()); ++oi)
                    view[OutPoint{txid, oi}] =
                        OutputRecord{tx.outputs[oi].address, tx.outputs[oi].amount, false};
                view_txids.insert(txid);
            }
            return true;
        }

    public:
        QChain() = default;

        static Block make_genesis(uint64_t timestamp = 0)
        {
            Block g;
            g.header.version = 1;
            g.header.prev_hash = Hash256{};
            g.header.timestamp = timestamp;
            g.header.nonce = 0;
            g.finalize();
            return g;
        }

        void init_genesis(uint64_t timestamp = 0)
        {
            blocks.clear();
            outputs.clear();
            confirmed_txids.clear();
            blocks.push_back(make_genesis(timestamp));
        }

        size_t height() const { return blocks.size(); }
        const Block &at(size_t i) const { return blocks.at(i); }
        const Block &tip() const { return blocks.back(); }
        const std::vector<Block> &get_blocks() const { return blocks; }

        bool append(const Block &block, const TxVerifier &verifier = {})
        {
            if (blocks.empty() && !(block.header.prev_hash == Hash256{}))
                blocks.push_back(make_genesis(0));

            if (!blocks.empty())
            {
                const Block &prev = blocks.back();
                if (!std::equal(block.header.prev_hash.begin(), block.header.prev_hash.end(),
                                prev.hash().begin()))
                {
                    std::cerr << "[qchain] append rejected: prev_hash mismatch.\n";
                    return false;
                }
            }
            Hash256 computed = block.compute_merkle_root();
            if (!std::equal(computed.begin(), computed.end(), block.header.merkle_root.begin()))
            {
                std::cerr << "[qchain] append rejected: merkle root mismatch.\n";
                return false;
            }
            auto view = outputs;
            auto view_txids = confirmed_txids;
            if (!apply_block(block, verifier, view, view_txids))
                return false;
            outputs = std::move(view);
            confirmed_txids = std::move(view_txids);
            blocks.push_back(block);
            return true;
        }

        bool is_output_spent(const Hash256 &txid, uint32_t index) const
        {
            auto it = outputs.find(OutPoint{txid, index});
            return it != outputs.end() && it->second.spent;
        }

        const OutputRecord *find_output(const Hash256 &txid, uint32_t index) const
        {
            auto it = outputs.find(OutPoint{txid, index});
            return it == outputs.end() ? nullptr : &it->second;
        }

        std::vector<std::pair<OutPoint, uint64_t>> unspent_for(const Address &address) const
        {
            std::vector<std::pair<OutPoint, uint64_t>> res;
            for (const auto &[op, rec] : outputs)
                if (!rec.spent && rec.address == address)
                    res.emplace_back(op, rec.amount);
            return res;
        }

        bool validate(const TxVerifier &verifier = {}) const
        {
            if (blocks.empty())
                return false;
            QChain replay;
            for (const Block &b : blocks)
                if (!replay.append(b, verifier))
                    return false;
            return replay.outputs == outputs && replay.confirmed_txids == confirmed_txids;
        }

        static std::vector<Block> resolve_fork(const std::vector<Block> &a,
                                               const std::vector<Block> &b)
        {
            auto weight = [](const std::vector<Block> &c) -> uint64_t {
                uint64_t w = 0;
                for (const auto &blk : c)
                    w += blk.header.nonce + 1;
                return w;
            };
            if (a.size() != b.size())
                return a.size() > b.size() ? a : b;
            return weight(a) >= weight(b) ? a : b;
        }

        bool replace(const std::vector<Block> &new_blocks)
        {
            QChain fresh;
            for (const Block &b : new_blocks)
                if (!fresh.append(b))
                    return false;
            blocks = std::move(fresh.blocks);
            outputs = std::move(fresh.outputs);
            confirmed_txids = std::move(fresh.confirmed_txids);
            return true;
        }
    };
}