#pragma once
// ============================================================================
// LF2B —— 无损特征快速上下文压缩：**块**层
//
// 三个文件的关系（LF2C2.hpp 是协调者）：
//
//   LF2B（本文件）  切块 + 抽「邻域特征」
//       把 token 上下文切成固定大小的块，为每块抽取一个定长特征向量。
//       特征同时概括**块自身**与**左右邻域**——只有块自身的信息不足以
//       支撑「下一块是什么」的预测，而邻域信息正是语言/时序数据里
//       最值钱的那部分。
//
//   LF2P            无损池 + 动态指针 + sumChunkloss
//       池里保留**原文块**（所以「无损」是构造性保证，不是靠训练出来的），
//       特征是访问路径；sumChunkloss 量化「只凭特征能重建多少」。
//
//   LF2C2           多子线路协调者
//       每块一条量子子线路 → 读出 ⟨Z⟩ → 特征块集合体 → 分配到无损池。
//
// ── 为什么在量子 AI 里需要「压缩上下文」 ──────────────────────────────
//
// 本仓的量子语言机（qlm::QLM）把上下文硬编码成 **16 qubit = 2 个 token**
// （QLM.hpp:345 的 meta["context.window"] = "2_Tokens"）：推理侧
// qml/Inference.hpp:216-226 取生成串的最后两个 token，用 apply_x 打进
// 16 个 qubit。问题是：
//
//   1. **窗口太短**：2 个 token 撑不起任何长程依赖；
//   2. **生成串无界增长**（Inference.hpp:272 `*generated_sequence += ...`），
//      上下文却只取最后 2 个 —— 中间全部丢掉；
//   3. **qubit 预算是硬约束**：每多看一个 token 就要多占 8 个 qubit，
//      而 16 qubit 已经是本仓的常用规模（QLM 默认 16 qubit）。
//
// LF2C 的回答是：**用同样的 qubit 数覆盖更长的历史**。把 N 个 token 的上下文
// 压成「特征块」（每块若干 qubit 预算内），再由量子子线路读出特征，
// 用特征构造上下文态。压缩比 N/2，qubit 占用不变。
//
// ── 特征语义（定长 16，顺序固定）──────────────────────────────────────
//
//   [0..7]  块内 8 个比特的密度（该位为 1 的 token 占比）
//   [8]     块内 token 均值 / 255
//   [9]     块内 token 标准差 / 255
//   [10]    首 token / 255
//   [11]    尾 token / 255
//   [12]    块内 8 比特分布的归一化熵（0..1）
//   [13]    块均值 − 左邻块均值（邻域差分）
//   [14]    块均值 − 右邻块均值
//   [15]    邻域（自身+左右）平均均值
//
// 之所以把这些写死成固定语义：特征是**表特征**（池按偏移寻址），语义必须
// 稳定 —— 否则「表」就退化成了无名的向量堆，sumChunkloss 的归因也就没了。
// ============================================================================

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
        /// 邻域特征维度（固定，见文件头的语义表）。
        constexpr size_t FEATURE_DIM = 16;

        /// 一个上下文块。
        struct Block
        {
            size_t index = 0;          ///< 块序号（从 0 起）
            size_t token_begin = 0;    ///< 在 token 序列中的起始下标
            size_t token_end = 0;      ///< 结束下标（不含）
            std::vector<uint8_t> tokens;  ///< 原文（块自身）
            std::vector<double> features; ///< 邻域特征（FEATURE_DIM 维）
        };

        /// 把 token 上下文切块并抽取邻域特征。
        ///
        /// 纯经典、确定性、无副作用 —— 它只负责「切」与「摘要」；
        /// 量子部分（把特征变成 ⟨Z⟩ 读出）在 lf2c2::Compressor。
        class BlockChunker
        {
        public:
            explicit BlockChunker(size_t block_size = 4)
                : block_size_(block_size < 1 ? 1 : block_size)
            {
            }

            size_t block_size() const { return block_size_; }

            /// 把 token 序列切成块（最后一块可能不满）。
            ///
            /// 空序列 ⇒ 空块表（不是一块空块：空块会让「块数」与「token 数」
            /// 的关系对不上，压缩比就成了除以零的形状）。
            std::vector<Block> chunk(const std::vector<uint8_t> &tokens) const
            {
                std::vector<Block> blocks;
                if (tokens.empty())
                    return blocks;

                const size_t n_blocks = (tokens.size() + block_size_ - 1) / block_size_;
                blocks.reserve(n_blocks);

                // 先切分（原文 + 范围），特征随后统一计算（邻域要看到邻居）
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

                // 邻域特征：逐块算，邻居的均值用「块均值表」预先算好
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

            /// 块均值（token 值 / 255，落在 0..1）。
            static double block_mean(const std::vector<uint8_t> &tokens)
            {
                if (tokens.empty())
                    return 0.0;
                double s = 0.0;
                for (uint8_t t : tokens)
                    s += static_cast<double>(t) / 255.0;
                return s / static_cast<double>(tokens.size());
            }

            /// 邻域特征：块自身摘要 + 与左右邻块的差分。
            ///
            /// `left_mean` / `right_mean` 是**邻块均值**（不是邻块特征）——
            /// 差分用的是「邻居有多不一样」这个一维量：足够表达相邻块的跳变，
            /// 又不会把特征维度撑成 2×FEATURE_DIM。
            static std::vector<double> neighborhood_feature(const std::vector<uint8_t> &tokens,
                                                           double left_mean,
                                                           double right_mean)
            {
                std::vector<double> f(FEATURE_DIM, 0.0);
                if (tokens.empty())
                {
                    // 空块：自身摘要全 0，但邻域差分仍然有意义（= −邻居均值）
                    f[13] = -left_mean;
                    f[14] = -right_mean;
                    f[15] = 0.5 * (left_mean + right_mean);
                    return f;
                }

                // [0..7] 比特密度
                for (size_t bit = 0; bit < 8; ++bit)
                {
                    size_t ones = 0;
                    for (uint8_t t : tokens)
                        if ((t >> bit) & 1u)
                            ++ones;
                    f[bit] = static_cast<double>(ones) / static_cast<double>(tokens.size());
                }

                // [8] 均值 / [9] 标准差
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

                // [10] 首 token / [11] 尾 token
                f[10] = static_cast<double>(tokens.front()) / 255.0;
                f[11] = static_cast<double>(tokens.back()) / 255.0;

                // [12] 8 比特分布的归一化熵（0..1，1 = 八个比特等概率）
                {
                    double h = 0.0;
                    for (size_t bit = 0; bit < 8; ++bit)
                    {
                        const double p = f[bit];
                        if (p > 1e-12)
                            h -= p * std::log2(p);
                    }
                    f[12] = h / 3.0; // log2(8) = 3
                }

                // [13][14] 与左右邻块的均值差
                f[13] = mean - left_mean;
                f[14] = mean - right_mean;
                // [15] 邻域平均
                f[15] = (mean + left_mean + right_mean) / 3.0;
                return f;
            }

        private:
            size_t block_size_;
        };
    }
}