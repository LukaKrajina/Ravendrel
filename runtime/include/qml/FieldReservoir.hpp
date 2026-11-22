#pragma once
#include "../spacetime/NeuralField.hpp"
#include "Reservoir.hpp"
#include <memory>
#include <vector>
#include <algorithm>
#include <cstdint>
#include <iostream>

namespace qml
{

    class FieldReservoir
    {
    private:
        std::unique_ptr<rvdl::spacetime::NeuralField> field_;
        std::unique_ptr<QuantumReservoir> reservoir_;
        size_t num_probes_;

    public:
        FieldReservoir(qhal::IQuantumBackend *be,
                       const rvdl::spacetime::NeuralFieldParams &params,
                       size_t num_probes,
                       size_t reservoir_layers,
                       size_t reservoir_out_dim = 0,
                       uint64_t seed = 42)
            : field_(std::make_unique<rvdl::spacetime::NeuralField>(params)),
              num_probes_(num_probes > 0 ? num_probes : 8)
        {
            reservoir_ = std::make_unique<QuantumReservoir>(
                be, num_probes_, reservoir_layers, reservoir_out_dim, seed);
            std::cout << "[FieldReservoir] Neural-field reservoir online (probes="
                      << num_probes_ << ", layers=" << reservoir_layers << ").\n";
        }

        std::vector<double> sample_field() const
        {
            const std::vector<double> &u = field_->field();
            const size_t n = u.size();
            std::vector<double> samples;
            samples.reserve(num_probes_);

            if (n == 0)
            {
                samples.assign(num_probes_, 0.0);
                return samples;
            }

            double vmin = *std::min_element(u.begin(), u.end());
            double vmax = *std::max_element(u.begin(), u.end());
            double range = vmax - vmin;
            if (range < 1e-12)
                range = 1.0;

            size_t stride = n / num_probes_;
            if (stride == 0)
                stride = 1;
            for (size_t p = 0; p < num_probes_; ++p)
            {
                size_t idx = (p * stride) % n;
                double v = (u[idx] - vmin) / range;
                samples.push_back(v);
            }
            return samples;
        }

        std::vector<double> step_and_probe()
        {
            field_->step();
            return reservoir_->evolve(sample_field());
        }

        std::vector<std::vector<double>> collect_sequence(size_t steps)
        {
            std::vector<std::vector<double>> seq;
            seq.reserve(steps);
            for (size_t t = 0; t < steps; ++t)
                seq.push_back(step_and_probe());
            return seq;
        }

        void train(const std::vector<std::vector<double>> &inputs,
                   const std::vector<std::vector<double>> &targets,
                   int epochs,
                   double lr)
        {
            reservoir_->train(inputs, targets, epochs, lr);
        }

        std::vector<double> predict()
        {
            return reservoir_->predict(sample_field());
        }

        rvdl::spacetime::NeuralFieldOrderParameter order_parameter() const
        {
            return field_->order_parameter();
        }

        QuantumReservoir *reservoir() { return reservoir_.get(); }
        rvdl::spacetime::NeuralField *field() { return field_.get(); }
        size_t num_probes() const { return num_probes_; }
    };

}