#pragma once

#include <vector>
#include <complex>
#include <cmath>
#include <stdexcept>
#include <random>

namespace qhal
{
    class IdealStateCore
    {
    private:
        std::vector<std::complex<double>> state_;
        size_t n_ = 0;
        std::mt19937 rng_{std::random_device{}()};

        void ensure_qubit(size_t q) const
        {
            if (q >= n_)
                throw std::out_of_range("IdealStateCore: qubit out of range");
        }

    public:
        void allocate(size_t n)
        {
            if (n <= n_)
                return;
            if (n_ == 0)
            {
                n_ = n;
                state_.assign(size_t(1) << n_, {0.0, 0.0});
                state_[0] = {1.0, 0.0};
            }
            else
            {
                std::vector<std::complex<double>> nxt(size_t(1) << n, {0.0, 0.0});
                size_t old = size_t(1) << n_;
                for (size_t i = 0; i < old; ++i)
                    nxt[i] = state_[i];
                state_.swap(nxt);
                n_ = n;
            }
        }

        void apply_x(size_t q)
        {
            ensure_qubit(q);
            size_t m = size_t(1) << q;
            for (size_t i = 0; i < state_.size(); ++i)
                if (i & m)
                    std::swap(state_[i], state_[i ^ m]);
        }

        void apply_h(size_t q)
        {
            ensure_qubit(q);
            const double inv = 1.0 / std::sqrt(2.0);
            size_t m = size_t(1) << q;
            for (size_t i = 0; i < state_.size(); ++i)
            {
                if (i & m)
                    continue;
                size_t j = i ^ m;
                auto a = state_[i], b = state_[j];
                state_[i] = inv * (a + b);
                state_[j] = inv * (a - b);
            }
        }

        void apply_rz(size_t q, double angle)
        {
            ensure_qubit(q);
            const std::complex<double> ph(std::cos(angle), std::sin(angle));
            size_t m = size_t(1) << q;
            for (size_t i = 0; i < state_.size(); ++i)
                if (i & m)
                    state_[i] *= ph;
        }

        void apply_cnot(size_t c, size_t t)
        {
            ensure_qubit(c);
            ensure_qubit(t);
            size_t mc = size_t(1) << c, mt = size_t(1) << t;
            for (size_t i = 0; i < state_.size(); ++i)
                if ((i & mc) && !(i & mt))
                    std::swap(state_[i], state_[i ^ mt]);
        }

        void apply_toffoli(size_t c1, size_t c2, size_t t)
        {
            ensure_qubit(c1);
            ensure_qubit(c2);
            ensure_qubit(t);
            size_t m1 = size_t(1) << c1, m2 = size_t(1) << c2, mt = size_t(1) << t;
            for (size_t i = 0; i < state_.size(); ++i)
                if ((i & m1) && (i & m2) && !(i & mt))
                    std::swap(state_[i], state_[i ^ mt]);
        }

        void apply_z(size_t q)
        {
            ensure_qubit(q);
            size_t m = size_t(1) << q;
            for (size_t i = 0; i < state_.size(); ++i)
                if (i & m)
                    state_[i] = -state_[i];
        }

        void apply_y(size_t q)
        {
            ensure_qubit(q);
            size_t m = size_t(1) << q;
            const std::complex<double> I(0.0, 1.0);
            for (size_t i = 0; i < state_.size(); ++i)
            {
                if (i & m)
                    continue;
                size_t j = i ^ m;
                auto a = state_[i], b = state_[j];
                state_[i] = -I * b;
                state_[j] = I * a;
            }
        }


        void apply_bit_flip(size_t q, double p)
        {
            std::bernoulli_distribution d(p);
            if (d(rng_))
                apply_x(q);
        }

        void apply_phase_flip(size_t q, double p)
        {
            std::bernoulli_distribution d(p);
            if (d(rng_))
                apply_z(q);
        }

        void apply_depolarizing(size_t q, double p)
        {
            std::uniform_real_distribution<double> dist(0.0, 1.0);
            double r = dist(rng_);
            double px = p / 4.0;
            if (r < px)
                apply_x(q);
            else if (r < 2 * px)
                apply_y(q);
            else if (r < 3 * px)
                apply_z(q);
        }

        void apply_amplitude_damping(size_t q, double gamma)
        {
            ensure_qubit(q);
            size_t m = size_t(1) << q;
            double p1 = 0.0;
            for (size_t i = 0; i < state_.size(); ++i)
                if (i & m)
                    p1 += std::norm(state_[i]);

            std::bernoulli_distribution d(gamma * p1);
            if (d(rng_))
            {
                for (size_t i = 0; i < state_.size(); ++i)
                {
                    if (i & m)
                    {
                        state_[i ^ m] = state_[i];
                        state_[i] = std::complex<double>(0.0, 0.0);
                    }
                }
                if (p1 > 0.0)
                {
                    double n = 1.0 / std::sqrt(p1);
                    for (size_t i = 0; i < state_.size(); ++i)
                        state_[i] *= n;
                }
            }
            else
            {
                double denom = std::sqrt(1.0 - gamma * p1);
                if (denom <= 0.0)
                    return;
                double s = std::sqrt(1.0 - gamma) / denom;
                double s0 = 1.0 / denom;
                for (size_t i = 0; i < state_.size(); ++i)
                {
                    if (i & m)
                        state_[i] *= s;
                    else
                        state_[i] *= s0;
                }
            }
        }

        void apply_noise_channel(size_t q, int channel, double param)
        {
            ensure_qubit(q);
            switch (channel)
            {
                case 0: apply_depolarizing(q, param); break;
                case 1: apply_phase_flip(q, param); break;
                case 2: apply_amplitude_damping(q, param); break;
                case 3:
                {
                    std::bernoulli_distribution d(param);
                    if (d(rng_)) apply_x(q);
                    break;
                }
                default: break;
            }
        }

        int measure(size_t q)
        {
            ensure_qubit(q);
            size_t m = size_t(1) << q;
            double p1 = 0.0;
            for (size_t i = 0; i < state_.size(); ++i)
                if (i & m)
                    p1 += std::norm(state_[i]);

            std::uniform_real_distribution<double> dist(0.0, 1.0);
            int r = (dist(rng_) < p1) ? 1 : 0;
            double norm = 1.0 / std::sqrt(r ? p1 : (1.0 - p1));
            for (size_t i = 0; i < state_.size(); ++i)
            {
                bool keep = (i & m) ? (r == 1) : (r == 0);
                state_[i] = keep ? state_[i] * norm : std::complex<double>(0.0, 0.0);
            }
            return r;
        }

        double expectation_z(size_t q)
        {
            ensure_qubit(q);
            size_t m = size_t(1) << q;
            double p1 = 0.0;
            for (size_t i = 0; i < state_.size(); ++i)
                if (i & m)
                    p1 += std::norm(state_[i]);
            return 1.0 - 2.0 * p1;
        }

        size_t size() const { return n_; }
    };
}