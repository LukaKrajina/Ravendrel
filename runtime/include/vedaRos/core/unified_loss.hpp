#pragma once
#include <vector>
#include <numeric>
#include <execution>
#include <algorithm>
#include <utility>

namespace vedaros::core
{

    // ─────────────────────────────────────────────────────────────
    // 自适应损失权重（梯度范数反比归一化 + EMA）
    //
    // 统一总损失：
    //   L_total = λ_fm·L_fm + λ_semi·L_semi + λ_grpo·L_grpo
    //           + λ_ifp·L_ifp + λ_mf·L_mf + λ_topo·L_topo + λ_metric·R_metric
    //           + λ_lf2c·L_lf2c
    //
    // 其中 L_lf2c 是上下文压缩器的重建损失（qlm::lf2p::LosslessPool 的
    // sumChunkloss）—— 压缩器与主任务联合训练。
    //
    // 每个训练步根据各损失的梯度范数 n_i = ‖∇_θ L_i‖ 动态调节 λ_i，使各损失贡献均衡。
    // GradNorm的简化版省去外层反向传播，适配 quantum parameter-shift 的高计算成本。
    // ─────────────────────────────────────────────────────────────
    class AdaptiveLossWeights
    {
    private:
        std::vector<double> lambda_;       // 当前权重
        std::vector<double> lambda_base_;  // 默认超参
        double beta_ = 0.1;                // EMA 平滑系数

    public:
        explicit AdaptiveLossWeights(std::vector<double> base)
            : lambda_(base), lambda_base_(std::move(base)) {}

        // 输入各损失梯度范数，反比归一化 + EMA 软更新
        void update(const std::vector<double> &grad_norms)
        {
            if (grad_norms.empty())
                return;

            double mean = std::reduce(std::execution::par, grad_norms.begin(), grad_norms.end(),0.0)
                          / grad_norms.size();
            for (size_t i = 0; i < grad_norms.size(); ++i)
            {
                double n_i = std::max(grad_norms[i], 1e-8); // 防除零
                double lambda_new = lambda_base_[i] * (mean / n_i);
                lambda_[i] = (1.0 - beta_) * lambda_[i] + beta_ * lambda_new;
            }
        }

        const std::vector<double> &weights() const { return lambda_; }

        double weight(size_t i) const
        {
            return (i < lambda_.size()) ? lambda_[i] : 1.0;
        }

        // 统一总损失：L_total = Σ_i λ_i · L_i
        double combine(const std::vector<double> &losses) const
        {
            double total = 0.0;
            for (size_t i = 0; i < losses.size() && i < lambda_.size(); ++i)
                total += lambda_[i] * losses[i];
            return total;
        }
    };

    // 默认超参（可调）：λ_fm=1.0, λ_semi=0.1, λ_grpo=0.5, λ_ifp=0.3,
    //                 λ_mf=1.0, λ_topo=0.05, λ_metric=0.01, λ_lf2c=0.05
    //
    // λ_lf2c 是**上下文压缩器**的重建损失（qlm::lf2p::LosslessPool 的
    // sumChunkloss）：它量化「只凭特征能重建多少原文」。放进统一总损失，
    // 压缩器就和主任务**联合训练** —— 为了重建得更准，特征会自己学会
    // 保留更有信息量的那部分（这正是 LF2C 的训练信号）。
    //
    // 顺序即 losses 向量的顺序：调用方传 losses 时第 8 项必须是
    // sumChunkloss（没有压缩器时传 0.0 —— 权重乘 0 不影响总损失）。
    inline std::vector<double> default_loss_weights()
    {
        return {1.0, 0.1, 0.5, 0.3, 1.0, 0.05, 0.01, 0.05};
    }
}