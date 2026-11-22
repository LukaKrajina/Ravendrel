#pragma once

#include "../numqk/Numqk.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace qlm
{
    namespace lf2b
    {
        constexpr size_t FEATURE_DIM = 16;

        struct Block
        {
            size_t index = 0;
            size_t token_begin = 0;
            size_t token_end = 0;
            std::vector<uint8_t> tokens;
            std::vector<double> features;
        };

        class BlockChunker
        {
        public:
            explicit BlockChunker(size_t block_size = 4)
                : block_size_(block_size < 1 ? 1 : block_size)
            {
            }

            size_t block_size() const { return block_size_; }

            std::vector<Block> chunk(const std::vector<uint8_t> &tokens) const
            {
                std::vector<Block> blocks;
                if (tokens.empty())
                    return blocks;

                const size_t n_blocks = (tokens.size() + block_size_ - 1) / block_size_;
                blocks.reserve(n_blocks);

                for (size_t b = 0; b < n_blocks; ++b)
                {
                    Block blk;
                    blk.index = b;
                    blk.token_begin = b * block_size_;
                    blk.token_end = std::min(blk.token_begin + block_size_, tokens.size());
                    blk.tokens.assign(tokens.begin() + (long)blk.token_begin,
                                      tokens.begin() + (long)blk.token_end);
                    blocks.push_back(std::move(blk));
                }

                std::vector<double> mean_of(n_blocks, 0.0);
                for (size_t b = 0; b < n_blocks; ++b)
                    mean_of[b] = block_mean(blocks[b].tokens);

                for (size_t b = 0; b < n_blocks; ++b)
                {
                    const double left = (b > 0) ? mean_of[b - 1] : mean_of[b];
                    const double right = (b + 1 < n_blocks) ? mean_of[b + 1] : mean_of[b];
                    blocks[b].features = neighborhood_feature(blocks[b].tokens, left, right);
                }
                return blocks;
            }

            static double block_mean(const std::vector<uint8_t> &tokens)
            {
                if (tokens.empty())
                    return 0.0;
                double s = 0.0;
                for (uint8_t t : tokens)
                    s += static_cast<double>(t) / 255.0;
                return s / static_cast<double>(tokens.size());
            }

            static std::vector<double> neighborhood_feature(const std::vector<uint8_t> &tokens,
                                                           double left_mean,
                                                           double right_mean)
            {
                std::vector<double> f(FEATURE_DIM, 0.0);
                if (tokens.empty())
                {
                    f[13] = -left_mean;
                    f[14] = -right_mean;
                    f[15] = 0.5 * (left_mean + right_mean);
                    return f;
                }

                for (size_t bit = 0; bit < 8; ++bit)
                {
                    size_t ones = 0;
                    for (uint8_t t : tokens)
                        if ((t >> bit) & 1u)
                            ++ones;
                    f[bit] = static_cast<double>(ones) / static_cast<double>(tokens.size());
                }

                double mean = 0.0;
                for (uint8_t t : tokens)
                    mean += static_cast<double>(t) / 255.0;
                mean /= static_cast<double>(tokens.size());
                double var = 0.0;
                for (uint8_t t : tokens)
                {
                    const double d = static_cast<double>(t) / 255.0 - mean;
                    var += d * d;
                }
                var /= static_cast<double>(tokens.size());
                f[8] = mean;
                f[9] = std::sqrt(var);

                f[10] = static_cast<double>(tokens.front()) / 255.0;
                f[11] = static_cast<double>(tokens.back()) / 255.0;

                {
                    double h = 0.0;
                    for (size_t bit = 0; bit < 8; ++bit)
                    {
                        const double p = f[bit];
                        if (p > 1e-12)
                            h -= p * std::log2(p);
                    }
                    f[12] = h / 3.0;
                }

                f[13] = mean - left_mean;
                f[14] = mean - right_mean;
                f[15] = (mean + left_mean + right_mean) / 3.0;
                return f;
            }

        private:
            size_t block_size_;
        };
    }
}