#pragma once
//
// qchain :: 量子货币与量子代币（量子币种基础）
//
// 两条可编程货币路径：
//   • QuantumMoney —— Wiesner 量子货币：序列号 + 多量子比特态，凭「不可克隆定理」
//     保证不可伪造、不可复制（信息论安全）。适用于数字现金。
//   • QuantumToken —— 可编程量子代币标准（类 ERC-20 的量子原生版本）：记账式余额、
//     铸币、转账，配合后量子签名。适用于各种量子币种 / 通证发行。
//
#include "../common.hpp"
#include "../primitives/hash.hpp"
#include "../../qhal/IQuantumBackend.hpp"
#include <string>
#include <unordered_map>

namespace qchain::token
{

    using qchain::Byte;
    using qchain::Bytes;
    using qchain::Rng;
    using qchain::Hash256;
    using crypto::sha256;

    // ══════════════════════════════════════════════════════════════════
    // QuantumMoney —— Wiesner 量子货币
    // ══════════════════════════════════════════════════════════════════
    class QuantumMoney
    {
    public:
        // 量子钞票：序列号 + 每个量子比特的 (基, 值) 制备描述。
        struct Banknote
        {
            Bytes serial;
            std::vector<bool> basis; // false=Z, true=X
            std::vector<bool> value;
            size_t start_id = 0;     // 钞票量子比特在后端上的起始 id（支持共享后端多次铸币）
            size_t size() const { return basis.size(); }
        };

        // 铸币：生成序列号 + 随机 (基, 值)，并在量子后端上制备对应量子态。
        // start_id 指定钞票量子比特在后端上的起始 id（供共享后端复用）。
        // 返回钞票（含制备描述 + 起始 id，供银行/验证方持有）。量子态留存在后端。
        static Banknote mint(qhal::IQuantumBackend *backend, size_t n_qubits, size_t start_id = 0)
        {
            Rng rng;
            Banknote note;
            note.serial = rng.random_bytes(16);
            note.start_id = start_id;
            note.basis.resize(n_qubits);
            note.value.resize(n_qubits);

            backend->allocate_qubits(start_id + n_qubits);
            for (size_t i = 0; i < n_qubits; ++i)
            {
                note.basis[i] = rng.random_bit();
                note.value[i] = rng.random_bit();
                if (note.value[i])
                    backend->apply_x(start_id + i);
                if (note.basis[i])
                    backend->apply_h(start_id + i);
            }
            return note;
        }

        // 验证流通中的钞票：按记录基测量，与记录值比对（使用 note.start_id）。
        // 由于不可克隆，任何复制尝试都会在测量中引入错误而被拒绝。
        static bool verify(qhal::IQuantumBackend *backend, const Banknote &note)
        {
            const size_t n = note.size();
            const size_t base = note.start_id;
            size_t accept = 0;
            for (size_t i = 0; i < n; ++i)
            {
                if (note.basis[i])
                    backend->apply_h(base + i);
                int outcome = backend->measure(base + i);
                if (outcome == (note.value[i] ? 1 : 0))
                    ++accept;
            }
            // 允许统计容差（理想后端无噪声时严格一致）。
            double ratio = n == 0 ? 0.0 : double(accept) / double(n);
            return ratio >= 0.95;
        }

        // 释放钞票占用的量子比特。
        static void destroy(qhal::IQuantumBackend *backend, const Banknote &note)
        {
            const size_t base = note.start_id;
            for (size_t i = 0; i < note.size(); ++i)
                backend->release_qubit(base + i);
        }
    };

    // ══════════════════════════════════════════════════════════════════
    // QuantumToken —— 可编程量子代币标准
    // ══════════════════════════════════════════════════════════════════
    class QuantumToken
    {
    public:
        struct Metadata
        {
            std::string name;
            std::string symbol;
            uint64_t total_supply = 0;
        };

    private:
        Metadata meta;
        std::unordered_map<Hash256, uint64_t, qchain::Hash256Hasher> balances;
        uint64_t minted = 0;

    public:
        explicit QuantumToken(Metadata m) : meta(std::move(m)) {}

        const Metadata &metadata() const { return meta; }

        // 铸币：向指定地址增发并计入总供应。
        void mint(const Hash256 &address, uint64_t amount)
        {
            balances[address] += amount;
            minted += amount;
            meta.total_supply = minted;
        }

        // 转账：原子扣减/增加。余额不足返回 false。
        bool transfer(const Hash256 &from, const Hash256 &to, uint64_t amount)
        {
            auto it = balances.find(from);
            if (it == balances.end() || it->second < amount)
                return false;
            it->second -= amount;
            balances[to] += amount;
            return true;
        }

        uint64_t balance_of(const Hash256 &address) const
        {
            auto it = balances.find(address);
            return it == balances.end() ? 0 : it->second;
        }
    };

}