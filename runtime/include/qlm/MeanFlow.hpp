#pragma once
#include <vector>
#include <functional>
#include <utility>
#include <cstddef>

namespace qlm
{
    // ─────────────────────────────────────────────────────────────
    // 端点参数化流 + 非酉前向扩散信道（QGDM 式）
    //
    // 「前向非酉信道 → 完全混合态；反向可训练信道 → 恢复目标态」结构：
    //
    //   前向扩散信道：   Φ_fwd(ρ, t) = (1-t)·ρ  +  t·(I/d)
    //     对 ⟨Z⟩ 期望而言，完全混合态 I/d 对应 ⟨Z⟩=0，故
    //       H_t = (1 - t) · H_1 ，t∈[0,1]
    //     （t=0 干净端点，t=1 完全混合态）——非酉，等价于退极化扩散。
    //
    //   反向（去噪）：网络预测干净端点 Ĥ_1 = X_θ(H_t, t)，
    //     诱导流映射 Φ(s,t)(H_s) = H_s + (t-s)/(1-s)·(Ĥ_1 - H_s)，
    //     半群一致性 Φ(s,t) = Φ(r,t)∘Φ(s,r) 保证任意步数采样一致。
    //
    // 对比旧实现（纯欧氏插值）：此处把「加噪到先验」显式建模为前向非酉
    // 扩散信道，使训练时能构造 (H_t, t) 加噪样本对，而非仅在干净端点回归。
    // ─────────────────────────────────────────────────────────────
    class MeanFlowHead
    {
    public:
        // 端点预测器：给定插值态 H_t 与流时间 t，返回干净目标 Ĥ_1
        using EndpointPredictor =
            std::function<std::vector<double>(const std::vector<double> &, double)>;

    private:
        EndpointPredictor predictor_;
        double depolarization_; // 前向扩散信道的退极化强度 [0,1)

    public:
        explicit MeanFlowHead(EndpointPredictor pred, double depolarization = 0.0)
            : predictor_(std::move(pred)), depolarization_(depolarization) {}

        // 端点预测
        std::vector<double> predict_endpoint(const std::vector<double> &H_t, double t) const
        {
            return predictor_(H_t, t);
        }

        // ── 前向扩散信道（非酉）─────────────────────────────────
        // 把干净端点 H_1 退化为扩散态 H_t：
        //   H_t = (1 - t·d) · H_1
        // d=depolarization_ 控制扩散速率；d=0 时退化为恒等（无扩散）。
        // 这是 QGDM「前向非酉信道 → 完全混合态」的期望空间实现。
        std::vector<double> forward_diffuse(const std::vector<double> &H_1, double t) const
        {
            std::vector<double> out(H_1.size());
            double atten = 1.0 - t * depolarization_;
            if (atten < 0.0)
                atten = 0.0;
            for (size_t i = 0; i < out.size(); ++i)
                out[i] = atten * H_1[i];
            return out;
        }

        // ── 反向流映射（端点回归）───────────────────────────────
        // 网络预测干净目标 Ĥ_1 = X_θ(H_t, t)，诱导流：
        //   Φ(s,t)(H_s) = H_s + (t-s)/(1-s)·(Ĥ_1 - H_s)
        std::vector<double> flow(const std::vector<double> &H_s,
                                 double s, double t) const
        {
            std::vector<double> H1 = predictor_(H_s, s);
            std::vector<double> out(H_s.size());
            double denom = 1.0 - s;
            if (denom < 1e-12)
                denom = 1e-12;
            double alpha = (t - s) / denom;
            for (size_t i = 0; i < out.size(); ++i)
                out[i] = H_s[i] + alpha * (H1[i] - H_s[i]);
            return out;
        }

        // ── 半群一致性损失 ──────────────────────────────────────
        //   L_semi = ‖ Φ(r,t)∘Φ(s,r)(H_s) - Φ(s,t)(H_s) ‖²
        double semi_group_loss(const std::vector<double> &H_s,
                               double s, double r, double t) const
        {
            auto H_r = flow(H_s, s, r);
            auto H_t_via_r = flow(H_r, r, t);
            auto H_t_direct = flow(H_s, s, t);
            double loss = 0.0;
            for (size_t i = 0; i < H_s.size(); ++i)
            {
                double d = H_t_via_r[i] - H_t_direct[i];
                loss += d * d;
            }
            return loss;
        }

        // ── 去噪训练损失（QGDM 式）──────────────────────────────
        // 给定干净端点 H_1 与随机扩散时间 t，构造加噪样本 H_t，
        // 端点预测器需从 H_t 恢复 H_1：
        //   L_denoise = ‖ predict_endpoint(forward_diffuse(H_1, t), t) - H_1 ‖²
        double denoise_loss(const std::vector<double> &H_1, double t) const
        {
            auto H_t = forward_diffuse(H_1, t);
            auto pred = predictor_(H_t, t);
            double loss = 0.0;
            for (size_t i = 0; i < H_1.size(); ++i)
            {
                double d = pred[i] - H_1[i];
                loss += d * d;
            }
            return loss;
        }
    };
}