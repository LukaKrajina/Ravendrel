#pragma once
// ============================================================================
// LF2C2 —— 无损特征快速上下文压缩：**协调者**
//
// 把三层接起来：
//
//   ① 动态指针通过表特征访问上下文   → lf2p::ChunkPtr + 特征表
//   ② 抽象出邻域数据特征成块         → lf2b::BlockChunker
//   ③ 凭借多子线路重复上述流程得特征块集合体 → 本文件的 Compressor
//   ④ 这些块集合体通过分配到无损池内 → lf2p::LosslessPool::allocate
//   ⑤ 池内 sumChunkloss 会精准得出所有块总损失 → lf2p::LosslessPool::sumChunkloss
//
// ── 量子子线路（每个块一条）───────────────────────────────────────────
//
//   对块 b：
//     1. 工作 qubit 从 |0⟩ 起；
//     2. 把该块的**邻域特征**（16 维，LF2B 给的）用 DCT-II 基投影到
//        feature_qubits 个角度上，逐个 RY 旋转；
//     3. 一条固定的三角 CNOT 梯（相邻工作 qubit 纠缠）—— 纠缠是必要的：
//        不纠缠的话每个读出只是特征的某个线性投影，信息量被摊薄；
//     4. 逐个读 ⟨Z⟩ ⇒ **量子特征**（feature_qubits 维，[-1,1]）——
//        这才是进池的「表特征」；
//     5. 把工作 qubit 复位到 |0⟩（测量 + 条件 X）供下一块使用。
//
//   「多子线路」在这里是**顺序实现**的：后端没有并行电路的接口，把各块
//   放在各自的工作 qubit 上并行需要 2×(块数×feature_qubits) 个 qubit ——
//   稠密态矢量的内存是 2^n，16 qubit 已经 1 MiB，再翻倍就不是「压缩」
//   而是「扩容」。所以这里让各块**共用**同一组工作 qubit，靠复位隔开，
//   语义等价（每块的子线路独立），代价是顺序执行。
//
// ── 特征为什么要有「经典摘要」和「量子读出」两层 ──────────────────────
//
//   邻域特征（经典，16 维）：块自身 + 邻域的可解释统计量，是子线路的**输入**；
//   量子特征（⟨Z⟩ 读出，feature_qubits 维）：子线路的**输出**，进池、给解码器。
//   两层都必要：只有经典层就不是「量子压缩」；只有量子层则没有可解释的
//   入口特征（而且子线路无从构造）。
// ============================================================================

