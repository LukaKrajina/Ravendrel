#pragma once

#include <vector>
#include <cmath>
#include <string>
#include <memory>
#include <stdexcept>
#include <iostream>
#include <algorithm>
#include <limits>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#ifndef M_PI_2
#define M_PI_2 1.57079632679489661923
#endif

#include "../qhal/IQuantumBackend.hpp"
#include "../numqk/Numqk.hpp"
#include "../../src/QObject.hpp"

namespace qbns
{

    struct NeuralStream
    {
        numqk::Tensor<double> samples;
        size_t num_channels = 0;
        size_t num_time_steps = 0;
        double sampling_rate_hz = 1000.0;
    };

    struct SpikeTrain
    {
        std::vector<std::vector<bool>> channel_spikes;
        size_t num_channels = 0;
        double window_duration_ms = 1000.0;
    };

    struct LocalFieldPotential
    {
        std::vector<double> values;
        double sampling_rate_hz = 1000.0;
    };

    struct EEGSpectrum
    {
        double delta_power = 0.0;
        double theta_power = 0.0;
        double alpha_power = 0.0;
        double beta_power = 0.0;
        double gamma_power = 0.0;
    };

    enum class QuantumSensor
    {
        NV_Center,
        SQUID,
        ATOMIC_CLOCK
    };

    struct QuantumSensorReading
    {
        std::vector<double> readings;
        double sensitivity_t_per_sqrt_hz = 1e-15;
        std::string sensor_type = "NV-Center";
    };

    class Transducer
    {
    private:
        qhal::IQuantumBackend *backend;

        static double clamp_normalize(double value, double min_val, double max_val,
                                      double target_min, double target_max)
        {
            double range = max_val - min_val;
            if (range < 1e-12)
                return (target_min + target_max) * 0.5;
            double normalized = (value - min_val) / range;
            if (normalized < 0.0) normalized = 0.0;
            else if (normalized > 1.0) normalized = 1.0;
            return target_min + normalized * (target_max - target_min);
        }

        static double vec_min(const std::vector<double> &v)
        {
            if (v.empty()) return 0.0;
            return *std::min_element(v.begin(), v.end());
        }

        static double vec_max(const std::vector<double> &v)
        {
            if (v.empty()) return 0.0;
            return *std::max_element(v.begin(), v.end());
        }

    public:
        explicit Transducer(qhal::IQuantumBackend *target_backend) : backend(target_backend)
        {
            std::cout << "[Transducer] Neural-to-Quantum transducer initialized.\n";
        }

        std::shared_ptr<rvdl::QDataState> amplitude_encode(const NeuralStream &stream)
        {
            size_t n = stream.num_channels;
            if (n == 0)
                throw std::runtime_error(
                    "[Transducer] amplitude_encode: zero channels in NeuralStream.");

            const double *raw = stream.samples.data();
            std::vector<double> channel_means(n, 0.0);

            double global_min = std::numeric_limits<double>::max();
            double global_max = std::numeric_limits<double>::lowest();

            for (size_t ch = 0; ch < n; ++ch)
            {
                double sum = 0.0;
                for (size_t t = 0; t < stream.num_time_steps; ++t)
                {
                    sum += raw[ch * stream.num_time_steps + t];
                }
                channel_means[ch] =
                    (stream.num_time_steps > 0) ? sum / static_cast<double>(stream.num_time_steps) : 0.0;
                global_min = std::min(global_min, channel_means[ch]);
                global_max = std::max(global_max, channel_means[ch]);
            }

            backend->allocate_qubits(n);
            std::vector<size_t> ids(n);

            for (size_t ch = 0; ch < n; ++ch)
            {
                ids[ch] = ch;
                double angle = clamp_normalize(channel_means[ch], global_min, global_max, 0.0, M_PI);
                backend->apply_rz(ch, angle);
            }

            std::cout << "[Transducer] amplitude_encode: encoded " << n
                      << " channels into Rz rotations.\n";
            return std::make_shared<rvdl::QDataState>(backend, ids);
        }

