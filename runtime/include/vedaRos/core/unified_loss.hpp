#pragma once
#include <vector>
#include <numeric>
#include <execution>
#include <algorithm>
#include <utility>

namespace vedaros::core
{

    class AdaptiveLossWeights
    {
    private:
        std::vector<double> lambda_;
        std::vector<double> lambda_base_;
        double beta_ = 0.1;

    public:
        explicit AdaptiveLossWeights(std::vector<double> base)
            : lambda_(base), lambda_base_(std::move(base)) {}

        void update(const std::vector<double> &grad_norms)
        {
            if (grad_norms.empty())
                return;

            double mean = std::reduce(std::execution::par, grad_norms.begin(), grad_norms.end(),0.0)
                          / grad_norms.size();
            for (size_t i = 0; i < grad_norms.size(); ++i)
            {
                double n_i = std::max(grad_norms[i], 1e-8);
                double lambda_new = lambda_base_[i] * (mean / n_i);
                lambda_[i] = (1.0 - beta_) * lambda_[i] + beta_ * lambda_new;
            }
        }

        const std::vector<double> &weights() const { return lambda_; }

        double weight(size_t i) const
        {
            return (i < lambda_.size()) ? lambda_[i] : 1.0;
        }

        double combine(const std::vector<double> &losses) const
        {
            double total = 0.0;
            for (size_t i = 0; i < losses.size() && i < lambda_.size(); ++i)
                total += lambda_[i] * losses[i];
            return total;
        }
    };

    inline std::vector<double> default_loss_weights()
    {
        return {1.0, 0.1, 0.5, 0.3, 1.0, 0.05, 0.01, 0.05};
    }
}