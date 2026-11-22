#pragma once
#include "IQuantumBackend.hpp"
#include <cstddef>
#include <cmath>
#include <random>
#include <vector>
#include <map>
#include <utility>
#include <mutex>
#include <stdexcept>
#include <iostream>
#include <complex>

namespace qhal
{

    struct GaussianMode
    {
        struct Peak
        {
            int index = 0;
            double re = 0.0;
            double im = 0.0;
        };
        std::vector<Peak> comb;
        double comb_sigma = 0.15;

        double x_mean = 0.0;
        double p_mean = 0.0;
        double x_var = 0.5;
        double p_var = 0.5;
        double xp_cov = 0.0;
        int photon_number = 0;
        std::complex<double> amp0 = {1.0, 0.0};
        std::complex<double> amp1 = {0.0, 0.0};
        bool allocated = false;
        bool locked = false;
    };

    struct PhotonicNoiseConfig
    {
        double loss_rate = 0.01;
        double detector_efficiency = 0.9;
        double dark_count_rate = 1e-3;
        double squeezing_db = 10.0;
    };

    class PhotonicBackend : public IQuantumBackend
    {
    private:
        std::mutex optics_mutex;
        std::vector<GaussianMode> modes;
        PhotonicNoiseConfig noise;
        std::mt19937_64 rng;
        bool use_gkp = true;

        struct JointPeak
        {
            std::vector<int> idx;
            double re = 0.0;
            double im = 0.0;
        };
        std::vector<JointPeak> gkp_joint;
        size_t gkp_n = 0;
        double gkp_sigma = 0.15;

        static constexpr int GKP_COMB = 4;

        double gauss_noise()
        {
            std::normal_distribution<double> dist(0.0, 1.0);
            return dist(rng);
        }
        bool bernoulli(double p)
        {
            std::uniform_real_distribution<double> dist(0.0, 1.0);
            return dist(rng) < p;
        }
        double uniform_unit()
        {
            std::uniform_real_distribution<double> dist(0.0, 1.0);
            return dist(rng);
        }

        void ensure_mode(size_t m)
        {
            if (m >= modes.size())
                modes.resize(m + 1);
        }

        void apply_loss(size_t m)
        {
            if (modes[m].photon_number == 1 && bernoulli(noise.loss_rate))
                modes[m].photon_number = 0;
        }

        void phase_shift(size_t m, double phi)
        {
            double c = std::cos(phi), s = std::sin(phi);
            double x = modes[m].x_mean, p = modes[m].p_mean;
            modes[m].x_mean = c * x - s * p;
            modes[m].p_mean = s * x + c * p;
            apply_loss(m);
        }

    public:
        PhotonicBackend() { rng.seed(std::random_device{}()); }
        explicit PhotonicBackend(const PhotonicNoiseConfig &cfg) : noise(cfg) { rng.seed(std::random_device{}()); }

        const PhotonicNoiseConfig &get_noise() const { return noise; }

        void set_gkp_encoding(bool enable) { use_gkp = enable; }
        bool get_gkp_encoding() const { return use_gkp; }

        void allocate_qubits(size_t num_qubits) override
        {
            std::lock_guard<std::mutex> lock(optics_mutex);
            if (num_qubits > modes.size())
                modes.resize(num_qubits);
            for (size_t i = 0; i < num_qubits; ++i)
                modes[i].allocated = true;
            if (use_gkp)
            {
                gkp_n = num_qubits;
                gkp_joint.clear();
                JointPeak p;
                p.idx.assign(num_qubits, 0);
                p.re = 1.0;
                p.im = 0.0;
                gkp_joint.push_back(std::move(p));
            }
        }
        void release_qubit(size_t qubit_id) override
        {
            std::lock_guard<std::mutex> lock(optics_mutex);
            if (qubit_id < modes.size())
                modes[qubit_id].allocated = false;
        }
        void lock_hardware_id(size_t qubit_id) override
        {
            if (qubit_id < modes.size())
                modes[qubit_id].locked = true;
        }
        void unlock_hardware_id(size_t qubit_id) override
        {
            if (qubit_id < modes.size())
                modes[qubit_id].locked = false;
        }

