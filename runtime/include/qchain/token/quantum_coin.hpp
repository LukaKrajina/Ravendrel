#pragma once
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

    class QuantumMoney
    {
    public:
        struct Banknote
        {
            Bytes serial;
            std::vector<bool> basis;
            std::vector<bool> value;
            size_t start_id = 0;
            size_t size() const { return basis.size(); }
        };

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
            double ratio = n == 0 ? 0.0 : double(accept) / double(n);
            return ratio >= 0.95;
        }

        static void destroy(qhal::IQuantumBackend *backend, const Banknote &note)
        {
            const size_t base = note.start_id;
            for (size_t i = 0; i < note.size(); ++i)
                backend->release_qubit(base + i);
        }
    };

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

        void mint(const Hash256 &address, uint64_t amount)
        {
            balances[address] += amount;
            minted += amount;
            meta.total_supply = minted;
        }

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