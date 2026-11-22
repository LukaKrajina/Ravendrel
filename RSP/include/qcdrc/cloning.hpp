#pragma once
#include <vector>
#include <utility>
#include <algorithm>
#include <cmath>
#include <iostream>
#include "hardware/observability.hpp"
#include "numqk/Numqk.hpp"
#include "qhal/IQuantumBackend.hpp"
#include "teleop.hpp"

namespace rvdlrsp::qcdrc
{

    struct Demonstration
    {
        std::vector<double> observation;
        std::vector<double> action;
    };

    class BehaviorCloner
    {
    private:
        std::vector<Demonstration> dataset_;
        qhal::IQuantumBackend *backend_ = nullptr;
        numqk::Tensor<double> policy_weights_{std::vector<size_t>{1}, false};
        size_t max_qubits_ = 8;
        int num_shots_ = 64;

        static double encode_angle(double v) { return std::atan(v) * 2.0; }

        static double euclidean(const std::vector<double> &a, const std::vector<double> &b)
        {
            size_t n = std::min(a.size(), b.size());
            double s = 0.0;
            for (size_t i = 0; i < n; ++i)
            {
                double d = a[i] - b[i];
                s += d * d;
            }
            return std::sqrt(s);
        }

        double rbf_kernel(const std::vector<double> &x, const std::vector<double> &y) const
        {
            double sigma = policy_weights_.data()[0];
            if (sigma <= 0.0) sigma = 1.0;
            double d2 = euclidean(x, y);
            d2 *= d2;
            return std::exp(-d2 / (2.0 * sigma * sigma));
        }

        double quantum_kernel(const std::vector<double> &x, const std::vector<double> &y) const
        {
            size_t n = std::min({x.size(), y.size(), max_qubits_});
            if (n == 0) return 1.0;

            backend_->allocate_qubits(n);

            int zero_count = 0;
            for (int shot = 0; shot < num_shots_; ++shot)
            {
                for (size_t i = 0; i < n; ++i)
                {
                    backend_->apply_h(i);
                    backend_->apply_rz(i, encode_angle(y[i]));
                }
                for (size_t i = 0; i < n; ++i)
                {
                    backend_->apply_rz(i, -encode_angle(x[i]));
                    backend_->apply_h(i);
                }
                bool all_zero = true;
                for (size_t i = 0; i < n; ++i)
                {
                    int m = backend_->measure(i);
                    if (m != 0)
                    {
                        all_zero = false;
                        backend_->apply_x(i);
                    }
                }
                if (all_zero) ++zero_count;
            }
            return static_cast<double>(zero_count) / static_cast<double>(num_shots_);
        }

        double kernel(const std::vector<double> &x, const std::vector<double> &y) const
        {
            if (backend_) return quantum_kernel(x, y);
            return rbf_kernel(x, y);
        }

        double estimate_bandwidth() const
        {
            double sum_dist = 0.0;
            size_t cnt = 0;
            for (size_t i = 0; i < dataset_.size(); ++i)
                for (size_t j = i + 1; j < dataset_.size(); ++j)
                {
                    sum_dist += euclidean(dataset_[i].observation, dataset_[j].observation);
                    ++cnt;
                }
            double sigma = (cnt > 0) ? (sum_dist / static_cast<double>(cnt)) : 1.0;
            return sigma < 1e-3 ? 1e-3 : sigma;
        }

    public:
        BehaviorCloner()
        {
            RVDLRSP_INFO("qcdrc") << "Behavioral cloning via quantum estimation ready.";
        }

        void set_backend(qhal::IQuantumBackend *be) { backend_ = be; }
        void set_kernel_config(size_t max_qubits, int shots)
        {
            max_qubits_ = max_qubits;
            num_shots_ = shots > 0 ? shots : 1;
        }

        void collect(const std::vector<double> &observation,
                     const std::vector<double> &action)
        {
            dataset_.push_back({observation, action});
        }

        size_t dataset_size() const { return dataset_.size(); }

        void train(int epochs)
        {
            if (dataset_.empty())
                return;
            RVDLRSP_INFO("qcdrc") << "Training policy from " << dataset_.size()
                                   << " demos (" << epochs << " epochs)...";

            policy_weights_.data()[0] = estimate_bandwidth();

            if (backend_ && !dataset_.front().observation.empty())
            {
                size_t samples = std::min<size_t>(dataset_.size(), 4);
                double self = 0.0;
                for (size_t i = 0; i < samples; ++i)
                    self += quantum_kernel(dataset_[i].observation, dataset_[i].observation);
                self /= static_cast<double>(samples);
                RVDLRSP_INFO("qcdrc") << "Quantum kernel self-overlap (calibration) = "
                                       << self << " (ideal 1.0).";
            }

            for (int e = 1; e < epochs; ++e)
                policy_weights_.data()[0] *= 0.99;

            RVDLRSP_INFO("qcdrc") << "Policy distribution estimated (bandwidth="
                                   << policy_weights_.data()[0] << ").";
        }

        std::vector<double> predict(const std::vector<double> &observation)
        {
            if (dataset_.empty())
                return {};

            size_t n = dataset_[0].action.size();
            std::vector<double> num(n, 0.0);
            double den = 0.0;

            for (const auto &d : dataset_)
            {
                double w = kernel(observation, d.observation);
                for (size_t i = 0; i < n; ++i)
                    num[i] += w * d.action[i];
                den += w;
            }

            if (den <= 1e-12)
            {
                std::vector<double> mean(n, 0.0);
                for (const auto &d : dataset_)
                    for (size_t i = 0; i < n; ++i)
                        mean[i] += d.action[i];
                for (auto &v : mean)
                    v /= static_cast<double>(dataset_.size());
                return mean;
            }

            std::vector<double> out(n, 0.0);
            for (size_t i = 0; i < n; ++i)
                out[i] = num[i] / den;
            return out;
        }
    };
}