        void displace(size_t mode, double alpha_re, double alpha_im)
        {
            std::lock_guard<std::mutex> lock(optics_mutex);
            ensure_mode(mode);
            const double sqrt2 = std::sqrt(2.0);
            modes[mode].x_mean += sqrt2 * alpha_re;
            modes[mode].p_mean += sqrt2 * alpha_im;
        }

        void squeeze(size_t mode, double r)
        {
            std::lock_guard<std::mutex> lock(optics_mutex);
            ensure_mode(mode);
            modes[mode].x_var *= std::exp(-2.0 * r);
            modes[mode].p_var *= std::exp(2.0 * r);
        }

        void phase_shift_gate(size_t mode, double phi)
        {
            std::lock_guard<std::mutex> lock(optics_mutex);
            ensure_mode(mode);
            phase_shift(mode, phi);
        }

        void beam_splitter(size_t a, size_t b, double theta)
        {
            std::lock_guard<std::mutex> lock(optics_mutex);
            ensure_mode(a);
            ensure_mode(b);
            double c = std::cos(theta), s = std::sin(theta);
            double xa = modes[a].x_mean, pa = modes[a].p_mean;
            double xb = modes[b].x_mean, pb = modes[b].p_mean;
            modes[a].x_mean = c * xa + s * xb;
            modes[b].x_mean = -s * xa + c * xb;
            modes[a].p_mean = c * pa + s * pb;
            modes[b].p_mean = -s * pa + c * pb;
            apply_loss(a);
            apply_loss(b);
        }

        double homodyne(size_t mode, double phi)
        {
            std::lock_guard<std::mutex> lock(optics_mutex);
            ensure_mode(mode);
            double c = std::cos(phi), s = std::sin(phi);
            double mean = c * modes[mode].x_mean + s * modes[mode].p_mean;
            double var = c * c * modes[mode].x_var + s * s * modes[mode].p_var;
            return mean + std::sqrt(var) * gauss_noise();
        }

        int photon_count(size_t mode)
        {
            std::lock_guard<std::mutex> lock(optics_mutex);
            ensure_mode(mode);
            double n_mean = (modes[mode].x_mean * modes[mode].x_mean +
                             modes[mode].p_mean * modes[mode].p_mean +
                             modes[mode].x_var + modes[mode].p_var - 1.0) / 2.0;
            if (n_mean < 0.0)
                n_mean = 0.0;
            double detected = modes[mode].photon_number * (bernoulli(noise.detector_efficiency) ? 1.0 : 0.0);
            if (bernoulli(noise.dark_count_rate))
                detected += 1.0;
            return static_cast<int>(std::round(detected + (n_mean > 0.5 ? 0.5 : 0.0)));
        }

        static constexpr double GKP_SQRT_PI = 1.772453850905516;

        void gkp_x(size_t mode)
        {
            std::lock_guard<std::mutex> lock(optics_mutex);
            for (auto &p : gkp_joint)
                if (mode < p.idx.size())
                    p.idx[mode] += 1;
        }

        void gkp_z(size_t mode)
        {
            std::lock_guard<std::mutex> lock(optics_mutex);
            for (auto &p : gkp_joint)
            {
                if (mode < p.idx.size() && (p.idx[mode] & 1))
                {
                    p.re = -p.re;
                    p.im = -p.im;
                }
            }
        }

        void gkp_h(size_t mode)
        {
            std::lock_guard<std::mutex> lock(optics_mutex);
            if (mode >= gkp_n)
                return;
            const double norm = 1.0 / std::sqrt(2.0 * static_cast<double>(GKP_COMB));
            std::map<std::vector<int>, std::pair<double, double>> merged;
            for (const auto &p : gkp_joint)
            {
                int k = p.idx[mode];
                for (int j = -GKP_COMB; j < GKP_COMB; ++j)
                {
                    auto idx = p.idx;
                    idx[mode] = j;
                    double sign = ((k * j) & 1) ? -1.0 : 1.0;
                    auto &acc = merged[idx];
                    acc.first += p.re * sign * norm;
                    acc.second += p.im * sign * norm;
                }
            }
            gkp_joint.clear();
            gkp_joint.reserve(merged.size());
            for (auto &kv : merged)
                gkp_joint.push_back({std::move(kv.first), kv.second.first, kv.second.second});
        }

