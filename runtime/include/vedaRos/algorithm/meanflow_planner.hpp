#pragma once
#include <vector>
#include <functional>
#include <utility>
#include <iostream>
#include "../../qhal/IQuantumBackend.hpp"

namespace vedaros::algorithm
{

    class MeanFlowPlanner
    {
    public:
        static constexpr size_t kDim = 6;

        using EndpointPredictor =
            std::function<std::vector<double>(const std::vector<double> &, double)>;

    private:
        qhal::IQuantumBackend *backend_;
        EndpointPredictor predictor_;
        bool injected_ = false;

        static std::vector<double> regularized_predictor(const std::vector<double> &H, double)
        {
            constexpr double kLambda = 0.98;
            std::vector<double> H1(H.size());
            for (size_t i = 0; i < H.size(); ++i)
                H1[i] = H[i] * kLambda;
            return H1;
        }

    public:
        explicit MeanFlowPlanner(qhal::IQuantumBackend *backend,
                                 EndpointPredictor pred = nullptr)
            : backend_(backend),
              predictor_(pred ? std::move(pred) : regularized_predictor),
              injected_(pred != nullptr)
        {
            if (!injected_) {
                std::cerr << "[vedaRos.mf][info] no endpoint predictor injected — "
                          << "using the REGULARIZED default (H_1 = 0.98·H_t, drifts toward "
                          << "the reference pose 0). Inject a real predictor (e.g. QLM "
                          << "VectorQuantumLayer + parameter-shift outputting 6-D <Z> "
                          << "expectations) to plan toward arbitrary targets.\n";
            }
            std::cout << "[vedaRos.mf] Lie-group MeanFlow planner online (SO(3)xR^3).\n";
        }

        bool has_predictor() const { return injected_; }

        std::vector<double> predict_endpoint(const std::vector<double> &H_t, double t) const
        {
            return predictor_(H_t, t);
        }

        std::vector<double> flow(const std::vector<double> &H_s,
                                 double s, double t, double t_end = 1.0) const
        {
            std::vector<double> H1 = predict_endpoint(H_s, s);
            std::vector<double> out(kDim);
            double denom = t_end - s;
            if (denom < 1e-12)
                denom = 1e-12;
            double alpha = (t - s) / denom;
            for (size_t i = 0; i < kDim; ++i)
                out[i] = H_s[i] + alpha * (H1[i] - H_s[i]);
            return out;
        }

        std::vector<double> sample(const std::vector<double> &H0, int T = 5) const
        {
            std::vector<double> H = H0;
            for (int k = 0; k < T; ++k)
            {
                double t_k = static_cast<double>(k) / T;
                double t_k1 = static_cast<double>(k + 1) / T;
                H = flow(H, t_k, t_k1);
            }
            return H;
        }
    };
}