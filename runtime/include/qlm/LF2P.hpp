#pragma once
// ============================================================================
// LF2P —— 无损特征快速上下文压缩：**池**层
//
// 池是「无损」这个词的落点：
//
//   · 池里保留**原文块**（tokens），所以恢复是**构造性**的 —— 不依赖
//     训练、不依赖解码器精度。LF2C 论文里的「lossless feature」在这里
//     被拆成两半：
//       - 原文在池里 ⇒ recover() 精确无损；
//       - 特征是**访问路径** ⇒ 只用特征能重建多少，由 sumChunkloss 度量。
//     把这两件事混在一起（「声称特征无损」）是常见误解：神经解码器永远
//     有误差，「无损」指的是**可以无损地取回原文**，而不是「特征里
//     藏着全文」。
//
//   · 特征是**表特征**（一张按偏移寻址的表），所以「动态指针」可以只带
//     （池 id、块号、特征偏移）三个整数就访问到上下文 —— 不复制原文。
//     指针带池 id 是为了**陈旧检测**：换一个池之后，旧指针必须失效，
//     而不是静默指到别的块上。
//
// ── sumChunkloss 的定义 ───────────────────────────────────────────────
//
// 它是「池内所有块的重建误差之和」：对每块，用**岭回归解码器**（特征 →
// 该块的比特）重建原文，算均方误差，再对所有块求和。
//
//   为什么用岭回归而不是神经网络：解码器是**度量工具**，不是模型。
//   它的职责是回答「这组量子特征到底保留了多少上下文信息」。用线性
//   岭回归有三条好处：① 闭式解、无随机性 ⇒ 同样特征永远给同样损失，
//   测试可复现；② 没有隐藏容量 ⇒ 损失不会被解码器自己的容量掩盖
//   （一个百万参数的解码器能把任何特征「记住」，那 loss 就没意义了）；
//   ③ 可解析（正规方程 + 对称伪逆）⇒ 与本仓既有的 numqk::sym_pinv 同源。
//
//   损失越小 ⇒ 特征保留的信息越多 ⇒ 这就是压缩器的训练信号。
// ============================================================================

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
        /// 动态指针：池内一个块的轻量句柄。
        ///
        /// 只带三个整数 —— **不复制原文**。这是「动态指针通过表特征访问上下文」
        /// 的字面实现：指针 + 表 = 上下文。
        struct ChunkPtr
        {
            uint64_t pool_id = 0;      ///< 池身份（陈旧检测用）
            uint32_t chunk_index = 0;  ///< 块号
            uint32_t feature_offset = 0; ///< 在特征表中的起始元素下标
            bool valid() const { return pool_id != 0; }
        };

        /// 无损池：原文块 + 特征表 + 岭回归解码器。
        class LosslessPool
        {
        public:
            /// 岭回归的正则强度（正规方程上加 λI）。
            ///
            /// 不能是 0：特征数（16+1）常常大于块数（短上下文），正规方程
            /// 会奇异，伪逆的阈值截断就成了唯一的「正则」—— 那是隐式的、
            /// 依赖阈值的正则，不如显式写死。
            static constexpr double RIDGE_LAMBDA = 1e-3;

            LosslessPool() : pool_id_(next_pool_id())
            {
            }

            uint64_t pool_id() const { return pool_id_; }
            size_t size() const { return blocks_.size(); }
            bool empty() const { return blocks_.empty(); }

            /// 特征表（所有块的特征首尾相接）—— 「表特征」本体。
            const std::vector<double> &feature_table() const { return feature_table_; }

            /// 分配一个特征块集合体：原文 + 特征 → 池。
            ///
            /// 返回的指针就是「动态指针」：调用方拿着它就能访问池里的这一块，
            /// 而不必把原文复制出去。
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

            /// 无损恢复：通过动态指针取回原文块。
            ///
            /// 池里存着原文，所以这是**精确**的（逐字节）。
            const std::vector<uint8_t> &recover(const ChunkPtr &ptr) const
            {
                static const std::vector<uint8_t> kEmpty;
                if (!valid(ptr))
                    return kEmpty;
                return blocks_[ptr.chunk_index].original;
            }

            /// 通过动态指针取回该块的特征（表特征的一段）。
            const std::vector<double> &features(const ChunkPtr &ptr) const
            {
                static const std::vector<double> kEmpty;
                if (!valid(ptr))
                    return kEmpty;
                return blocks_[ptr.chunk_index].features;
            }

            /// 指针是否仍然有效（池 id 匹配 + 块号在范围内）。
            bool valid(const ChunkPtr &ptr) const
            {
                return ptr.pool_id == pool_id_ && ptr.chunk_index < blocks_.size();
            }

            /// 单块重建损失（特征 → 原文比特的均方误差）。
            ///
            /// 解码器未拟合时返回 0（无可说的重建 ⇒ 不谎报）。
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

            /// 池内所有块的总损失 —— 压缩器的训练信号。
            double sumChunkloss() const
            {
                double total = 0.0;
                for (size_t i = 0; i < blocks_.size(); ++i)
                    total += chunk_loss(i);
                return total;
            }

            /// 压缩比：**上下文 token 数 / 特征值数**。
            ///
            /// 「一个特征承载多少个 token」。>1 才是压缩。
            ///
            /// 为什么不用「比特数之比」：特征是 double（64 bit），拿它和
            /// token 的 8 bit 相比，量纲上是在比「存储」而不是比「信息」——
            /// 而 LF2C 要回答的问题是**同样的 qubit 预算能覆盖多长的历史**
            /// （qubit 层面的比值见 lf2c2::Compressor::qubit_compression_ratio）。
            /// 特征表为空时返回 0（比值无意义，不是 ∞）。
            double compression_ratio() const
            {
                if (feature_table_.empty())
                    return 0.0;
                size_t tokens = 0;
                for (const Block &b : blocks_)
                    tokens += b.original.size();
                return static_cast<double>(tokens) / static_cast<double>(feature_table_.size());
            }

            /// 上下文 token 总数（池里所有块的原文长度之和）。
            size_t total_tokens() const
            {
                size_t n = 0;
                for (const Block &b : blocks_)
                    n += b.original.size();
                return n;
            }

            /// 用一维线性模型从特征重建比特：给定的 (x, y) 样本拟合
            /// w = (XᵀX + λI)⁻¹ Xᵀ y，解码器随之更新。
            ///
            /// 供上层（量子子线路的训练回路）在线更新解码器用。
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

                // 正规方程：(XᵀX + λI) w = Xᵀ y
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
                // w = A⁻¹ · B
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
                std::vector<double> target; ///< 原文的比特展开（0/1）
            };

            /// 块的比特展开：每个 token 的 8 位，按 bit0..bit7 顺序。
            static std::vector<double> block_targets(const std::vector<uint8_t> &tokens)
            {
                std::vector<double> t;
                t.reserve(tokens.size() * 8);
                for (uint8_t tok : tokens)
                    for (size_t bit = 0; bit < 8; ++bit)
                        t.push_back(((tok >> bit) & 1u) ? 1.0 : 0.0);
                return t;
            }

            /// 岭回归解码器的输出维度（所有块里最宽的那个）。
            size_t target_dim() const
            {
                size_t d = 0;
                for (const Block &b : blocks_)
                    d = std::max(d, b.target.size());
                return d;
            }

            /// 拟合解码器（若尚未拟合或池已变更）。
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
                // 自拟合：设计矩阵 = 特征 + 偏置列
                std::vector<std::vector<double>> X;
                std::vector<std::vector<double>> Y;
                X.reserve(blocks_.size());
                Y.reserve(blocks_.size());
                for (const Block &b : blocks_)
                {
                    std::vector<double> row = b.features;
                    row.push_back(1.0); // 偏置
                    std::vector<double> t = b.target;
                    t.resize(dim, 0.0);
                    X.push_back(std::move(row));
                    Y.push_back(std::move(t));
                }
                fit_decoder(X, Y, dim);
            }

            /// 用当前解码器预测一块的比特。
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
            // 解码器是由池内容派生的**缓存**（惰性拟合）⇒ mutable。
            // 池的原文与特征本身不是 mutable：只有解码器是。
            mutable std::vector<double> decoder_;
            mutable size_t decoder_rows_ = 0;
            mutable size_t decoder_cols_ = 0;
            mutable bool decoder_dirty_ = true;
        };
    }
}