        void gkp_cnot(size_t control, size_t target)
        {
            std::lock_guard<std::mutex> lock(optics_mutex);
            for (auto &p : gkp_joint)
                if (target < p.idx.size() && control < p.idx.size())
                    p.idx[target] += p.idx[control];
        }

        int gkp_measure(size_t mode)
        {
            std::lock_guard<std::mutex> lock(optics_mutex);
            if (gkp_joint.empty())
                return 0;
            double total = 0.0;
            for (const auto &p : gkp_joint)
                total += p.re * p.re + p.im * p.im;
            if (total <= 0.0)
                return 0;
            double r = uniform_unit() * total;
            double acc = 0.0;
            int sampled_index = 0;
            for (const auto &p : gkp_joint)
            {
                acc += p.re * p.re + p.im * p.im;
                if (r < acc)
                {
                    sampled_index = (mode < p.idx.size()) ? p.idx[mode] : 0;
                    break;
                }
            }
            double x = sampled_index * GKP_SQRT_PI + gkp_sigma * gauss_noise();
            double mod = std::fmod(x, 2.0 * GKP_SQRT_PI);
            if (mod < 0.0)
                mod += 2.0 * GKP_SQRT_PI;
            int result = (mod >= GKP_SQRT_PI / 2.0 && mod < 1.5 * GKP_SQRT_PI) ? 1 : 0;

            int parity = result;
            std::vector<JointPeak> collapsed;
            collapsed.reserve(gkp_joint.size());
            double new_total = 0.0;
            for (const auto &p : gkp_joint)
            {
                if (mode < p.idx.size() && ((p.idx[mode] & 1) == parity))
                {
                    collapsed.push_back(p);
                    new_total += p.re * p.re + p.im * p.im;
                }
            }
            if (new_total > 0.0)
            {
                double norm = 1.0 / std::sqrt(new_total);
                for (auto &p : collapsed)
                {
                    p.re *= norm;
                    p.im *= norm;
                }
                gkp_joint.swap(collapsed);
            }
            return result;
        }

        void apply_noise(size_t qubit_id, int channel, double param) override
        {
            std::lock_guard<std::mutex> lock(optics_mutex);
            ensure_mode(qubit_id);
            auto &m = modes[qubit_id];
            switch (channel)
            {
                case 0:
                {
                    const double r = uniform_unit();
                    const double px = param / 4.0;
                    if (r < px)
                    {
                        std::swap(m.amp0, m.amp1);
                    }
                    else if (r < 2 * px)
                    {
                        const std::complex<double> I(0.0, 1.0);
                        const std::complex<double> t = m.amp0;
                        m.amp0 = -I * m.amp1;
                        m.amp1 = I * t;
                    }
                    else if (r < 3 * px)
                    {
                        m.amp1 *= std::complex<double>(-1.0, 0.0);
                    }
                    break;
                }
                case 1:
                    if (bernoulli(param)) m.amp1 *= std::complex<double>(-1.0, 0.0);
                    break;
                case 2:
                    if (bernoulli(param)) m.photon_number = 0;
                    break;
                case 3:
                    if (bernoulli(param)) m.photon_number ^= 1;
                    break;
                default: break;
            }
        }

        void apply_x(size_t qubit_id) override
        {
            if (use_gkp)
            {
                gkp_x(qubit_id);
                return;
            }
            std::lock_guard<std::mutex> lock(optics_mutex);
            ensure_mode(qubit_id);
            std::swap(modes[qubit_id].amp0, modes[qubit_id].amp1);
            modes[qubit_id].photon_number ^= 1;
            apply_loss(qubit_id);
        }

        void apply_h(size_t qubit_id) override
        {
            if (use_gkp)
            {
                gkp_h(qubit_id);
                return;
            }
            std::lock_guard<std::mutex> lock(optics_mutex);
            ensure_mode(qubit_id);
            const double inv = 1.0 / std::sqrt(2.0);
            auto a0 = modes[qubit_id].amp0;
            auto a1 = modes[qubit_id].amp1;
            modes[qubit_id].amp0 = inv * (a0 + a1);
            modes[qubit_id].amp1 = inv * (a0 - a1);
            phase_shift(qubit_id, M_PI / 2.0);
        }

