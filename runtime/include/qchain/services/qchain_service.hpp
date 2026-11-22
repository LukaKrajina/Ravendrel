#pragma once
#include "../common.hpp"
#include "../primitives/hash.hpp"
#include "../primitives/pqc.hpp"
#include "../primitives/qkd.hpp"
#include "../primitives/qds.hpp"
#include "../primitives/causality.hpp"
#include "../primitives/spacetime_cipher.hpp"
#include "../ledger/chain.hpp"
#include "../consensus/consensus.hpp"
#include "../token/quantum_coin.hpp"
#include "../../qhal/QM.hpp"
#include "../../qhal/QVM.hpp"
#include "../../qhal/Export.hpp"
#include <iostream>
#include <memory>
#include <unordered_map>
#include <chrono>
#include <mutex>

namespace qchain
{

    class RVDL_RT_API QChainService
    {
    public:
        struct Config
        {
            bool use_real_quantum_machine = false;
            qhal::HardwareModality hardware = qhal::HardwareModality::Superconducting;
            pqc::KemScheme kem_scheme = pqc::KemScheme::ML_KEM_768;
            pqc::SigScheme sig_scheme = pqc::SigScheme::ML_DSA;
            size_t merkle_height = 4;
            consensus::ConsensusProtocol consensus = consensus::ConsensusProtocol::QuantumPoW;
            unsigned pow_difficulty = 1;
            bool qkd_enabled = true;
        };

        struct Wallet
        {
            std::unique_ptr<pqc::ISignatureScheme> scheme;
            Bytes secret_key;
            Bytes public_key;
            ledger::Address address;
        };

    private:
        Config config;
        qhal::IQuantumBackend *backend = nullptr;
        std::unique_ptr<qhal::IQuantumBackend> owned_backend;
        qhal::QM *qm_ptr = nullptr;
        qhal::QVM *qvm_ptr = nullptr;

        ledger::QChain chain;
        std::vector<Wallet> wallets;
        std::unordered_map<ledger::Address, Bytes, Hash256Hasher> address_to_pk;

        std::vector<ledger::SignedTransaction> mempool;
        token::QuantumToken native_token;
        std::unique_ptr<pqc::IKem> kem;
        std::unique_ptr<pqc::ISignatureScheme> verify_scheme;
        Bytes network_key;
        causality::CausalityGuard causal_guard;

        mutable std::mutex mtx_;

        static uint64_t now_ms()
        {
            using namespace std::chrono;
            return static_cast<uint64_t>(
                duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count());
        }

        bool verify_transaction(const ledger::SignedTransaction &st) const
        {
            if (st.public_key.empty() || st.signature.empty())
                return false;
            return verify_scheme->verify(st.public_key,
                                         qchain::to_bytes(st.tx.txid()),
                                         st.signature);
        }

        std::set<std::pair<Hash256, uint32_t>> mempool_spends_locked() const
        {
            std::set<std::pair<Hash256, uint32_t>> s;
            for (const auto &st : mempool)
                for (const auto &in : st.tx.inputs)
                    s.insert({in.prev_txid, in.output_index});
            return s;
        }

    public:
        explicit QChainService(const Config &cfg)
            : config(cfg), native_token({"QuantumCoin", "QKC", 0})
        {
            kem = pqc::make_kem_scheme(config.kem_scheme);
            verify_scheme = pqc::make_sig_scheme(config.sig_scheme);
            if (cfg.use_real_quantum_machine)
            {
                auto qm = std::make_unique<qhal::QM>(cfg.hardware, 0);
                qm_ptr = qm.get();
                backend = qm.get();
                owned_backend = std::move(qm);
            }
            else
            {
                auto qvm = std::make_unique<qhal::QVM>();
                qvm_ptr = qvm.get();
                backend = qvm.get();
                owned_backend = std::move(qvm);
            }
            chain.init_genesis(0);
            std::cout << "[qchain] Quantum blockchain service online (backend: "
                      << (cfg.use_real_quantum_machine ? "QM" : "QVM") << ").\n";
        }

