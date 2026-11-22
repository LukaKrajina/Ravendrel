#pragma once
#include "../qhal/IQuantumBackend.hpp"
#include "../numqk/Numqk.hpp"
#include <vector>
#include <random>
#include <memory>
#include <atomic>
#include <cstdint>
#include <iostream>
#include <stdexcept>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#include "Inference.hpp"

namespace qml
{

    class QuantumReservoir
    {
    private:
        qhal::IQuantumBackend *backend;
        size_t num_qubits;
        size_t num_layers;
        size_t out_dim;
        uint64_t seed;

        std::vector<std::vector<double>> layer_angles;
        std::vector<std::pair<size_t, size_t>> cnot_topology;
        std::vector<size_t> qubit_ids_;

        std::unique_ptr<numqk::Tensor<double>> W;
        std::unique_ptr<numqk::Tensor<double>> b;

    public:
        QuantumReservoir(qhal::IQuantumBackend *be,
                         size_t qubits,
                         size_t layers,
                         size_t out_dim = 0,
                         uint64_t seed_val = 42)
            : backend(be),
              num_qubits(qubits),
              num_layers(layers),
              out_dim(out_dim == 0 ? qubits : out_dim),
              seed(seed_val)
        {
            if (!backend)
                throw std::runtime_error("[QRC] null backend.");

            std::mt19937_64 rng(seed);
            std::uniform_real_distribution<double> ang(0.0, 2.0 * M_PI);

            layer_angles.resize(num_layers);
            for (size_t l = 0; l < num_layers; ++l)
            {
                layer_angles[l].resize(num_qubits);
                for (size_t q = 0; q < num_qubits; ++q)
                    layer_angles[l][q] = ang(rng);
            }

            cnot_topology.reserve(num_qubits);
            for (size_t q = 0; q < num_qubits; ++q)
                cnot_topology.emplace_back(q, (q + 1) % num_qubits);

            W = std::make_unique<numqk::Tensor<double>>(
                std::vector<size_t>{this->out_dim, num_qubits}, true);
            b = std::make_unique<numqk::Tensor<double>>(
                std::vector<size_t>{this->out_dim, 1}, true);
            std::normal_distribution<double> wdist(0.0, 0.1);
            for (size_t i = 0; i < W->size(); ++i)
                W->data()[i] = wdist(rng);
            for (size_t i = 0; i < b->size(); ++i)
                b->data()[i] = wdist(rng);

            qubit_ids_.reserve(num_qubits);
            for (size_t q = 0; q < num_qubits; ++q)
            {
                size_t id = rvdl_alloc_qubit_id();
                qubit_ids_.push_back(id);
                backend->allocate_qubit(id);
            }

            std::cout << "[QRC] Quantum reservoir online (" << num_qubits
                      << " qubits, " << num_layers << " layers, out_dim="
                      << this->out_dim << ").\n";
        }

        ~QuantumReservoir()
        {
            for (size_t q = 0; q < num_qubits; ++q)
            {
                size_t id = qubit_ids_[q];
                backend->release_qubit(id);
                rvdl_free_qubit_id(id);
            }
        }

        std::vector<double> evolve(const std::vector<double> &input)
        {
            for (size_t q = 0; q < num_qubits; ++q)
            {
                size_t id = qubit_ids_[q];
                if (backend->measure(id) == 1)
                    backend->apply_x(id);
            }

            for (size_t q = 0; q < num_qubits; ++q)
            {
                size_t id = qubit_ids_[q];
                backend->apply_h(id);
                double x = (q < input.size()) ? input[q] : 0.0;
                if (x < 0.0) x = 0.0;
                if (x > 1.0) x = 1.0;
                backend->apply_rz(id, x * 2.0 * M_PI);
            }

            for (size_t l = 0; l < num_layers; ++l)
            {
                for (size_t q = 0; q < num_qubits; ++q)
                    backend->apply_rz(qubit_ids_[q], layer_angles[l][q]);
                for (const auto &cn : cnot_topology)
                    backend->apply_cnot(qubit_ids_[cn.first], qubit_ids_[cn.second]);
            }

            std::vector<double> feats(num_qubits);
            for (size_t q = 0; q < num_qubits; ++q)
                feats[q] = backend->expectation_z(qubit_ids_[q]);

            return feats;
        }

        numqk::Tensor<double> readout_forward(const std::vector<double> &feats)
        {
            numqk::Tensor<double> f({num_qubits, 1}, false);
            for (size_t i = 0; i < num_qubits; ++i)
                f.data()[i] = feats[i];

            numqk::Tensor<double> out = W->matmul(f);
            out = out.add(*b);
            return out;
        }

        void train(const std::vector<std::vector<double>> &inputs,
                   const std::vector<std::vector<double>> &targets,
                   int epochs,
                   double lr)
        {
            if (inputs.empty() || inputs.size() != targets.size())
                throw std::runtime_error("[QRC] train: input/target count mismatch.");

            std::vector<std::vector<double>> X;
            X.reserve(inputs.size());
            for (const auto &in : inputs)
                X.push_back(evolve(in));

            for (int e = 0; e < epochs; ++e)
            {
                double epoch_loss = 0.0;
                for (size_t s = 0; s < X.size(); ++s)
                {
                    numqk::Tensor<double> t({out_dim, 1}, false);
                    for (size_t j = 0; j < out_dim; ++j)
                        t.data()[j] = (j < targets[s].size()) ? targets[s][j] : 0.0;

                    auto out = readout_forward(X[s]);
                    auto diff = out.sub(t);
                    auto loss = diff.mul(diff).mean();

                    loss.backward();

                    for (size_t i = 0; i < W->size(); ++i)
                    {
                        W->data()[i] -= lr * W->get_grad()->data()[i];
                        W->get_grad()->data()[i] = 0.0;
                    }
                    for (size_t i = 0; i < b->size(); ++i)
                    {
                        b->data()[i] -= lr * b->get_grad()->data()[i];
                        b->get_grad()->data()[i] = 0.0;
                    }

                    epoch_loss += loss.data()[0];
                }
                if ((e + 1) % 10 == 0 || e == 0)
                    std::cout << "      [QRC] Epoch " << e + 1 << "/" << epochs
                              << " | MSE: " << (epoch_loss / X.size()) << "\n";
            }
        }

        std::vector<double> predict(const std::vector<double> &input)
        {
            auto feats = evolve(input);
            auto out = readout_forward(feats);
            std::vector<double> y(out_dim);
            for (size_t j = 0; j < out_dim; ++j)
                y[j] = out.data()[j];
            return y;
        }

        size_t qubits() const { return num_qubits; }
        size_t layers() const { return num_layers; }
        size_t output_dim() const { return out_dim; }
        const std::vector<std::vector<double>> &fixed_angles() const { return layer_angles; }
    };
}