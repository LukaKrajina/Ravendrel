#pragma once
#include "../common.hpp"
#include "../../qhal/IQuantumBackend.hpp"
#include <set>
#include <vector>

namespace qchain::consensus
{

    using qchain::Byte;
    using qchain::Rng;

    inline void create_ghz(qhal::IQuantumBackend *backend, size_t n)
    {
        backend->allocate_qubits(n);
        for (size_t i = 0; i < n; ++i)
        {
            if (backend->measure(i) == 1)
                backend->apply_x(i);
        }
        backend->apply_h(0);
        for (size_t i = 1; i < n; ++i)
            backend->apply_cnot(0, i);
    }

    struct QbaResult
    {
        bool agreement = false;
        size_t detected_rounds = 0;
        size_t total_rounds = 0;
        std::vector<int> outcomes;
    };

    class QuantumByzantineAgreement
    {
    public:
        static QbaResult run(qhal::IQuantumBackend *backend, size_t n_parties,
                             const std::set<size_t> &corrupt, size_t num_rounds,
                             uint64_t seed = 0)
        {
            Rng rng(seed);
            QbaResult res;
            res.total_rounds = num_rounds;

            for (size_t round = 0; round < num_rounds; ++round)
            {
                create_ghz(backend, n_parties);
                std::vector<int> out(n_parties);

                bool global_x = rng.random_bit();
                for (size_t i = 0; i < n_parties; ++i)
                {
                    if (global_x)
                        backend->apply_h(i);
                    out[i] = backend->measure(i);
                }

                for (size_t i : corrupt)
                    out[i] ^= 1;

                bool consistent = true;
                if (global_x)
                {
                    int x_xor = 0;
                    for (int v : out)
                        x_xor ^= v;
                    consistent = (x_xor == 0);
                }
                else
                {
                    int ref = out.empty() ? 0 : out[0];
                    for (int v : out)
                        if (v != ref)
                            consistent = false;
                }

                if (!consistent)
                    ++res.detected_rounds;

                if (round + 1 == num_rounds)
                    res.outcomes = out;

                for (size_t i = 0; i < n_parties; ++i)
                    backend->release_qubit(i);
            }

            res.agreement = (res.detected_rounds == 0);
            return res;
        }
    };
}