        QChainService(qhal::IQuantumBackend *external_backend, const Config &cfg)
            : config(cfg), native_token({"QuantumCoin", "QKC", 0}), backend(external_backend)
        {
            kem = pqc::make_kem_scheme(config.kem_scheme);
            verify_scheme = pqc::make_sig_scheme(config.sig_scheme);
            chain.init_genesis(0);
            std::cout << "[qchain] Quantum blockchain service attached to external backend.\n";
        }

        qhal::IQuantumBackend *get_backend() { return backend; }
        ledger::QChain &get_chain() { return chain; }
        const Config &get_config() const { return config; }
        token::QuantumToken &native_coin() { return native_token; }
        const std::vector<ledger::SignedTransaction> &get_mempool() const { return mempool; }
        size_t wallet_count() const { return wallets.size(); }
        Wallet &get_wallet(size_t index) { return wallets.at(index); }
        const Wallet &get_wallet(size_t index) const { return wallets.at(index); }

        Wallet &create_wallet()
        {
            std::lock_guard<std::mutex> lock(mtx_);
            wallets.emplace_back();
            Wallet &w = wallets.back();
            w.scheme = pqc::make_sig_scheme(config.sig_scheme);
            auto [sk, pk] = w.scheme->keygen();
            w.secret_key = std::move(sk);
            w.public_key = std::move(pk);
            w.address = crypto::derive_address(w.public_key);
            address_to_pk[w.address] = w.public_key;
            std::cout << "[qchain] wallet created (" << static_cast<int>(config.sig_scheme)
                      << "): " << to_hex(w.address) << "\n";
            return w;
        }

        qkd::QkdResult establish_qkd(size_t num_rounds = 64)
        {
            auto res = qkd::BB84::run(backend, num_rounds);
            network_key = res.shared_key;
            std::cout << "[qchain] QKD established (QBER=" << res.error_rate
                      << ", key=" << to_hex(network_key).substr(0, 16) << "...)\n";
            return res;
        }

        ledger::SignedTransaction mint(Wallet &to, uint64_t amount)
        {
            std::lock_guard<std::mutex> lock(mtx_);
            ledger::Transaction tx;
            ledger::TxOutput out{to.address, amount};
            tx.outputs.push_back(out);
            tx.timestamp = now_ms();

            ledger::SignedTransaction st;
            st.tx = std::move(tx);
            st.public_key = to.public_key;
            st.signature = to.scheme->sign(to.secret_key, qchain::to_bytes(st.tx.txid()));

            native_token.mint(to.address, amount);
            mempool.push_back(st);
            return st;
        }

        bool transfer(Wallet &from, const ledger::Address &to, uint64_t amount)
        {
            std::lock_guard<std::mutex> lock(mtx_);
            if (amount == 0)
            {
                std::cerr << "[qchain] transfer rejected: zero amount.\n";
                return false;
            }

            auto pending = mempool_spends_locked();
            std::vector<std::pair<ledger::QChain::OutPoint, uint64_t>> selected;
            uint64_t total = 0;
            for (const auto &[op, amt] : chain.unspent_for(from.address))
            {
                if (total >= amount)
                    break;
                if (pending.count({op.txid, op.index}) != 0)
                    continue;
                selected.emplace_back(op, amt);
                total += amt;
            }
            if (total < amount)
            {
                std::cerr << "[qchain] transfer rejected: insufficient confirmed UTXO for "
                          << to_hex(from.address) << " (available " << total << ", need " << amount
                          << "; mine pending blocks first).\n";
                return false;
            }

            ledger::Transaction tx;
            for (const auto &[op, amt] : selected)
            {
                ledger::TxInput in;
                in.prev_txid = op.txid;
                in.output_index = op.index;
                tx.inputs.push_back(in);
            }
            tx.outputs.push_back({to, amount});
            uint64_t change = total - amount;
            if (change > 0)
                tx.outputs.push_back({from.address, change});
            tx.timestamp = now_ms();

            ledger::SignedTransaction st;
            st.tx = std::move(tx);
            st.public_key = from.public_key;
            st.signature = from.scheme->sign(from.secret_key, qchain::to_bytes(st.tx.txid()));

            if (!native_token.transfer(from.address, to, amount))
                std::cerr << "[qchain] warning: accounting mirror out of sync with UTXO ledger.\n";
            mempool.push_back(st);
            std::cout << "[qchain] transfer staged: " << amount << " -> " << to_hex(to)
                      << " (" << st.tx.inputs.size() << " UTXO in, change " << change << ").\n";
            return true;
        }