        void apply_rz(size_t qubit_id, double angle) override
        {
            std::lock_guard<std::mutex> lock(optics_mutex);
            ensure_mode(qubit_id);
            modes[qubit_id].amp1 *= std::complex<double>(std::cos(angle), std::sin(angle));
            phase_shift(qubit_id, angle);
        }

        void apply_cnot(size_t control, size_t target) override
        {
            if (use_gkp)
            {
                gkp_cnot(control, target);
                return;
            }
            std::lock_guard<std::mutex> lock(optics_mutex);
            ensure_mode(control);
            ensure_mode(target);
            if (modes[control].photon_number == 1)
                modes[target].photon_number ^= 1;
            apply_loss(control);
            apply_loss(target);
        }

        void apply_toffoli(size_t c1, size_t c2, size_t target) override
        {
            std::lock_guard<std::mutex> lock(optics_mutex);
            ensure_mode(c1);
            ensure_mode(c2);
            ensure_mode(target);
            if (modes[c1].photon_number == 1 && modes[c2].photon_number == 1)
                modes[target].photon_number ^= 1;
            apply_loss(c1);
            apply_loss(c2);
            apply_loss(target);
        }

        int measure(size_t qubit_id) override
        {
            if (use_gkp)
                return gkp_measure(qubit_id);
            std::lock_guard<std::mutex> lock(optics_mutex);
            ensure_mode(qubit_id);
            double p1 = std::norm(modes[qubit_id].amp1);
            int result = bernoulli(p1) ? 1 : 0;
            if (result == 1 && !bernoulli(noise.detector_efficiency))
                result = 0;
            if (result == 0 && bernoulli(noise.dark_count_rate))
                result = 1;
            modes[qubit_id].amp0 = (result == 0) ? std::complex<double>(1.0, 0.0) : std::complex<double>(0.0, 0.0);
            modes[qubit_id].amp1 = (result == 1) ? std::complex<double>(1.0, 0.0) : std::complex<double>(0.0, 0.0);
            modes[qubit_id].photon_number = result;
            return result;
        }

        struct BellResult
        {
            double z_agreement = 0.0;
            double x_agreement = 0.0;
            double witness = 0.0;
            double chsh = 0.0;
        };

        void prepare_bell()
        {
            std::lock_guard<std::mutex> lock(optics_mutex);
            gkp_n = 2;
            gkp_joint.clear();
            JointPeak p;
            p.idx.assign(2, 0);
            p.re = 1.0;
            p.im = 0.0;
            gkp_joint.push_back(std::move(p));
        }

        BellResult bell_verify(size_t rounds)
        {
            BellResult res;
            if (!use_gkp)
                return res;
            size_t z_same = 0, z_total = 0;
            size_t x_same = 0, x_total = 0;

            for (size_t r = 0; r < rounds; ++r)
            {
                prepare_bell();
                gkp_h(0);
                gkp_cnot(0, 1);
                int a = gkp_measure(0);
                int b = gkp_measure(1);
                ++z_total;
                if (a == b)
                    ++z_same;

                prepare_bell();
                gkp_h(0);
                gkp_cnot(0, 1);
                gkp_h(0);
                gkp_h(1);
                int xa = gkp_measure(0);
                int xb = gkp_measure(1);
                ++x_total;
                if (xa == xb)
                    ++x_same;
            }

            res.z_agreement = z_total ? double(z_same) / double(z_total) : 0.0;
            res.x_agreement = x_total ? double(x_same) / double(x_total) : 0.0;
            res.witness = 2.0 * (res.z_agreement + res.x_agreement) - 2.0;
            res.chsh = 2.0 * std::sqrt(2.0) * std::max(0.0, res.witness) / 2.0;
            return res;
        }

        double expectation_z(size_t qubit_id) override
        {
            std::lock_guard<std::mutex> lock(optics_mutex);
            ensure_mode(qubit_id);
            return modes[qubit_id].photon_number == 1 ? -1.0 : 1.0;
        }

        const GaussianMode &get_mode(size_t m) const { return modes.at(m); }
        size_t get_num_qubits() const override { return modes.size(); }
    };

}