#pragma once

#include "../qhal/IQuantumBackend.hpp"
#include "LF2B.hpp"
#include "LF2P.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace qlm
{
    namespace lf2c2
    {

        class Compressor
        {
        public:
            Compressor(qhal::IQuantumBackend *be, size_t block_size = 4, size_t feature_qubits = 8)
                : backend_(be), chunker_(block_size),
                  feature_qubits_(feature_qubits < 1 ? 1 : feature_qubits)
            {
            }

            size_t block_size() const { return chunker_.block_size(); }
            size_t feature_qubits() const { return feature_qubits_; }

            void set_quantum_encoder(bool on) { quantum_encoder_ = on; }
            bool quantum_encoder() const { return quantum_encoder_; }

            void compress(const std::vector<uint8_t> &tokens, lf2p::LosslessPool &pool,
                          const std::vector<size_t> &work_qubits)
            {
                if (quantum_encoder_)
                {
                    if (!backend_ || work_qubits.size() < feature_qubits_)
                        return;
                    const std::vector<lf2b::Block> blocks = chunker_.chunk(tokens);
                    for (const lf2b::Block &blk : blocks)
                    {
                        const std::vector<double> qfeat = encode_block(blk.features, work_qubits);
                        pool.allocate(blk.tokens, qfeat);
                    }
                    return;
                }

                const std::vector<lf2b::Block> blocks = chunker_.chunk(tokens);
                for (const lf2b::Block &blk : blocks)
                {
                    const std::vector<double> feat = project(blk.features);
                    pool.allocate(blk.tokens, feat);
                }
            }

            void prepare_context_state(const lf2p::LosslessPool &pool,
                                       const std::vector<size_t> &context_qubits) const
            {
                if (!backend_ || context_qubits.empty())
                    return;
                const std::vector<double> &table = pool.feature_table();
                if (table.empty())
                    return;

                const size_t n_ctx = std::min(context_qubits.size(), table.size());
                const size_t first = table.size() - n_ctx;
                for (size_t s = 0; s < n_ctx; ++s)
                {
                    const double z = table[first + s];
                    backend_->apply_ry(context_qubits[s], (M_PI / 2.0) * z);
                }
            }

            double sumChunkloss(const lf2p::LosslessPool &pool) const
            {
                return pool.sumChunkloss();
            }

            double compression_ratio(const lf2p::LosslessPool &pool) const
            {
                return pool.compression_ratio();
            }

            double qubit_compression_ratio(const lf2p::LosslessPool &pool) const
            {
                const size_t n_blocks = pool.size();
                if (n_blocks == 0)
                    return 0.0;
                const size_t raw_qubits = pool.total_tokens() * 8;
                const size_t compressed_qubits = n_blocks * feature_qubits_;
                if (compressed_qubits == 0)
                    return 0.0;
                return static_cast<double>(raw_qubits) / static_cast<double>(compressed_qubits);
            }

        private:
            std::vector<double> project(const std::vector<double> &features) const
            {
                const size_t n = feature_qubits_;
                std::vector<double> out(n, 0.0);
                const double norm = 2.0 / std::sqrt(static_cast<double>(lf2b::FEATURE_DIM));
                for (size_t i = 0; i < n; ++i)
                {
                    double acc = 0.0;
                    for (size_t j = 0; j < features.size(); ++j)
                        acc += features[j] *
                               std::cos(M_PI * (static_cast<double>(i) + 0.5) *
                                        static_cast<double>(j) /
                                        static_cast<double>(lf2b::FEATURE_DIM));
                    out[i] = norm * acc;
                }
                return out;
            }

            std::vector<double> encode_block(const std::vector<double> &features,
                                            const std::vector<size_t> &work_qubits)
            {
                const size_t n = feature_qubits_;
                std::vector<double> out(n, 0.0);

                std::vector<double> angles(n, 0.0);
                const double norm = 2.0 / std::sqrt(static_cast<double>(lf2b::FEATURE_DIM));
                for (size_t i = 0; i < n; ++i)
                {
                    double acc = 0.0;
                    for (size_t j = 0; j < features.size(); ++j)
                        acc += features[j] *
                               std::cos(M_PI * (static_cast<double>(i) + 0.5) *
                                        static_cast<double>(j) /
                                        static_cast<double>(lf2b::FEATURE_DIM));
                    angles[i] = M_PI * norm * acc;
                }

                for (size_t i = 0; i < n; ++i)
                    backend_->apply_ry(work_qubits[i], angles[i]);

                for (size_t i = 0; i + 1 < n; ++i)
                    backend_->apply_cnot(work_qubits[i], work_qubits[i + 1]);

                for (size_t i = 0; i < n; ++i)
                    out[i] = backend_->expectation_z(work_qubits[i]);

                for (size_t i = 0; i < n; ++i)
                {
                    if (backend_->measure(work_qubits[i]) == 1)
                        backend_->apply_x(work_qubits[i]);
                }
                return out;
            }

            qhal::IQuantumBackend *backend_;
            lf2b::BlockChunker chunker_;
            size_t feature_qubits_;
            bool quantum_encoder_ = false;
        };

        struct ContextConfig
        {
            size_t block_size = 4;
            size_t feature_qubits = 2;
            size_t window_tokens = 32;
        };

        inline ContextConfig context_config_for_qubits(size_t n_q)
        {
            ContextConfig cfg;
            const size_t blocks =
                (n_q + cfg.feature_qubits - 1) / cfg.feature_qubits;
            cfg.window_tokens = blocks * cfg.block_size;
            return cfg;
        }

        inline std::vector<double> context_angles(const ContextConfig &cfg,
                                                  const std::vector<uint8_t> &tokens,
                                                  size_t n_ctx)
        {
            std::vector<double> angles;
            if (n_ctx == 0)
                return angles;

            std::vector<uint8_t> window = tokens;
            if (cfg.window_tokens > 0 && window.size() > cfg.window_tokens)
                window.erase(window.begin(),
                             window.end() - static_cast<long>(cfg.window_tokens));

            lf2p::LosslessPool pool;
            Compressor compressor(nullptr, cfg.block_size, cfg.feature_qubits);
            compressor.compress(window, pool, {});

            const std::vector<double> &table = pool.feature_table();
            if (table.empty())
                return angles;

            const size_t n = std::min(n_ctx, table.size());
            const size_t first = table.size() - n;
            angles.resize(n);
            for (size_t s = 0; s < n; ++s)
                angles[s] = (M_PI / 2.0) * table[first + s];
            return angles;
        }

        inline void encode_context(qhal::IQuantumBackend *be, const ContextConfig &cfg,
                                   const std::vector<uint8_t> &tokens,
                                   const std::vector<size_t> &context_qubits,
                                   lf2p::LosslessPool *pool_out = nullptr)
        {
            if (!be || context_qubits.empty())
                return;

            const std::vector<double> angles =
                context_angles(cfg, tokens, context_qubits.size());
            for (size_t s = 0; s < angles.size(); ++s)
                be->apply_ry(context_qubits[s], angles[s]);

            if (pool_out)
            {
                std::vector<uint8_t> window = tokens;
                if (cfg.window_tokens > 0 && window.size() > cfg.window_tokens)
                    window.erase(window.begin(),
                                 window.end() - static_cast<long>(cfg.window_tokens));
                Compressor compressor(be, cfg.block_size, cfg.feature_qubits);
                compressor.compress(window, *pool_out, {});
            }
        }
    }
}