        bool mint_to_address(const ledger::Address &addr, uint64_t amount)
        {
            native_token.mint(addr, amount);
            return true;
        }

        bool transfer_addresses(const ledger::Address &from, const ledger::Address &to, uint64_t amount)
        {
            return native_token.transfer(from, to, amount);
        }

        uint64_t balance_of_address(const ledger::Address &addr) const
        {
            return native_token.balance_of(addr);
        }

        bool mine_block()
        {
            std::lock_guard<std::mutex> lock(mtx_);

            for (const auto &st : mempool)
                if (!st.tx.inputs.empty() && !verify_transaction(st))
                {
                    std::cerr << "[qchain] mine abort: mempool contains a transaction with invalid signature.\n";
                    return false;
                }

            ledger::Block blk;
            blk.header.version = 1;
            blk.header.prev_hash = chain.tip().hash();
            blk.header.timestamp = now_ms();
            blk.header.qkd_entropy = network_key.empty() ? crypto::sha256(Bytes{}) : crypto::sha256(network_key);
            Bytes ent;
            ent.insert(ent.end(), blk.header.prev_hash.begin(), blk.header.prev_hash.end());
            ent.insert(ent.end(), blk.header.qkd_entropy.begin(), blk.header.qkd_entropy.end());
            blk.header.entanglement_root = crypto::sha256(ent);
            blk.header.causal_root = causal_guard.fingerprint();

            blk.transactions = mempool;
            blk.finalize();

            if (config.consensus == consensus::ConsensusProtocol::QuantumPoW)
            {
                if (!consensus::ConsensusEngine::quantum_pow(blk, config.pow_difficulty))
                {
                    std::cerr << "[qchain] mine abort: PoW difficulty not met within iteration budget.\n";
                    return false;
                }
            }

            if (!chain.append(blk, [this](const ledger::SignedTransaction &st) {
                    return verify_transaction(st);
                }))
            {
                std::cerr << "[qchain] mine abort: block append rejected; mempool preserved.\n";
                return false;
            }

            mempool.clear();
            std::cout << "[qchain] block #" << chain.height() - 1
                      << " mined (" << blk.transactions.size() << " tx).\n";

            causal_guard.step();
            if (!causal_guard.verify())
                std::cerr << "[qchain] CAUSALITY VIOLATION DETECTED (超时空干预)!\n";
            return true;
        }

        bool verify_chain() const
        {
            return chain.validate([this](const ledger::SignedTransaction &st) {
                return verify_transaction(st);
            });
        }

        bool verify_block_signatures(const ledger::Block &blk) const
        {
            for (const auto &st : blk.transactions)
                if (!st.tx.inputs.empty() && !verify_transaction(st))
                    return false;
            return true;
        }

        uint64_t balance_of(const ledger::Address &address) const
        {
            return native_token.balance_of(address);
        }

        consensus::QbaResult run_qdba(size_t n_parties, const std::set<size_t> &corrupt,
                                      size_t num_rounds)
        {
            return consensus::QuantumByzantineAgreement::run(backend, n_parties,
                                                             corrupt, num_rounds);
        }

        token::QuantumMoney::Banknote mint_quantum_coin(size_t n_qubits)
        {
            size_t start = backend->get_num_qubits();
            return token::QuantumMoney::mint(backend, n_qubits, start);
        }

        bool verify_quantum_coin(const token::QuantumMoney::Banknote &note)
        {
            return token::QuantumMoney::verify(backend, note);
        }
    };

}