#include "../qhal/IQuantumBackend.hpp"
#include "LF2B.hpp"
#include "LF2P.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace qlm
{
    namespace lf2c2
    {
        /// 上下文压缩参数。
        ///
        /// ⚠ 训练侧与推理侧**必须用同一份**（见 encode_context 的说明）。

        /// 多子线路压缩器：把 token 上下文压成特征块集合体并入池。
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

            /// 是否走**量子**子线路编码。
            ///
            /// 默认 false（经典特征路径）：训练侧要给**每个样本**都算一遍上下文，
            /// 而量子子线路的开销是每个样本都要跑 n_blocks 条电路 —— 训练循环
            /// 会被拖垮。所以训练与推理默认都用经典路径（**两侧必须一致**，
            /// 否则模型在训练时看的是原始 2-token 上下文、推理时看的是压缩
            /// 上下文 ⇒ train/test skew，输出是垃圾）。
            ///
            /// 置 true 则每块跑一条量子子线路、读出 ⟨Z⟩ 作为特征 —— 那是
            /// 「真·量子压缩」的实验路径（单次推理付得起，批量训练付不起）。
            void set_quantum_encoder(bool on) { quantum_encoder_ = on; }
            bool quantum_encoder() const { return quantum_encoder_; }

            /// 压缩：token 序列 → 特征块集合体 → 分配到无损池。
            ///
            /// `work_qubits` 是编码子线路用的工作 qubit，**由调用方分配**：
            /// 压缩器不能自己分配（会和上下文的 qubit 撞号 —— 本仓的 qubit id
            /// 是全局的，QLM 的上下文占 0..15）。经典路径不需要工作 qubit。
            ///
            /// 经典路径下，块的特征 = 邻域特征经 DCT-II 投影到 feature_qubits
            /// 维（与量子路径同维 ⇒ 池、解码器、上下文态的接法完全一致，
            /// 只是特征的来源不同）。
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
                        // ② 邻域特征 → 量子子线路 → ③ ⟨Z⟩ 读出 = 量子特征
                        const std::vector<double> qfeat = encode_block(blk.features, work_qubits);
                        // ④ 分配到无损池（原文一起进池 ⇒ 恢复是构造性无损的）
                        pool.allocate(blk.tokens, qfeat);
                    }
                    return;
                }

                // 经典路径：邻域特征 → DCT 投影 → 入池
                const std::vector<lf2b::Block> blocks = chunker_.chunk(tokens);
                for (const lf2b::Block &blk : blocks)
                {
                    const std::vector<double> feat = project(blk.features);
                    pool.allocate(blk.tokens, feat);
                }
            }

            /// 用压缩上下文构造量子态。
            ///
            /// 池特征表里的值（经典路径：投影后的邻域特征；量子路径：⟨Z⟩ 读出）
            /// 逐个映射成上下文 qubit 上的 RY 角：(π/2)·z —— 特征为 +1 时把
            /// qubit 转成 |0⟩ 与 |1⟩ 的叠加、−1 时转成反向叠加，0 时不动。
            /// 上下文态因此是压缩历史的**连续**函数，而不是「最近两个 token 的
            /// 比特拷贝」。
            ///
            /// 取的是特征表的**最后 n_ctx 个值**（扁平取用，跨块连续）——
            /// 块与特征是两级粒度，按块取会让一半的上下文 qubit 拿不到特征。
            /// 覆盖范围 = (n_ctx / feature_qubits) 个块 = 该数量的 block_size
            /// 个 token —— 这就是「同样 qubit 预算能覆盖多长历史」的答案。
            void prepare_context_state(const lf2p::LosslessPool &pool,
                                       const std::vector<size_t> &context_qubits) const
            {
                if (!backend_ || context_qubits.empty())
                    return;
                const std::vector<double> &table = pool.feature_table();
                if (table.empty())
                    return;

                const size_t n_ctx = std::min(context_qubits.size(), table.size());
                const size_t first = table.size() - n_ctx; // 取最后 n_ctx 个
                for (size_t s = 0; s < n_ctx; ++s)
                {
                    const double z = table[first + s];
                    // (π/2)·z：z=+1 → +π/2，z=-1 → -π/2
                    backend_->apply_ry(context_qubits[s], (M_PI / 2.0) * z);
                }
            }

            /// 池内总损失（转发到池）—— 压缩器的训练信号。
            double sumChunkloss(const lf2p::LosslessPool &pool) const
            {
                return pool.sumChunkloss();
            }

            /// 压缩比（转发到池）：上下文 token 数 / 特征值数。
            double compression_ratio(const lf2p::LosslessPool &pool) const
            {
                return pool.compression_ratio();
            }

            /// **qubit 层面**的压缩比：原始编码要的 qubit 数 / 压缩后要的 qubit 数。
            ///
            /// 这是量子场景里真正的头条指标 —— 「同样的 qubit 预算覆盖多长的
            /// 历史」：原始编码每个 token 要 8 个 qubit（1 byte = 8 bit），
            /// 压缩后每块只占 feature_qubits 个 qubit。
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
            /// 邻域特征 → feature_qubits 维（经典路径用）。
            ///
            /// 用 DCT-II 基投影：把全部 16 维特征摊到每个输出维上（不是只取
            /// 前几维 —— 那样一半的信息会被丢掉，而「丢掉多少」正是压缩比
            /// 与重建保真度要衡量的东西）。投影是确定性的、无参数、可测。
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

            /// 一个块的量子子线路：邻域特征 → 量子特征。
            std::vector<double> encode_block(const std::vector<double> &features,
                                            const std::vector<size_t> &work_qubits)
            {
                const size_t n = feature_qubits_;
                std::vector<double> out(n, 0.0);

                // 1) 特征 → 角度（DCT-II 基投影：用全部 16 维特征，不只前几维）
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

                // 2) 逐个 RY 旋转
                for (size_t i = 0; i < n; ++i)
                    backend_->apply_ry(work_qubits[i], angles[i]);

                // 3) 三角 CNOT 梯（纠缠：不纠缠的话每个读出只是某个线性投影）
                for (size_t i = 0; i + 1 < n; ++i)
                    backend_->apply_cnot(work_qubits[i], work_qubits[i + 1]);

                // 4) 读 ⟨Z⟩ ⇒ 量子特征
                for (size_t i = 0; i < n; ++i)
                    out[i] = backend_->expectation_z(work_qubits[i]);

                // 5) 复位工作 qubit（测量 + 条件 X）供下一块使用。
                //    测量是破坏性的 —— 无妨：特征已经取出来了。
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
            /// 每块几个 token。
            size_t block_size = 4;
            /// 每块几维特征（= 每块占几个上下文 qubit）。
            ///
            /// 默认 2：每块 4 个 token、2 维特征 ⇒ **每个特征承载 2 个
            /// token**。16 个上下文 qubit 能装 8 个特征 = 8 块 = **32 个
            /// token** 的历史，而原始编码同样 16 qubit 只能装 8 个 token ——
            /// 4 倍的覆盖。feature_qubits=1 时是 8 倍（但每块只剩 1 维信息）。
            size_t feature_qubits = 2;
            /// 压缩窗口：取最近多少个 token 进上下文。
            ///
            /// 覆盖能力 = (context_qubits / feature_qubits) × block_size 个 token。
            /// 默认 (16/2)×4 = **32 个 token**（原来是 2 个）—— 同样的 16 个
            /// qubit，覆盖 16 倍长的历史。
            ///
            /// 窗口必须够大，否则特征表装不满 context_qubits 个值：16 个
            /// qubit 需要 16 个特征 = 8 块 = 32 个 token。窗口给小了不会有
            /// 报错，只是**一半的上下文 qubit 拿不到特征** —— 那种错误极难
            /// 察觉（状态没崩、loss 也在降，就是白扔了一半算力）。
            size_t window_tokens = 32;
        };

        /// 统一的上下文编码：token 序列 → 特征 → 上下文态。
        ///
        /// **训练与推理必须走同一个函数** —— 这是本文件存在的集成理由：
        /// 训练侧（qml/Transmitter.cpp 的样本装配）与推理侧（qml/Inference.hpp
        /// 的前向）如果用**不同**的上下文编码，模型就是在训练分布之外被
        /// 推理（train/test skew），输出是垃圾而且**不报错**。把编码收敛到
        /// 一个函数里，skew 就从一个「约定」变成了「结构上不可能」。
        ///
        /// `pool_out` 非空时把池交还调用方（训练侧要靠 sumChunkloss 做
        /// 压缩器的损失；推理侧不需要）。
        inline void encode_context(qhal::IQuantumBackend *be, const ContextConfig &cfg,
                                   const std::vector<uint8_t> &tokens,
                                   const std::vector<size_t> &context_qubits,
                                   lf2p::LosslessPool *pool_out = nullptr)
        {
            if (!be || context_qubits.empty())
                return;

            // 截到窗口（只保留最近的 window_tokens 个 token）
            std::vector<uint8_t> window = tokens;
            if (cfg.window_tokens > 0 && window.size() > cfg.window_tokens)
                window.erase(window.begin(),
                            window.end() - static_cast<long>(cfg.window_tokens));

            lf2p::LosslessPool local_pool;
            lf2p::LosslessPool &pool = pool_out ? *pool_out : local_pool;

            Compressor compressor(be, cfg.block_size, cfg.feature_qubits);
            // 默认经典路径（训练侧每个样本都要算，量子子线路付不起）。
            // 量子路径需要工作 qubit，与上下文 qubit 撞号时不可用 —— 所以
            // 这里显式传空 work_qubits 并保持经典模式。
            compressor.compress(window, pool, {});
            compressor.prepare_context_state(pool, context_qubits);
        }
    }
}