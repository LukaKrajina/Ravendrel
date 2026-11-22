#pragma once
#include <vector>
#include <cmath>

namespace qbns {
    struct LIFNeuron {
        double tau   = 0.9;
        double theta = 1.0;
        double V     = 0.0;

        bool step(double I) {
            V = tau * V + I;
            if (V > theta) { V = 0.0; return true; }
            return false;
        }
    };

    inline double nef_encode(const std::vector<double>& e, double alpha, double bias,
                            const std::vector<double>& x) {
        double dot = 0.0;
        for (size_t i = 0; i < e.size(); ++i) dot += e[i] * x[i];
        return alpha * dot + bias;
    }

    inline double nef_decode(const std::vector<double>& decoders,
                            const std::vector<double>& activities) {
        double out = 0.0;
        for (size_t i = 0; i < decoders.size(); ++i) out += decoders[i] * activities[i];
        return out;
    }

    struct Gaussian { double mean = 0.0; double cov = 1.0; };

    inline Gaussian ukf_approx(const Gaussian& prior, double (*f)(double), double obs_cov) {
        double s    = std::sqrt(prior.cov);
        double y0   = f(prior.mean - s);
        double y1   = f(prior.mean + s);
        double mean = 0.5 * (y0 + y1);
        double var  = 0.25 * (y1 - y0) * (y1 - y0) + obs_cov;
        return Gaussian{mean, var};
    }

    inline Gaussian gaussian_product(const Gaussian& a, const Gaussian& b) {
        double prec = 1.0 / a.cov + 1.0 / b.cov;
        double mean = (a.mean / a.cov + b.mean / b.cov) / prec;
        return Gaussian{mean, 1.0 / prec};
    }


    inline double lif_step(double v, double current, double tau, double theta) {
        double V = tau * v + current;
        if (V > theta) return 0.0;
        return V;
    }
}