#pragma once
//
// TQNFLayer.hpp —— 拓扑量子神经场训练器（TQNF 范式端到端闭环）
//
// 把三大支柱组合成可训练的量子层，取代经典「层/权重」：
//   time   → 耗散/相干演化流（dissipate 信道 + expm_hermitian 相干流）
//   coord  → 拓扑并置（compose = kron）
//   thread → 纠缠（reduce = 部分迹，entanglement = 纠缠熵）
// 训练     = 完整 QGT 自然梯度（无权重矩阵，见 numqk/QuantumGeometry.hpp）
//
// 对应 TQNF 设计法则：几何-贫瘠高原对偶律（完整 Fubini-Study 度量）、
// 拓扑-表征分离律（表征由拓扑/纠缠决定）、耗散-可训练性对偶律（信道为资源）。
// 全部为纯线性代数 + 头文件，后端无关。
//
#include "../numqk/QuantumGeometry.hpp"
#include <functional>
#include <vector>
#include <cmath>
#include <iostream>

namespace qml
{

    // ─────────────────────────────────────────────────────────────
    // TQNFTrainer：给定态准备 state_fn(θ) → ket，用完整 QGT 自然梯度
    // 最小化 loss_fn(θ)。导数态用中心差分估计（对任意态准备约定精确）。
    // ─────────────────────────────────────────────────────────────
    template <typename T>
    class TQNFTrainer
    {
    public:
        using StateFn = std::function<numqk::CTensor(const numqk::Tensor<T> &)>;
        using LossFn = std::function<double(const numqk::Tensor<T> &)>;

    private:
        StateFn state_fn_;
        LossFn loss_fn_;
        double diff_step_ = 1e-4;

        // 中心差分导数态 |∂i ψ⟩ ≈ [ψ(θ+δ) - ψ(θ-δ)] / (2δ)
        std::vector<numqk::CTensor> derivative_kets(numqk::Tensor<T> &theta,
                                                     const numqk::CTensor &psi)
        {
            const size_t n = theta.size();
            std::vector<numqk::CTensor> dpsi;
            dpsi.reserve(n);
            for (size_t i = 0; i < n; ++i)
            {
                const T orig = theta.data()[i];

                theta.data()[i] = orig + T(diff_step_);
                numqk::CTensor plus = state_fn_(theta);
                theta.data()[i] = orig - T(diff_step_);
                numqk::CTensor minus = state_fn_(theta);
                theta.data()[i] = orig; // 恢复

                numqk::CTensor d({psi.size()}, false);
                for (size_t k = 0; k < psi.size(); ++k)
                    d.data()[k] = (plus.data()[k] - minus.data()[k]) / T(2.0 * diff_step_);
                dpsi.push_back(std::move(d));
            }
            return dpsi;
        }

    public:
        TQNFTrainer(StateFn sf, LossFn lf) : state_fn_(std::move(sf)), loss_fn_(std::move(lf)) {}

        // 完整 QGT（Fubini-Study 度量）
        numqk::Tensor<double> metric(numqk::Tensor<T> &theta)
        {
            numqk::CTensor psi = state_fn_(theta);
            auto dpsi = derivative_kets(theta, psi);
            return numqk::fubini_study(dpsi, psi);
        }

        // 欧氏梯度 ∇L（中心差分）
        numqk::Tensor<double> euclidean_gradient(numqk::Tensor<T> &theta)
        {
            const size_t n = theta.size();
            numqk::Tensor<double> grad({n}, false);
            for (size_t i = 0; i < n; ++i)
            {
                const T orig = theta.data()[i];
                theta.data()[i] = orig + T(diff_step_);
                const double lp = loss_fn_(theta);
                theta.data()[i] = orig - T(diff_step_);
                const double lm = loss_fn_(theta);
                theta.data()[i] = orig;
                grad.data()[i] = (lp - lm) / (2.0 * diff_step_);
            }
            return grad;
        }

        // 自然梯度一步：θ ← θ - lr · G⁺ ∇L
        void step(numqk::Tensor<T> &theta, double lr)
        {
            numqk::Tensor<double> G = metric(theta);
            numqk::Tensor<double> grad = euclidean_gradient(theta);
            numqk::Tensor<double> dtheta = numqk::natural_gradient(G, grad);
            for (size_t i = 0; i < theta.size(); ++i)
                theta.data()[i] -= T(lr) * dtheta.data()[i];
        }

        // 训练循环，返回最终损失
        double train(numqk::Tensor<T> &theta, int epochs, double lr, bool verbose = false)
        {
            double final_loss = loss_fn_(theta);
            for (int e = 0; e < epochs; ++e)
            {
                step(theta, lr);
                final_loss = loss_fn_(theta);
                if (verbose && (e + 1) % 20 == 0)
                    std::cout << "  [TQNF] epoch " << e + 1 << "/" << epochs
                              << " loss=" << final_loss << "\n";
            }
            return final_loss;
        }
    };

    // ─────────────────────────────────────────────────────────────
    // 拓扑 / 耗散支柱的便捷原语（对应 @layer 的 coord/thread/time）
    // ─────────────────────────────────────────────────────────────
    // coord：子系统并置（Kronecker 积）
    inline numqk::CTensor compose(const numqk::CTensor &a, const numqk::CTensor &b)
    {
        return numqk::kron(a, b);
    }

    // thread：约化密度矩阵（部分迹）
    inline numqk::CTensor reduce(const numqk::CTensor &rho,
                                 const std::vector<size_t> &dims,
                                 const std::vector<bool> &keep)
    {
        return numqk::partial_trace(rho, dims, keep);
    }

    // thread：纠缠熵
    inline double entanglement(const numqk::CTensor &psi,
                               const std::vector<size_t> &dims,
                               const std::vector<bool> &keep)
    {
        return numqk::entanglement_entropy(psi, dims, keep);
    }

    // time：耗散信道（channel: 0=去极化, 1=退相, 2=振幅阻尼）
    inline numqk::CTensor dissipate(const numqk::CTensor &rho, int channel, double p)
    {
        switch (channel)
        {
        case 0:
            return numqk::depolarizing(rho, p);
        case 1:
            return numqk::dephasing(rho, p);
        case 2:
        default:
            return numqk::amplitude_damping(rho, p);
        }
    }
}