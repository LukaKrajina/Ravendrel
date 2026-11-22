#pragma once
#include "../numqk/QuantumGeometry.hpp"
#include <functional>
#include <vector>
#include <cmath>

namespace qml
{

    template <typename T>
    class QuantumKernel
    {
    public:
        using FeatureMap = std::function<numqk::CTensor(const std::vector<T> &)>;

    private:
        FeatureMap feature_map_;

    public:
        explicit QuantumKernel(FeatureMap fm) : feature_map_(std::move(fm)) {}

        double eval(const std::vector<T> &a, const std::vector<T> &b) const
        {
            numqk::CTensor pa = feature_map_(a);
            numqk::CTensor pb = feature_map_(b);
            numqk::CTensor rho_a = numqk::pure_state_density(pa);
            numqk::CTensor rho_b = numqk::pure_state_density(pb);
            return numqk::fidelity(rho_a, rho_b);
        }

        numqk::Tensor<double> gram(const std::vector<std::vector<T>> &X) const
        {
            const size_t n = X.size();
            numqk::Tensor<double> K({n, n}, false);
            for (size_t i = 0; i < n; ++i)
                for (size_t j = 0; j < n; ++j)
                    K.data()[i * n + j] = eval(X[i], X[j]);
            for (size_t i = 0; i < n; ++i)
                for (size_t j = i + 1; j < n; ++j)
                {
                    double avg = 0.5 * (K.data()[i * n + j] + K.data()[j * n + i]);
                    K.data()[i * n + j] = K.data()[j * n + i] = avg;
                }
            return K;
        }
    };

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