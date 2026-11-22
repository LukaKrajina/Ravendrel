#pragma once
#include "../common.hpp"
#include "hash.hpp"
#include "../../qhal/IQuantumBackend.hpp"
#include <iostream>

namespace qchain::qds
{

    using qchain::Byte;
    using qchain::Bytes;
    using qchain::Rng;
    using crypto::sha256;

    struct PreparationSpec
    {
        std::vector<bool> basis;
        std::vector<bool> value;
        size_t size() const { return basis.size(); }
    };

    class QuantumOneWayFunction
    {
    public:
        static Bytes stream(const Bytes &key, size_t needed_bits)
        {
            Bytes out;
            for (uint64_t c = 0; out.size() * 8 < needed_bits; ++c)
            {
                Bytes block = key;
                for (int i = 0; i < 8; ++i)
                    block.push_back(Byte((c >> (8 * i)) & 0xFF));
                Hash256 h = sha256(block);
                out.insert(out.end(), h.begin(), h.end());
            }
            return out;
        }

        static PreparationSpec evaluate(const Bytes &key, size_t m)
        {
            Bytes s = stream(key, 2 * m);
            PreparationSpec spec;
            spec.basis.resize(m);
            spec.value.resize(m);
            for (size_t i = 0; i < m; ++i)
            {
                spec.basis[i] = ((s[(2 * i) / 8] >> ((2 * i) % 8)) & 1) != 0;
                spec.value[i] = ((s[(2 * i + 1) / 8] >> ((2 * i + 1) % 8)) & 1) != 0;
            }
            return spec;
        }
    };

    class QuantumDigitalSignature
    {
    public:
        struct SigningKey
        {
            Bytes secret;
            PreparationSpec public_spec;
        };

        static SigningKey keygen(size_t m)
        {
            if (m == 0)
                throw std::invalid_argument("qchain::qds: keygen requires m > 0");
            Rng rng;
            SigningKey kp;
            kp.secret = rng.random_bytes(32);
            kp.public_spec = QuantumOneWayFunction::evaluate(kp.secret, m);
            return kp;
        }

        static std::vector<bool> reveal_mask(const Bytes &message, size_t m)
        {
            Hash256 dg = sha256(message);
            Bytes key(dg.begin(), dg.end());
            Bytes mask_stream = QuantumOneWayFunction::stream(key, m);
            std::vector<bool> mask(m);
            for (size_t i = 0; i < m; ++i)
                mask[i] = ((mask_stream[i / 8] >> (i % 8)) & 1) != 0;
            return mask;
        }

        static std::vector<size_t> prepare_public_state(qhal::IQuantumBackend *backend,
                                                        const PreparationSpec &spec)
        {
            std::vector<size_t> ids;
            const size_t m = spec.size();
            if (m == 0)
                return ids;
            const size_t base = backend->get_num_qubits();
            backend->allocate_qubits(base + m);
            ids.reserve(m);
            for (size_t i = 0; i < m; ++i)
            {
                ids.push_back(base + i);
                if (spec.value[i])
                    backend->apply_x(base + i);
                if (spec.basis[i])
                    backend->apply_h(base + i);
            }
            return ids;
        }

        static Bytes sign(const SigningKey &kp, const Bytes &message)
        {
            const size_t m = kp.public_spec.size();
            std::vector<bool> mask = reveal_mask(message, m);
            Bytes sig;
            sig.reserve(m / 2 + 1);
            for (size_t i = 0; i < m; ++i)
                if (mask[i])
                    sig.push_back(Byte(kp.public_spec.value[i] ? 1 : 0));
            return sig;
        }

        static bool verify(qhal::IQuantumBackend *backend, const PreparationSpec &public_spec,
                           const Bytes &message, const Bytes &signature)
        {
            const size_t m = public_spec.size();
            if (m == 0)
                return false;
            std::vector<bool> mask = reveal_mask(message, m);
            size_t revealed = 0;
            for (bool b : mask)
                if (b)
                    ++revealed;
            if (signature.size() != revealed)
            {
                std::cerr << "[qchain::qds] verify rejected: signature reveals " << signature.size()
                          << " bit(s), message expects " << revealed << ".\n";
                return false;
            }

            std::vector<size_t> ids = prepare_public_state(backend, public_spec);
            size_t idx = 0;
            bool ok = true;
            for (size_t i = 0; i < m; ++i)
            {
                if (!mask[i])
                    continue;
                if (public_spec.basis[i])
                    backend->apply_h(ids[i]);
                int outcome = backend->measure(ids[i]);
                if (outcome != static_cast<int>(signature[idx]))
                    ok = false;
                ++idx;
            }
            for (size_t id : ids)
                backend->release_qubit(id);
            return ok;
        }
    };

}