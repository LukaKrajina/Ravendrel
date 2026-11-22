#pragma once

#include "../numqk/Numqk.hpp"
#include "../numqk/QuantumGeometry.hpp"
#include "LF2B.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace qlm
{
    namespace lf2p
    {
        struct ChunkPtr
        {
            uint64_t pool_id = 0;
            uint32_t chunk_index = 0;
            uint32_t feature_offset = 0;
            bool valid() const { return pool_id != 0; }
        };

        class LosslessPool
        {
        public:
            static constexpr double RIDGE_LAMBDA = 1e-3;

            LosslessPool() : pool_id_(next_pool_id())
            {
            }

            uint64_t pool_id() const { return pool_id_; }
            size_t size() const { return blocks_.size(); }
            bool empty() const { return blocks_.empty(); }

            const std::vector<double> &feature_table() const { return feature_table_; }

            ChunkPtr allocate(const std::vector<uint8_t> &original,
                              const std::vector<double> &features)
            {
                ChunkPtr ptr;
                ptr.pool_id = pool_id_;
                ptr.chunk_index = static_cast<uint32_t>(blocks_.size());
                ptr.feature_offset = static_cast<uint32_t>(feature_table_.size());

                blocks_.push_back(Block{original, features, block_targets(original)});
                feature_table_.insert(feature_table_.end(), features.begin(), features.end());
                decoder_dirty_ = true;
                return ptr;
            }

            const std::vector<uint8_t> &recover(const ChunkPtr &ptr) const
            {
                static const std::vector<uint8_t> kEmpty;
                if (!valid(ptr))
                    return kEmpty;
                return blocks_[ptr.chunk_index].original;
            }

            const std::vector<double> &features(const ChunkPtr &ptr) const
            {
                static const std::vector<double> kEmpty;
                if (!valid(ptr))
                    return kEmpty;
                return blocks_[ptr.chunk_index].features;
            }

            bool valid(const ChunkPtr &ptr) const
            {
                return ptr.pool_id == pool_id_ && ptr.chunk_index < blocks_.size();
            }

            double chunk_loss(size_t chunk_index) const
            {
                if (chunk_index >= blocks_.size())
                    return 0.0;
                ensure_decoder();
                if (decoder_.empty())
                    return 0.0;

                const Block &blk = blocks_[chunk_index];
                const size_t n_bits = blk.target.size();
                if (n_bits == 0)
                    return 0.0;

                std::vector<double> pred = predict(blk.features);
                double se = 0.0;
                for (size_t i = 0; i < n_bits && i < pred.size(); ++i)
                {
                    const double d = pred[i] - blk.target[i];
                    se += d * d;
                }
                return se / static_cast<double>(n_bits);
            }

            double sumChunkloss() const
            {
                double total = 0.0;
                for (size_t i = 0; i < blocks_.size(); ++i)
                    total += chunk_loss(i);
                return total;
            }

            double compression_ratio() const
            {
                if (feature_table_.empty())
                    return 0.0;
                size_t tokens = 0;
                for (const Block &b : blocks_)
                    tokens += b.original.size();
                return static_cast<double>(tokens) / static_cast<double>(feature_table_.size());
            }

            size_t total_tokens() const
            {
                size_t n = 0;
                for (const Block &b : blocks_)
                    n += b.original.size();
                return n;
            }

            void fit_decoder(const std::vector<std::vector<double>> &x,
                             const std::vector<std::vector<double>> &y,
                             size_t out_dim) const
            {
                decoder_.clear();
                if (x.empty() || y.empty() || x.size() != y.size())
                    return;
                const size_t n_feat = x[0].size();
                const size_t n_rows = x.size();
                if (n_feat == 0)
                    return;

                numqk::Tensor<double> A({n_feat, n_feat}, false);
                numqk::Tensor<double> B({n_feat, out_dim}, false);
                for (size_t r = 0; r < n_rows; ++r)
                {
                    for (size_t i = 0; i < n_feat; ++i)
                    {
                        const double xi = x[r][i];
                        if (xi == 0.0)
                            continue;
                        for (size_t j = 0; j < n_feat; ++j)
                            A.data()[i * n_feat + j] += xi * x[r][j];
                        for (size_t j = 0; j < out_dim && j < y[r].size(); ++j)
                            B.data()[i * out_dim + j] += xi * y[r][j];
                    }
                }
                for (size_t i = 0; i < n_feat; ++i)
                    A.data()[i * n_feat + i] += RIDGE_LAMBDA;

                const numqk::Tensor<double> Ainv = numqk::sym_pinv(A);
                numqk::Tensor<double> W({n_feat, out_dim}, false);
                for (size_t i = 0; i < n_feat; ++i)
                    for (size_t j = 0; j < out_dim; ++j)
                    {
                        double acc = 0.0;
                        for (size_t k = 0; k < n_feat; ++k)
                            acc += Ainv.data()[i * n_feat + k] * B.data()[k * out_dim + j];
                        W.data()[i * out_dim + j] = acc;
                    }

                decoder_.assign(W.data(), W.data() + n_feat * out_dim);
                decoder_rows_ = n_feat;
                decoder_cols_ = out_dim;
                decoder_dirty_ = false;
            }

        private:
            struct Block
            {
                std::vector<uint8_t> original;
                std::vector<double> features;
                std::vector<double> target;
            };

            static std::vector<double> block_targets(const std::vector<uint8_t> &tokens)
            {
                std::vector<double> t;
                t.reserve(tokens.size() * 8);
                for (uint8_t tok : tokens)
                    for (size_t bit = 0; bit < 8; ++bit)
                        t.push_back(((tok >> bit) & 1u) ? 1.0 : 0.0);
                return t;
            }

            size_t target_dim() const
            {
                size_t d = 0;
                for (const Block &b : blocks_)
                    d = std::max(d, b.target.size());
                return d;
            }

            void ensure_decoder() const
            {
                if (!decoder_dirty_ && !decoder_.empty())
                    return;
                const size_t dim = target_dim();
                if (dim == 0 || blocks_.empty())
                {
                    decoder_.clear();
                    decoder_dirty_ = false;
                    return;
                }
                std::vector<std::vector<double>> X;
                std::vector<std::vector<double>> Y;
                X.reserve(blocks_.size());
                Y.reserve(blocks_.size());
                for (const Block &b : blocks_)
                {
                    std::vector<double> row = b.features;
                    row.push_back(1.0);
                    std::vector<double> t = b.target;
                    t.resize(dim, 0.0);
                    X.push_back(std::move(row));
                    Y.push_back(std::move(t));
                }
                fit_decoder(X, Y, dim);
            }

            std::vector<double> predict(const std::vector<double> &features) const
            {
                std::vector<double> out(decoder_cols_, 0.0);
                if (decoder_.empty())
                    return out;
                const size_t rows = std::min(decoder_rows_, features.size() + 1);
                for (size_t i = 0; i < rows; ++i)
                {
                    const double xi = (i < features.size()) ? features[i] : 1.0;
                    if (xi == 0.0)
                        continue;
                    for (size_t j = 0; j < decoder_cols_; ++j)
                        out[j] += xi * decoder_[i * decoder_cols_ + j];
                }
                return out;
            }

            static uint64_t next_pool_id()
            {
                static uint64_t counter = 0;
                return ++counter;
            }

            uint64_t pool_id_;
            std::vector<Block> blocks_;
            std::vector<double> feature_table_;
            mutable std::vector<double> decoder_;
            mutable size_t decoder_rows_ = 0;
            mutable size_t decoder_cols_ = 0;
            mutable bool decoder_dirty_ = true;
        };
    }
}