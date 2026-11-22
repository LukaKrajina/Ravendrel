#pragma once
#include "../common.hpp"
#include "hash.hpp"
#include "../../qhal/IQuantumBackend.hpp"
#include <algorithm>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace qchain::qkd
{

    using qchain::Byte;
    using qchain::Bytes;
    using qchain::Rng;
    using crypto::sha256;

    enum class QkdProtocol
    {
        BB84,
        E91,
        DecoyBB84
    };

    struct QkdResult
    {
        Bytes sifted_key;
        Bytes shared_key;
        double error_rate = 0.0;
        double key_rate = 0.0;
        size_t raw_bits = 0;
        size_t sifted_bits = 0;
    };

    namespace detail
    {
        inline void reset_qubit(qhal::IQuantumBackend *backend, size_t q)
        {
            if (backend->measure(q) == 1)
                backend->apply_x(q);
        }

        inline int measure_angle(qhal::IQuantumBackend *backend, size_t q, double theta)
        {
            backend->apply_rz(q, -M_PI / 2.0);
            backend->apply_h(q);
            backend->apply_rz(q, -theta);
            backend->apply_h(q);
            backend->apply_rz(q, M_PI / 2.0);
            return backend->measure(q);
        }
    }

    class BB84
    {
    public:
        static QkdResult run(qhal::IQuantumBackend *backend, size_t num_rounds, double noise = 0.0)
        {
            Rng rng;
            const size_t q = backend->get_num_qubits();
            backend->allocate_qubits(q + 1);

            std::vector<bool> alice_bits(num_rounds), alice_basis(num_rounds), bob_basis(num_rounds);
            std::vector<int> outcomes(num_rounds);

            for (size_t i = 0; i < num_rounds; ++i)
            {
                alice_bits[i] = rng.random_bit();
                alice_basis[i] = rng.random_bit();
                bob_basis[i] = rng.random_bit();

                detail::reset_qubit(backend, q);
                if (alice_bits[i])
                    backend->apply_x(q);
                if (alice_basis[i])
                    backend->apply_h(q);

                if (bob_basis[i])
                    backend->apply_h(q);
                outcomes[i] = backend->measure(q);
            }
            backend->release_qubit(q);

            Bytes sifted;
            for (size_t i = 0; i < num_rounds; ++i)
                if (alice_basis[i] == bob_basis[i])
                    sifted.push_back(Byte(alice_bits[i] ? 1 : 0));

            Bytes received = sifted;
            if (noise > 0.0)
                for (auto &b : received)
                    if (rng.uniform_real() < noise)
                        b ^= 1;

            size_t errors = 0;
            for (size_t i = 0; i < sifted.size(); ++i)
                if (sifted[i] != received[i])
                    ++errors;
            double qber = sifted.empty() ? 0.0 : double(errors) / double(sifted.size());

            QkdResult res;
            res.raw_bits = num_rounds;
            res.sifted_bits = sifted.size();
            res.sifted_key = received;
            res.error_rate = qber;
            res.key_rate = num_rounds == 0 ? 0.0 : double(sifted.size()) / double(num_rounds);
            Bytes material = received;
            for (size_t i = 0; i < num_rounds; ++i)
                material.push_back(Byte(alice_basis[i] ? 1 : 0));
            Hash256 sk = sha256(material);
            res.shared_key = Bytes(sk.begin(), sk.end());
            return res;
        }
    };

    class E91
    {
    public:
        static QkdResult run(qhal::IQuantumBackend *backend, size_t num_rounds)
        {
            Rng rng;
            const size_t qa = backend->get_num_qubits();
            backend->allocate_qubits(qa + 2);
            const size_t qb = qa + 1;
            std::vector<bool> alice_basis(num_rounds), bob_basis(num_rounds);
            std::vector<int> a_out(num_rounds), b_out(num_rounds);

            for (size_t i = 0; i < num_rounds; ++i)
            {
                detail::reset_qubit(backend, qa);
                detail::reset_qubit(backend, qb);
                backend->apply_h(qa);
                backend->apply_cnot(qa, qb);

                alice_basis[i] = rng.random_bit();
                bob_basis[i] = rng.random_bit();

                if (alice_basis[i])
                    backend->apply_h(qa);
                if (bob_basis[i])
                    backend->apply_h(qb);

                a_out[i] = backend->measure(qa);
                b_out[i] = backend->measure(qb);
            }
            backend->release_qubit(qa);
            backend->release_qubit(qb);

            Bytes sifted;
            size_t agree = 0;
            for (size_t i = 0; i < num_rounds; ++i)
            {
                if (alice_basis[i] == bob_basis[i])
                {
                    sifted.push_back(Byte(a_out[i] & 1));
                    if (a_out[i] == b_out[i])
                        ++agree;
                }
            }

            QkdResult res;
            res.raw_bits = num_rounds;
            res.sifted_bits = sifted.size();
            res.sifted_key = sifted;
            res.error_rate = sifted.empty() ? 0.0 : 1.0 - double(agree) / double(sifted.size());
            res.key_rate = num_rounds == 0 ? 0.0 : double(sifted.size()) / double(num_rounds);
            Hash256 sk = sha256(sifted);
            res.shared_key = Bytes(sk.begin(), sk.end());
            return res;
        }

        static double chsh_estimate(qhal::IQuantumBackend *backend, size_t num_rounds)
        {
            const size_t qa = backend->get_num_qubits();
            backend->allocate_qubits(qa + 2);
            const size_t qb = qa + 1;
            auto corr = [&](double a_ang, double b_ang) -> double {
                int same = 0, total = 0;
                for (size_t i = 0; i < num_rounds; ++i)
                {
                    detail::reset_qubit(backend, qa);
                    detail::reset_qubit(backend, qb);
                    backend->apply_h(qa);
                    backend->apply_cnot(qa, qb);
                    int a = detail::measure_angle(backend, qa, a_ang);
                    int b = detail::measure_angle(backend, qb, b_ang);
                    ++total;
                    if (a == b)
                        ++same;
                }
                return 2.0 * double(same) / double(total) - 1.0;
            };
            const double a0 = 0.0, a1 = M_PI / 2.0;
            const double b0 = M_PI / 4.0, b1 = -M_PI / 4.0;
            double E00 = corr(a0, b0);
            double E01 = corr(a0, b1);
            double E10 = corr(a1, b0);
            double E11 = corr(a1, b1);
            backend->release_qubit(qa);
            backend->release_qubit(qb);
            return E00 + E01 + E10 - E11;
        }
    };

    class DecoyBB84
    {
    public:
        struct DecoyResult : QkdResult
        {
            size_t decoy_rounds = 0;
            double pns_attack_probability = 0.0;
        };

        static DecoyResult run(qhal::IQuantumBackend *backend, size_t num_rounds,
                               double decoy_ratio = 0.3, double pns_rate = 0.0)
        {
            Rng rng;
            const size_t q = backend->get_num_qubits();
            backend->allocate_qubits(q + 1);

            std::vector<bool> alice_bits(num_rounds), alice_basis(num_rounds), bob_basis(num_rounds);
            std::vector<bool> is_decoy(num_rounds);
            std::vector<int> outcomes(num_rounds);
            size_t decoy_count = 0;

            for (size_t i = 0; i < num_rounds; ++i)
            {
                alice_bits[i] = rng.random_bit();
                alice_basis[i] = rng.random_bit();
                bob_basis[i] = rng.random_bit();
                is_decoy[i] = rng.uniform_real() < decoy_ratio;
                if (is_decoy[i])
                    ++decoy_count;

                detail::reset_qubit(backend, q);
                if (alice_bits[i])
                    backend->apply_x(q);
                if (alice_basis[i])
                    backend->apply_h(q);

                if (!is_decoy[i] && rng.uniform_real() < pns_rate)
                    if (rng.random_bit())
                        backend->apply_x(q);

                if (bob_basis[i])
                    backend->apply_h(q);
                outcomes[i] = backend->measure(q);
            }
            backend->release_qubit(q);

            Bytes sifted;
            size_t decoy_errors = 0, decoy_sifted = 0;
            size_t signal_errors = 0, signal_sifted = 0;
            for (size_t i = 0; i < num_rounds; ++i)
            {
                if (alice_basis[i] != bob_basis[i])
                    continue;
                int expected = alice_bits[i] ? 1 : 0;
                bool err = (outcomes[i] != expected);
                sifted.push_back(Byte(expected));
                if (is_decoy[i])
                {
                    ++decoy_sifted;
                    if (err)
                        ++decoy_errors;
                }
                else
                {
                    ++signal_sifted;
                    if (err)
                        ++signal_errors;
                }
            }

            double decoy_qber = decoy_sifted ? double(decoy_errors) / double(decoy_sifted) : 0.0;
            double signal_qber = signal_sifted ? double(signal_errors) / double(signal_sifted) : 0.0;

            DecoyResult res;
            res.raw_bits = num_rounds;
            res.sifted_bits = sifted.size();
            res.sifted_key = sifted;
            res.decoy_rounds = decoy_count;
            res.error_rate = decoy_qber;
            res.pns_attack_probability = std::max(0.0, signal_qber - decoy_qber);
            res.key_rate = num_rounds == 0 ? 0.0 : double(sifted.size()) / double(num_rounds);
            Hash256 sk = sha256(sifted);
            res.shared_key = Bytes(sk.begin(), sk.end());
            return res;
        }
    };

}