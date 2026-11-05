#pragma once
//
// qchain :: 账本层 —— 量子链（QChain）
//
// 量子区块链本体：由哈希指针 + 量子纠缠链接串联的区块序列。
// 提供创世块、区块追加（结构 + 交易规则校验）、全链校验与分叉解决（最长/最大累积难度）。
//
// 交易规则校验（真实 UTXO 语义，不再只查「是否已被花费」）：
//   • 输入必须引用「存在且未花费」的输出（引用不存在的输出 = 凭空造币，拒绝）；
//   • 同一输出不得被重复花费（含同一区块内的双花）；
//   • 花费授权：输入引用的输出地址必须等于交易携带公钥的派生地址（与签名方案无关）；
//   • 金额守恒：非 coinbase 交易的输出总额 ≤ 输入总额（差额为手续费）；
//   • coinbase（无输入交易）：每个区块至多一笔，且必须是第一笔；
//   • 同一交易（txid）不得重复确认（防止 coinbase / 交易重放增发）。
// 签名验证由调用方通过 TxVerifier 回调注入（链层不对具体后量子签名方案做假设）。
//
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
        // 交易验证回调：由调用方（服务层）用具体签名方案注入。
        // 返回 false 的交易所在区块一律拒绝。空回调 = 跳过签名校验。
        using TxVerifier = std::function<bool(const SignedTransaction &)>;

        // UTXO 引用（前序交易 txid + 输出索引）。
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
            bool operator==(const OutPoint &) const = default; // 供 validate() 重放比对
        };

        // 已确认输出记录（含花费标记）。
        struct OutputRecord
        {
            Address address{};
            uint64_t amount = 0;
            bool spent = false;
            bool operator==(const OutputRecord &) const = default; // 供 validate() 重放比对
        };

    private:
        std::vector<Block> blocks;
        // 全部已确认输出：{txid,index} → 输出。spent 标记即双花状态。
        std::map<OutPoint, OutputRecord> outputs;
        // 已确认交易 id 集合（拒绝重复确认）。
        std::set<Hash256> confirmed_txids;

        // 在给定 UTXO 视图上应用区块：签名 / coinbase / UTXO / 守恒校验，
        // 通过后把花费标记与新输出写入视图（预演用；失败可能留下部分修改，调用方丢弃视图）。
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

                // 重复确认（含本区块内重复）→ 拒绝：否则 coinbase 可被重放增发。
                if (view_txids.count(txid) != 0)
                {
                    std::cerr << "[qchain] append rejected: duplicate transaction.\n";
                    return false;
                }

                if (tx.inputs.empty())
                {
                    // coinbase 规则：至多一笔，且必须是区块第一笔。
                    if (coinbase_seen || ti != 0)
                    {
                        std::cerr << "[qchain] append rejected: coinbase must be the first and only coinbase tx.\n";
                        return false;
                    }
                    coinbase_seen = true;
                    // coinbase 无输入（无花费授权），不要求签名；
                    // 铸币额度由经济策略（服务层 / 共识）约束，链层只保证唯一性。
                }
                else
                {
                    // 签名校验（若调用方提供了验证器；coinbase 无输入故无需）。
                    if (verifier && !verifier(st))
                    {
                        std::cerr << "[qchain] append rejected: transaction signature invalid.\n";
                        return false;
                    }
                }

                // 输入解析：存在性 / 双花 / 地址归属。
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

                // 金额守恒：非 coinbase 交易的输出总额 ≤ 输入总额（差额 = 手续费）。
                uint64_t out_sum = 0;
                for (const TxOutput &o : tx.outputs)
                    out_sum += o.amount;
                if (!tx.inputs.empty() && out_sum > in_sum)
                {
                    std::cerr << "[qchain] append rejected: value conservation violated (outputs > inputs).\n";
                    return false;
                }

                // 记录新输出。
                for (uint32_t oi = 0; oi < static_cast<uint32_t>(tx.outputs.size()); ++oi)
                    view[OutPoint{txid, oi}] =
                        OutputRecord{tx.outputs[oi].address, tx.outputs[oi].amount, false};
                view_txids.insert(txid);
            }
            return true;
        }

    public:
        QChain() = default;

        // 创建创世块。
        static Block make_genesis(uint64_t timestamp = 0)
        {
            Block g;
            g.header.version = 1;
            g.header.prev_hash = Hash256{}; // 全零
            g.header.timestamp = timestamp;
            g.header.nonce = 0;
            g.finalize();
            return g;
        }

        // 初始化为只有创世块的链（同时清空 UTXO 状态）。
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

        // 追加区块。校验：父哈希、默克尔根、交易签名（verifier 可选）、
        // coinbase 规则、UTXO 存在性/双花、金额守恒。
        bool append(const Block &block, const TxVerifier &verifier = {})
        {
            // 空链：直接追加创世块；若给定区块不是创世块，先补一个创世块（兼容旧行为）。
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
            // 校验默克尔根与交易一致。
            Hash256 computed = block.compute_merkle_root();
            if (!std::equal(computed.begin(), computed.end(), block.header.merkle_root.begin()))
            {
                std::cerr << "[qchain] append rejected: merkle root mismatch.\n";
                return false;
            }
            // 交易规则校验（预演：克隆当前视图，成功后一次性提交）。
            auto view = outputs;
            auto view_txids = confirmed_txids;
            if (!apply_block(block, verifier, view, view_txids))
                return false;
            outputs = std::move(view);
            confirmed_txids = std::move(view_txids);
            blocks.push_back(block);
            return true;
        }

        // 查询某输出是否已被确认花费。
        bool is_output_spent(const Hash256 &txid, uint32_t index) const
        {
            auto it = outputs.find(OutPoint{txid, index});
            return it != outputs.end() && it->second.spent;
        }

        // 查询某输出的完整记录（不存在返回 nullptr）。
        const OutputRecord *find_output(const Hash256 &txid, uint32_t index) const
        {
            auto it = outputs.find(OutPoint{txid, index});
            return it == outputs.end() ? nullptr : &it->second;
        }

        // 查询某地址当前可花费（未使用）的 UTXO 列表。
        std::vector<std::pair<OutPoint, uint64_t>> unspent_for(const Address &address) const
        {
            std::vector<std::pair<OutPoint, uint64_t>> res;
            for (const auto &[op, rec] : outputs)
                if (!rec.spent && rec.address == address)
                    res.emplace_back(op, rec.amount);
            return res;
        }

        // 全链校验：从创世块重放每个区块（哈希链 + 默克尔根 + 交易规则 + 签名），
        // 并比对各阶段 UTXO 视图与当前状态一致（可检出状态被旁路篡改的情况）。
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

        // 分叉解决：从候选中选择「高度更高，或同高时累积 nonce 更大」的链。
        // 返回获胜链的拷贝（供共识层替换本地视图）。
        static std::vector<Block> resolve_fork(const std::vector<Block> &a,
                                               const std::vector<Block> &b)
        {
            auto weight = [](const std::vector<Block> &c) -> uint64_t {
                uint64_t w = 0;
                for (const auto &blk : c)
                    w += blk.header.nonce + 1; // 简单累积工作量
                return w;
            };
            if (a.size() != b.size())
                return a.size() > b.size() ? a : b;
            return weight(a) >= weight(b) ? a : b;
        }

        // 用法币视图替换链（分叉解决后调用）：重放重建 UTXO 状态。
        // 失败（新链自身非法）返回 false 且保持原状；替换后建议再调用 validate() 复核。
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
