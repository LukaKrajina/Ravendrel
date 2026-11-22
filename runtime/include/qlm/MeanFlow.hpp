#pragma once
#include <vector>
#include <functional>
#include <utility>
#include <cstddef>

namespace qlm
{
    class MeanFlowHead
    {
    public:
        using EndpointPredictor =
            std::function<std::vector<double>(const std::vector<double> &, double)>;

    private:
        EndpointPredictor predictor_;
        double depolarization_;

    public:
        explicit MeanFlowHead(EndpointPredictor pred, double depolarization = 0.0)
            : predictor_(std::move(pred)), depolarization_(depolarization) {}

        std::vector<double> predict_endpoint(const std::vector<double> &H_t, double t) const
        {
            return predictor_(H_t, t);
        }

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