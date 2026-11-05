#pragma once
#include <vector>
#include <cstddef>
#include <cstdint>

namespace qlm
{

    // ─────────────────────────────────────────────────────────────
    // 上下文未来块预测
    //
    // 辅助头不直接访问上下文（人类视频/意识态）
    // 只能通过主线路输出的融合表示 φ 间接获取任务信息
    // 强制主分支真正利用上下文，抑制「从已见任务学习捷径」
    //
    // 跨步未来块索引：j_k = (i+1) + 1 + (k-1)·s
    // 汇总损失：       L_ifp = Σ_k w_k · L_fm(x^{j_k}; φ^{i+1}, ...)
    // ─────────────────────────────────────────────────────────────
    struct IFPHead
    {
        int K = 4;                                        // 未来块数
        int s = 2;                                        // 时间步幅
        std::vector<double> w = {0.5, 0.25, 0.15, 0.15}; // 损失权重（降序衰减）

        // 第 k 个未来目标块索引
        int future_chunk_index(int current_i, int k) const
        {
            return (current_i + 1) + 1 + (k - 1) * s;
        }

        // 单个未来块的流匹配损失（端点回归形式）
        double chunk_loss(const double *pred, uint8_t target_token, int k) const
        {
            double loss = 0.0;
            for (size_t j = 0; j < 8; ++j)
            {
                double x1 = 2.0 * ((target_token >> j) & 1) - 1.0;
                double diff = pred[j] - x1;
                loss += diff * diff;
            }
            loss /= 8.0;
            return (k < static_cast<int>(w.size())) ? w[k] * loss : 0.0;
        }

        // 汇总 IFP 损失
        double ifp_loss(const std::vector<std::vector<double>> &future_preds,
                        const std::vector<uint8_t> &future_targets) const
        {
            double total = 0.0;
            for (int k = 0; k < static_cast<int>(future_preds.size()) &&
                            k < static_cast<int>(future_targets.size()); ++k)
            {
                total += chunk_loss(future_preds[k].data(), future_targets[k], k);
            }
            return total;
        }
    };
}