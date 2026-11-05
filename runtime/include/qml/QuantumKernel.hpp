#pragma once
//
// QuantumKernel.hpp —— 量子核方法（TQNF 隐式模型路径）
//
// 与变分电路（显式模型，见 TQNFLayer.hpp）互补的两条量子机器学习路径：
//   • 显式模型：可训练参数化态 + 完整 QGT 自然梯度（几何支柱）；
//   • 隐式模型：固定特征映射 φ(x) 的保真度核 k(x,x') = |⟨φ(x)|φ(x')⟩|²，
//     学习只作用于线性读出（凸优化，**无贫瘠高原**）。
//
// 对应 TQNF 设计法则「耗散-可训练性对偶律」：表征（特征映射）固定，
// 学习（线性读出）可训练。消费 numqk 的 fidelity / sym_pinv 原语。
// 全部为纯线性代数，后端无关。
//
#include "../numqk/QuantumGeometry.hpp"
#include <functional>
#include <vector>
#include <cmath>

namespace qml
{

    // ─────────────────────────────────────────────────────────────
    // QuantumKernel：保真度核 k(a,b) = |⟨φ(a)|φ(b)⟩|²
    // FeatureMap：经典数据 x → 量子态 |φ(x)⟩（numqk::CTensor ket）
    // ─────────────────────────────────────────────────────────────
    template <typename T>
    class QuantumKernel
    {
    public:
        using FeatureMap = std::function<numqk::CTensor(const std::vector<T> &)>;

    private:
        FeatureMap feature_map_;

    public:
        explicit QuantumKernel(FeatureMap fm) : feature_map_(std::move(fm)) {}

        // 单点核值
        double eval(const std::vector<T> &a, const std::vector<T> &b) const
        {
            numqk::CTensor pa = feature_map_(a);
            numqk::CTensor pb = feature_map_(b);
            numqk::CTensor rho_a = numqk::pure_state_density(pa);
            numqk::CTensor rho_b = numqk::pure_state_density(pb);
            return numqk::fidelity(rho_a, rho_b);
        }

        // Gram 矩阵 K[i][j] = k(x_i, x_j)（对称、半正定）
        numqk::Tensor<double> gram(const std::vector<std::vector<T>> &X) const
        {
            const size_t n = X.size();
            numqk::Tensor<double> K({n, n}, false);
            for (size_t i = 0; i < n; ++i)
                for (size_t j = 0; j < n; ++j)
                    K.data()[i * n + j] = eval(X[i], X[j]);
            // 显式对称化（消除特征分解的数值非对称）
            for (size_t i = 0; i < n; ++i)
                for (size_t j = i + 1; j < n; ++j)
                {
                    double avg = 0.5 * (K.data()[i * n + j] + K.data()[j * n + i]);
                    K.data()[i * n + j] = K.data()[j * n + i] = avg;
                }
            return K;
        }
    };

    // ─────────────────────────────────────────────────────────────
    // KernelRidgeClassifier：核岭回归分类器
    //   α = (K + λI)⁻¹ y（凸优化，闭式解）
    //   ŷ(x) = Σ_i α_i k(x_i, x)，分类取 sign
    // ─────────────────────────────────────────────────────────────
    template <typename T>
    class KernelRidgeClassifier
    {
    private:
        QuantumKernel<T> kernel_;
        double lambda_;
        std::vector<std::vector<T>> support_;
        numqk::Tensor<double> alpha_;
        bool trained_ = false;

    public:
        explicit KernelRidgeClassifier(QuantumKernel<T> k, double lambda = 1e-4)
            : kernel_(std::move(k)), lambda_(lambda), alpha_({0}, false) {}

        void fit(const std::vector<std::vector<T>> &X, const std::vector<double> &y)
        {
            if (X.empty() || X.size() != y.size())
                throw std::invalid_argument("KernelRidgeClassifier::fit: X/y size mismatch.");

            support_ = X;
            const size_t n = X.size();

            numqk::Tensor<double> K = kernel_.gram(X);
            for (size_t i = 0; i < n; ++i)
                K.data()[i * n + i] += lambda_;

            // α = K⁺ y
            numqk::Tensor<double> Kp = numqk::sym_pinv(K);
            alpha_ = numqk::Tensor<double>({n}, false);
            for (size_t i = 0; i < n; ++i)
            {
                double acc = 0.0;
                for (size_t j = 0; j < n; ++j)
                    acc += Kp.data()[i * n + j] * y[j];
                alpha_.data()[i] = acc;
            }
            trained_ = true;
        }

        // 决策值（>0 → +1 类，<0 → -1 类）
        double decision(const std::vector<T> &x) const
        {
            if (!trained_)
                throw std::runtime_error("KernelRidgeClassifier: not trained.");
            double s = 0.0;
            for (size_t i = 0; i < support_.size(); ++i)
                s += alpha_.data()[i] * kernel_.eval(support_[i], x);
            return s;
        }

        double predict(const std::vector<T> &x) const
        {
            return decision(x) > 0.0 ? 1.0 : -1.0;
        }

        bool trained() const { return trained_; }
    };

}