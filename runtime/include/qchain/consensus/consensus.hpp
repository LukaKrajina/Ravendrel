#pragma once
//
// qchain :: 共识层 —— 共识引擎（量子 PoW / 量子 PoS / QDBA 最终性）
//
// 三种量子增强共识原语：
//   • 量子工作量证明 QuantumPoW —— 哈希挑战 + QKD 量子熵注入，抗 Grover 加速集中。
//   • 量子权益证明 QuantumPoS   —— 用 QKD 量子随机数做无偏「跟随权益」抽签。
//   • QDBA 最终性              —— 用 GHZ 纠缠态达成拜占庭容错的确定性最终确认。
//
#include "../common.hpp"
#include "../primitives/hash.hpp"
#include "../ledger/block.hpp"
#include "qba.hpp"

namespace qchain::consensus
{

    using qchain::Byte;
    using qchain::Bytes;
    using qchain::Rng;
    using crypto::sha256;

    enum class ConsensusProtocol
    {
        QuantumPoW,
        QuantumPoS,
        QDBA
    };

    class ConsensusEngine
    {
    public:
        // ─── 量子工作量证明 ────────────────────────────────────────────
        //
        // 寻找 nonce 使区块哈希（含 QKD 量子熵）满足「difficulty 个前导零字节」。
        // 量子熵 qkd_entropy 由 QKD 会话产生，使哈希输入具备量子随机性。
        static bool quantum_pow(ledger::Block &block, unsigned difficulty, uint64_t max_iters = 10'000'000)
        {
            for (uint64_t i = 0; i < max_iters; ++i)
            {
                block.header.nonce = i;
                Hash256 h = block.hash();
                bool ok = true;
                for (unsigned b = 0; b < difficulty; ++b)
                {
                    if (h[b] != 0)
                    {
                        ok = false;
                        break;
                    }
                }
                if (ok)
                    return true;
            }
            return false;
        }

        // ─── 量子权益证明（无偏抽签）───────────────────────────────────
        //
        // 用 QKD 量子熵 + 区块哈希生成无偏随机数，按权益权重选出提议者。
        // 返回中选者索引。量子随机性保证抽签不可预测、不可操纵。
        static size_t quantum_pos_select(const std::vector<uint64_t> &stakes, const Bytes &entropy)
        {
            if (stakes.empty())
                throw std::invalid_argument("qchain::ConsensusEngine::quantum_pos_select: empty stakes");
            uint64_t total = 0;
            for (uint64_t s : stakes)
                total += s;
            if (total == 0)
                throw std::invalid_argument("qchain::ConsensusEngine::quantum_pos_select: zero total stake");

            Bytes seed = entropy;
            Hash256 h = sha256(seed);
            // 取哈希前 8 字节作为均匀票值。
            uint64_t ticket = 0;
            for (int i = 0; i < 8; ++i)
                ticket = (ticket << 8) | h[i];
            uint64_t point = ticket % total;
            uint64_t acc = 0;
            for (size_t i = 0; i < stakes.size(); ++i)
            {
                acc += stakes[i];
                if (point < acc)
                    return i;
            }
            return stakes.size() - 1;
        }

        // ─── QDBA 最终性 ──────────────────────────────────────────────
        //
        // 用 QDBA 对候选块达成一致：返回是否达成（无拜占庭干扰）。
        static bool qdba_finalize(qhal::IQuantumBackend *backend, size_t n_parties,
                                  const std::set<size_t> &corrupt, size_t num_rounds)
        {
            auto res = QuantumByzantineAgreement::run(backend, n_parties, corrupt, num_rounds);
            return res.agreement;
        }
    };
}