        std::shared_ptr<rvdl::QDataState> spike_to_basis(const SpikeTrain &train)
        {
            size_t n = train.num_channels;
            if (n == 0)
                throw std::runtime_error(
                    "[Transducer] spike_to_basis: zero channels in SpikeTrain.");

            backend->allocate_qubits(n);
            std::vector<size_t> ids(n);

            for (size_t ch = 0; ch < n; ++ch)
            {
                ids[ch] = ch;
                bool has_spike = false;
                if (ch < train.channel_spikes.size())
                {
                    has_spike = std::any_of(train.channel_spikes[ch].begin(),
                                            train.channel_spikes[ch].end(),
                                            [](bool s) { return s; });
                }
                if (has_spike)
                {
                    backend->apply_x(ch);
                }
            }

            std::cout << "[Transducer] spike_to_basis: encoded " << n
                      << " channels into computational basis.\n";
            return std::make_shared<rvdl::QDataState>(backend, ids);
        }

        std::shared_ptr<rvdl::QDataState> lfp_to_phase(const LocalFieldPotential &lfp,
                                                        size_t max_qubits = 16)
        {
            if (lfp.values.empty())
                throw std::runtime_error("[Transducer] lfp_to_phase: empty LFP values.");

            size_t n = std::min(lfp.values.size(), max_qubits);
            size_t stride = (lfp.values.size() > max_qubits)
                                ? lfp.values.size() / max_qubits
                                : 1;

            double vmin = vec_min(lfp.values);
            double vmax = vec_max(lfp.values);

            backend->allocate_qubits(n);
            std::vector<size_t> ids(n);

            for (size_t i = 0; i < n; ++i)
            {
                ids[i] = i;
                double sample = lfp.values[i * stride];
                double phase = clamp_normalize(sample, vmin, vmax, 0.0, 2.0 * M_PI);

                backend->apply_h(i);
                backend->apply_rz(i, phase);
            }

            std::cout << "[Transducer] lfp_to_phase: encoded " << n
                      << " LFP samples into phase states.\n";
            return std::make_shared<rvdl::QDataState>(backend, ids);
        }

        std::shared_ptr<rvdl::QDataState> eeg_to_entangled(const EEGSpectrum &spectrum)
        {
            const size_t n = 5;
            double powers[5] = {
                spectrum.delta_power, spectrum.theta_power, spectrum.alpha_power,
                spectrum.beta_power, spectrum.gamma_power};

            double pmin = *std::min_element(powers, powers + n);
            double pmax = *std::max_element(powers, powers + n);

            backend->allocate_qubits(n);
            std::vector<size_t> ids(n);

            for (size_t i = 0; i < n; ++i)
            {
                ids[i] = i;
                double angle = clamp_normalize(powers[i], pmin, pmax, 0.0, M_PI);
                backend->apply_rz(i, angle);
            }

            backend->apply_h(0);
            for (size_t i = 0; i < n - 1; ++i)
            {
                backend->apply_cnot(i, i + 1);
            }

            std::cout << "[Transducer] eeg_to_entangled: encoded 5 EEG bands into entangled state.\n";
            return std::make_shared<rvdl::QDataState>(backend, ids);
        }

        std::shared_ptr<rvdl::QDataState> sensor_to_state(const QuantumSensorReading &reading,
                                                           size_t max_qubits = 16)
        {
            if (reading.readings.empty())
                throw std::runtime_error("[Transducer] sensor_to_state: empty sensor readings.");

            size_t n = std::min(reading.readings.size(), max_qubits);

            double vmin = vec_min(reading.readings);
            double vmax = vec_max(reading.readings);

            backend->allocate_qubits(n);
            std::vector<size_t> ids(n);

            for (size_t i = 0; i < n; ++i)
            {
                ids[i] = i;
                double angle = clamp_normalize(reading.readings[i], vmin, vmax, 0.0, M_PI);

                backend->apply_h(i);
                backend->apply_rz(i, angle);
            }

            std::cout << "[Transducer] sensor_to_state: encoded " << n << " "
                      << reading.sensor_type << " readings into superposition states.\n";
            return std::make_shared<rvdl::QDataState>(backend, ids);
        }

        qhal::IQuantumBackend *get_backend() const { return backend; }
    };
}