#pragma once
#include "../numqk/QuantumGeometry.hpp"
#include <functional>
#include <vector>
#include <cmath>
#include <iostream>

namespace qml
{

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
                theta.data()[i] = orig;

                numqk::CTensor d({psi.size()}, false);
                for (size_t k = 0; k < psi.size(); ++k)
                    d.data()[k] = (plus.data()[k] - minus.data()[k]) / T(2.0 * diff_step_);
                dpsi.push_back(std::move(d));
            }
            return dpsi;
        }

    public:
        TQNFTrainer(StateFn sf, LossFn lf) : state_fn_(std::move(sf)), loss_fn_(std::move(lf)) {}

        numqk::Tensor<double> metric(numqk::Tensor<T> &theta)
        {
            numqk::CTensor psi = state_fn_(theta);
            auto dpsi = derivative_kets(theta, psi);
            return numqk::fubini_study(dpsi, psi);
        }

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

        void step(numqk::Tensor<T> &theta, double lr)
        {
            numqk::Tensor<double> G = metric(theta);
            numqk::Tensor<double> grad = euclidean_gradient(theta);
            numqk::Tensor<double> dtheta = numqk::natural_gradient(G, grad);
            for (size_t i = 0; i < theta.size(); ++i)
                theta.data()[i] -= T(lr) * dtheta.data()[i];
        }

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

    inline numqk::CTensor compose(const numqk::CTensor &a, const numqk::CTensor &b)
    {
        return numqk::kron(a, b);
    }

    inline numqk::CTensor reduce(const numqk::CTensor &rho,
                                 const std::vector<size_t> &dims,
                                 const std::vector<bool> &keep)
    {
        return numqk::partial_trace(rho, dims, keep);
    }

    inline double entanglement(const numqk::CTensor &psi,
                               const std::vector<size_t> &dims,
                               const std::vector<bool> &keep)
    {
        return numqk::entanglement_entropy(psi, dims, keep);
    }

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