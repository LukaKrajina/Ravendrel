#pragma once
#include "StatePolicy.hpp"
#include "../numqk/Numqk.hpp"
#include "../qhal/IQuantumBackend.hpp"
#include "../qhal/PhotonicBackend.hpp"
#include "../qml/Layer.hpp"
#include "../qml/RVDLModelFormat.hpp"
#include "../qml/QGT.hpp"
#include "../../src/QObject.hpp"
#include "../../src/QDataEncoder.hpp"
#include <memory>
#include <functional>
#include <vector>
#include <cmath>
#include <random>
#include <string>
#include <unordered_map>
#include <iostream>
#include <algorithm>
#include <chrono>
#include <complex>
#include <cstdint>
#include <ctime>
#include <iomanip>
#include <limits>
#include <numeric>
#include <sstream>
#include <stdexcept>

namespace qlm
{
    using DeviceAdjointPOD = double (*)(
        const unsigned char *gate_type, const size_t *q0, const size_t *q1,
        const size_t *param, const double *scale, size_t gate_count,
        const double *ket, size_t dim, const double *theta,
        const size_t *readout_bits, size_t readout_count, uint8_t target_token,
        double *grad, size_t grad_count, double *pred, double *accuracy,
        size_t moe_experts, size_t active_expert, const size_t *gate_expert);

    using DeviceAdjointBatchPOD = void (*)(
        const unsigned char *gate_type, const size_t *q0, const size_t *q1,
        const size_t *param, const double *scale, size_t gate_count,
        const double *ket_batch, size_t B, size_t N,
        const double *theta, const size_t *readout_bits, size_t D,
        const uint8_t *targets,
        double *grad, size_t P, double *loss_out, double *acc_out,
        size_t tile_bits, double tile_epsilon,
        double *skipped_mass, double *error_bound);






    using DeviceAdjointSparseBatchPOD = void (*)(
        const unsigned char *gate_type, const size_t *q0, const size_t *q1,
        const size_t *param, const double *scale, size_t gate_count,
        const double *ket_batch, size_t B, size_t N,
        const double *theta, const size_t *readout_bits, size_t D,
        const uint8_t *targets,
        double *grad, size_t P, double *loss_out, double *acc_out,
        const AdjointRuntimeConfig *cfg);

    struct TrainProgress
    {
        int epoch = 0;
        int epochs = 0;
        size_t sample_in_epoch = 0;
        size_t total_samples = 0;
        size_t samples_seen = 0;
        double loss = 0.0;
        double epoch_loss = 0.0;
        double best_loss = 0.0;
        double accuracy = 0.0;
        double epoch_accuracy = 0.0;
        double samples_per_sec = 0.0;
        double elapsed_sec = 0.0;
        double eta_sec = 0.0;
        size_t params = 0;
        bool epoch_end = false;
        bool stopped = false;
    };

    enum class Optimizer
    {
        Adam,
        SGD
    };

    struct TrainOptions
    {
        int epochs = 1;
        double lr = 0.02;

        Optimizer optimizer = Optimizer::Adam;
        double beta1 = 0.9;
        double beta2 = 0.999;
        double adam_eps = 1e-8;
        double weight_decay = 0.0;

        int early_stop_patience = 0;
        double early_stop_min_delta = 1e-5;

        const std::vector<double> *init_weights = nullptr;

        std::function<void(std::shared_ptr<rvdl::QObject> &)> prepare_sample;

        std::function<void(int epoch, size_t total_samples)> on_epoch_start;

        std::function<void(const TrainProgress &)> progress;

        std::function<bool()> should_stop;

        std::function<void(const numqk::Tensor<double> &, const TrainProgress &)> checkpoint;
        size_t checkpoint_every = 0;

        std::unordered_map<std::string, std::string> extra_meta;

        DeviceAdjointPOD device_adjoint_pod = nullptr;

        size_t moe_experts = 1;

        size_t batch_size = 1;
        size_t tile_bits = 0;
        double tile_epsilon = 0.0;
        DeviceAdjointBatchPOD device_adjoint_batch_pod = nullptr;

        StateBackend state_backend = StateBackend::Auto;
        StateResources state_resources;
        double sparse_epsilon = 0.0;
        size_t sparse_tile_bits = 6;
        DeviceAdjointSparseBatchPOD device_adjoint_sparse_batch_pod = nullptr;

        size_t moe_active_offset = 0;

        size_t ras_rank = 0;
        double ras_epsilon = 0.0;
        uint64_t ras_seed = 42;

        bool use_adjoint = true;
        bool shuffle = false;
        uint64_t seed = 42;

        double dp_epsilon = 0.0;

        size_t progress_every = 0;

        double grad_clip = 10.0;
    };

    struct TrainStepResult
    {
        double loss = 0.0;
        double accuracy = 0.0;
    };

    namespace train_detail
    {
        using Complex = std::complex<double>;

        struct GateOp
        {
            enum class Type
            {
                RY,
                RZ,
                CNOT
            };
            Type type = Type::RY;
            size_t q0 = 0;
            size_t q1 = 0;
            size_t param = 0;
            double scale = 1.0;
            size_t expert = 0;
        };

        static constexpr size_t SHARED_GATE = static_cast<size_t>(-1);

        inline bool gate_active(const GateOp &g, size_t experts, size_t active)
        {
            return experts <= 1 || g.expert == SHARED_GATE || g.expert == active;
        }

        inline void sv_apply_rz(std::vector<Complex> &s, size_t q, double angle)
        {
            const Complex phase(std::cos(angle), std::sin(angle));
            const size_t mask = size_t(1) << q;
            for (size_t i = 0; i < s.size(); ++i)
                if (i & mask)
                    s[i] *= phase;
        }

        inline void sv_apply_ry(std::vector<Complex> &s, size_t q, double angle)
        {
            const double c = std::cos(angle * 0.5);
            const double sn = std::sin(angle * 0.5);
            const size_t mask = size_t(1) << q;
            for (size_t i = 0; i < s.size(); ++i)
            {
                if (i & mask)
                    continue;
                const Complex a = s[i];
                const Complex b = s[i | mask];
                s[i] = c * a - sn * b;
                s[i | mask] = sn * a + c * b;
            }
        }

        inline void sv_apply_cnot(std::vector<Complex> &s, size_t control, size_t target)
        {
            const size_t cmask = size_t(1) << control;
            const size_t tmask = size_t(1) << target;
            for (size_t i = 0; i < s.size(); ++i)
            {
                if ((i & tmask) || !(i & cmask))
                    continue;
                std::swap(s[i], s[i | tmask]);
            }
        }

        inline void sv_apply_forward(std::vector<Complex> &s, const GateOp &g, const double *theta)
        {
            if (g.type == GateOp::Type::RZ)
                sv_apply_rz(s, g.q0, theta[g.param] * g.scale);
            else if (g.type == GateOp::Type::RY)
                sv_apply_ry(s, g.q0, theta[g.param] * g.scale);
            else
                sv_apply_cnot(s, g.q0, g.q1);
        }

        inline void sv_apply_inverse(std::vector<Complex> &s, const GateOp &g, const double *theta)
        {
            if (g.type == GateOp::Type::RZ)
                sv_apply_rz(s, g.q0, -theta[g.param] * g.scale);
            else if (g.type == GateOp::Type::RY)
                sv_apply_ry(s, g.q0, -theta[g.param] * g.scale);
            else
                sv_apply_cnot(s, g.q0, g.q1);
        }

        inline std::vector<GateOp> build_gates(size_t n_q, size_t num_layers,
                                               const std::vector<double> &lapse_functions,
                                               size_t moe_experts = 1)
        {
            if (n_q < 8)
                throw std::invalid_argument(
                    "[QLM] build_gates: 至少需要 8 个 qubit（8 位读出）");

            const size_t readout_base = n_q - 8;
            std::vector<GateOp> gates;
            gates.reserve(num_layers * (n_q + (n_q - 8) + 8));
            size_t param = 0;
            for (size_t t = 0; t < num_layers; ++t)
            {
                const double scale = (t < lapse_functions.size()) ? lapse_functions[t] : 1.0;
                for (size_t i = 0; i < n_q; ++i)
                {
                    GateOp g;
                    g.type = GateOp::Type::RY;
                    g.q0 = i;
                    g.param = param++;
                    g.scale = scale;
                    g.expert = (moe_experts > 1) ? (g.param % moe_experts) : 0;
                    gates.push_back(g);
                }
                for (size_t c = 0; c + 8 < n_q; ++c)
                {
                    GateOp cx;
                    cx.type = GateOp::Type::CNOT;
                    cx.q0 = c;
                    cx.q1 = readout_base + (c % 8);
                    cx.expert = SHARED_GATE;
                    gates.push_back(cx);
                }
                for (size_t j = 0; j < 8; ++j)
                {
                    GateOp r;
                    r.type = GateOp::Type::RZ;
                    r.q0 = readout_base + j;
                    r.param = param++;
                    r.scale = scale;
                    r.expert = (moe_experts > 1) ? (r.param % moe_experts) : 0;
                    gates.push_back(r);
                }
            }
            return gates;
        }

        inline double adjoint_loss_grad(const std::vector<GateOp> &gates,
                                        const std::vector<Complex> &ket,
                                        const double *theta,
                                        const std::vector<size_t> &readout_bits,
                                        uint8_t target_token,
                                        std::vector<double> &grad,
                                        std::vector<double> &pred,
                                        std::vector<Complex> &phi,
                                        std::vector<Complex> &lambda,
                                        double &accuracy,
                                        size_t moe_experts = 1,
                                        size_t active_expert = 0)
        {
            const size_t D = readout_bits.size();
            if (D == 0)
            {
                pred.clear();
                accuracy = 0.0;
                return 0.0;
            }

            phi = ket;
            for (const auto &g : gates)
                if (gate_active(g, moe_experts, active_expert))
                    sv_apply_forward(phi, g, theta);

            pred.assign(D, 0.0);
            for (size_t j = 0; j < D; ++j)
            {
                const size_t mask = size_t(1) << readout_bits[j];
                double p1 = 0.0;
                for (size_t s = 0; s < phi.size(); ++s)
                    if (s & mask)
                        p1 += std::norm(phi[s]);
                pred[j] = 1.0 - 2.0 * p1;
            }

            std::vector<double> gz(D, 0.0);
            double loss = 0.0;
            accuracy = 0.0;
            for (size_t j = 0; j < D; ++j)
            {
                const int bit = (target_token >> j) & 1;
                const double target = 1.0 - 2.0 * bit;
                const double d = pred[j] - target;
                loss += d * d;
                gz[j] = 2.0 * d / static_cast<double>(D);

                const int predicted_bit = (pred[j] < 0.0) ? 1 : 0;
                if (predicted_bit == bit)
                    accuracy += 1.0;
            }
            loss /= static_cast<double>(D);
            accuracy /= static_cast<double>(D);

            lambda.resize(phi.size());
            for (size_t s = 0; s < phi.size(); ++s)
            {
                double c = 0.0;
                for (size_t j = 0; j < D; ++j)
                {
                    const double sign = ((s >> readout_bits[j]) & 1) ? -1.0 : 1.0;
                    c += gz[j] * sign;
                }
                lambda[s] = c * phi[s];
            }

            for (size_t k = gates.size(); k > 0; --k)
            {
                const GateOp &g = gates[k - 1];
                if (!gate_active(g, moe_experts, active_expert)) continue;
                const size_t mask = size_t(1) << g.q0;
                if (g.type == GateOp::Type::RZ)
                {
                    double dot_im = 0.0;
                    for (size_t s = 0; s < phi.size(); ++s)
                        if (s & mask)
                            dot_im += (std::conj(lambda[s]) * phi[s]).imag();
                    grad[g.param] += -2.0 * g.scale * dot_im;
                }
                else if (g.type == GateOp::Type::RY)
                {
                    double dot_re = 0.0;
                    for (size_t s = 0; s < phi.size(); ++s)
                    {
                        if (s & mask)
                            continue;
                        dot_re += (std::conj(lambda[s | mask]) * phi[s] -
                                   std::conj(lambda[s]) * phi[s | mask])
                                      .real();
                    }
                    grad[g.param] += g.scale * dot_re;
                }
                sv_apply_inverse(phi, g, theta);
                sv_apply_inverse(lambda, g, theta);
            }

            return loss;
        }

        inline void apply_optimizer_step(numqk::Tensor<double> &theta,
                                         const std::vector<double> &grad,
                                         std::vector<double> &adam_m,
                                         std::vector<double> &adam_v,
                                         size_t &step,
                                         const TrainOptions &opt)
        {
            if (grad.size() != theta.size())
                throw std::invalid_argument("[QLM] optimizer: gradient size mismatch");

            const double clip = opt.grad_clip;
            if (opt.optimizer == Optimizer::Adam)
            {
                ++step;
                const double bc1 = 1.0 - std::pow(opt.beta1, static_cast<double>(step));
                const double bc2 = 1.0 - std::pow(opt.beta2, static_cast<double>(step));
                for (size_t i = 0; i < theta.size(); ++i)
                {
                    double g = grad[i];
                    if (!std::isfinite(g))
                        throw std::runtime_error(
                            "[QLM] optimizer: gradient[" + std::to_string(i) +
                            "] is not finite (numerical blow-up)");
                    if (opt.weight_decay != 0.0)
                        g += opt.weight_decay * theta.data()[i];
                    if (clip > 0.0)
                        g = std::max(-clip, std::min(clip, g));

                    adam_m[i] = opt.beta1 * adam_m[i] + (1.0 - opt.beta1) * g;
                    adam_v[i] = opt.beta2 * adam_v[i] + (1.0 - opt.beta2) * g * g;
                    const double m_hat = adam_m[i] / bc1;
                    const double v_hat = adam_v[i] / bc2;
                    theta.data()[i] -= opt.lr * m_hat / (std::sqrt(v_hat) + opt.adam_eps);
                }
            }
            else
            {
                for (size_t i = 0; i < theta.size(); ++i)
                {
                    double g = grad[i];
                    if (!std::isfinite(g))
                        throw std::runtime_error(
                            "[QLM] optimizer: gradient[" + std::to_string(i) +
                            "] is not finite (numerical blow-up)");
                    if (opt.weight_decay != 0.0)
                        g += opt.weight_decay * theta.data()[i];
                    if (clip > 0.0)
                        g = std::max(-clip, std::min(clip, g));
                    theta.data()[i] -= opt.lr * g;
                }
            }
        }
    }

    class RVDL_RT_API QLM
    {
    private:
        qhal::IQuantumBackend *backend;
        size_t num_layers;
        size_t num_qubits_;
        std::vector<double> lapse_functions;

    public:
        QLM(qhal::IQuantumBackend *be, size_t qubits, size_t layers)
            : backend(be), num_layers(layers), num_qubits_(qubits)
        {
            for (size_t i = 0; i < layers; ++i)
            {
                lapse_functions.push_back(std::exp(-0.1 * static_cast<double>(i)));
            }
        }

        double circuit_ansatz(std::shared_ptr<rvdl::QObject> input_data, numqk::Tensor<double> &params)
        {
            const auto& active_qubits = input_data->get_ids();
            size_t n_q = active_qubits.size();
            if (n_q == 0)
            {
                return 0.0;
            }
            if (n_q < 8)
                throw std::runtime_error(
                    "[QLM] circuit_ansatz: 至少需要 8 个 qubit（8 位读出），当前 " +
                    std::to_string(n_q));

            uint8_t target_token = *static_cast<uint8_t*>(input_data->qlm_data);
            size_t param_idx = 0;
            const size_t readout_base = n_q - 8;

            for (size_t t = 0; t < num_layers; ++t)
            {
                double N_t = lapse_functions[t];

                for (size_t i = 0; i < n_q; ++i) {
                    if (param_idx < params.size()) {
                        backend->apply_ry(active_qubits[i], params.data()[param_idx++] * N_t);
                    }
                }

                for (size_t c = 0; c + 8 < n_q; ++c)
                    backend->apply_cnot(active_qubits[c],
                                        active_qubits[readout_base + (c % 8)]);
                for (size_t j = 0; j < 8; ++j)
                {
                    if (param_idx < params.size())
                        backend->apply_rz(active_qubits[readout_base + j],
                                          params.data()[param_idx++] * N_t);
                }
            }

            double loss = 0.0;
            for (size_t i = 0; i < 8; ++i)
            {
                int measured_bit = backend->measure(active_qubits[readout_base + i]);
                int target_bit = (target_token >> i) & 1;
                
                if (measured_bit != target_bit) {
                    loss += 1.0;
                }
            }
            
            return loss / 8.0;
        }

        void circuit_expectation(std::shared_ptr<rvdl::QObject> input_data,
                                 numqk::Tensor<double> &params,
                                 double *out)
        {
            const auto &active_qubits = input_data->get_ids();
            size_t n_q = active_qubits.size();
            if (n_q == 0)
            {
                for (size_t j = 0; j < 8; ++j) out[j] = 0.0;
                return;
            }
            if (n_q < 8)
                throw std::runtime_error(
                    "[QLM] circuit_expectation: 至少需要 8 个 qubit（8 位读出），当前 " +
                    std::to_string(n_q));

            size_t param_idx = 0;
            const size_t readout_base = n_q - 8;

            for (size_t t = 0; t < num_layers; ++t)
            {
                double N_t = lapse_functions[t];
                for (size_t i = 0; i < n_q; ++i)
                {
                    if (param_idx < params.size())
                        backend->apply_ry(active_qubits[i], params.data()[param_idx++] * N_t);
                }
                for (size_t c = 0; c + 8 < n_q; ++c)
                    backend->apply_cnot(active_qubits[c],
                                        active_qubits[readout_base + (c % 8)]);
                for (size_t j = 0; j < 8; ++j)
                {
                    if (param_idx < params.size())
                        backend->apply_rz(active_qubits[readout_base + j],
                                          params.data()[param_idx++] * N_t);
                }
            }

            for (size_t j = 0; j < 8; ++j)
                out[j] = backend->expectation_z(active_qubits[readout_base + j]);
        }

        numqk::CTensor circuit_state_ket(std::shared_ptr<rvdl::QObject> input_data,
                                         const numqk::Tensor<double> &params) const
        {
            const auto &active_qubits = input_data->get_ids();
            const size_t n_q = active_qubits.size();
            input_data->reset_to_ground_state();
            if (n_q == 0)
                return numqk::CTensor({0}, false);
            if (n_q < 8)
                throw std::runtime_error(
                    "[QLM] circuit_state_ket: 至少需要 8 个 qubit（8 位读出），当前 " +
                    std::to_string(n_q));

            size_t param_idx = 0;
            const size_t readout_base = n_q - 8;

            for (size_t t = 0; t < num_layers; ++t)
            {
                double N_t = lapse_functions[t];
                for (size_t i = 0; i < n_q; ++i)
                    if (param_idx < params.size())
                        backend->apply_ry(active_qubits[i], params.data()[param_idx++] * N_t);
                for (size_t c = 0; c + 8 < n_q; ++c)
                    backend->apply_cnot(active_qubits[c],
                                        active_qubits[readout_base + (c % 8)]);
                for (size_t j = 0; j < 8; ++j)
                    if (param_idx < params.size())
                        backend->apply_rz(active_qubits[readout_base + j],
                                          params.data()[param_idx++] * N_t);
            }

            std::vector<std::complex<double>> sv = backend->get_state_vector();
            numqk::CTensor ket({sv.size()}, false);
            for (size_t i = 0; i < sv.size(); ++i)
                ket.data()[i] = sv[i];
            return ket;
        }

        numqk::Tensor<double> compute_full_qgt(std::shared_ptr<rvdl::QObject> input_data,
                                               numqk::Tensor<double> &params)
        {
            return qml::quantum_geometric_tensor<double>(
                [this, input_data](const numqk::Tensor<double> &p) {
                    return this->circuit_state_ket(input_data, p);
                },
                params);
        }

        void apply_full_qng_update(numqk::Tensor<double> &params, double learning_rate,
                                   std::shared_ptr<rvdl::QObject> input_data)
        {
            auto grad = params.get_grad();

            numqk::Tensor<double> euclidean({params.size()}, false);
            for (size_t i = 0; i < params.size(); ++i)
                euclidean.data()[i] = grad->data()[i];

            auto G = compute_full_qgt(input_data, params);
            auto dtheta = qml::natural_gradient_step(G, euclidean);

            for (size_t i = 0; i < params.size(); ++i)
            {
                params.data()[i] -= learning_rate * dtheta.data()[i];
                grad->data()[i] = 0.0;
            }
        }

        std::vector<double> estimate_fs_diagonal(numqk::Tensor<double> &params,
                                                 std::shared_ptr<rvdl::QObject> input_data)
        {
            const auto &active_qubits = input_data->get_ids();
            size_t n_q = active_qubits.size();
            size_t total = params.size();
            std::vector<double> diag(total, 0.25);
            if (n_q == 0)
                return diag;
            if (n_q < 8)
                throw std::runtime_error(
                    "[QLM] estimate_fs_diagonal: 至少需要 8 个 qubit（8 位读出），当前 " +
                    std::to_string(n_q));

            const size_t readout_base = n_q - 8;

            input_data->reset_to_ground_state();
            size_t param_idx = 0;

            for (size_t t = 0; t < num_layers; ++t)
            {
                double N_t = lapse_functions[t];
                for (size_t i = 0; i < n_q; ++i)
                    if (param_idx < params.size())
                        backend->apply_ry(active_qubits[i], params.data()[param_idx++] * N_t);
                for (size_t c = 0; c + 8 < n_q; ++c)
                    backend->apply_cnot(active_qubits[c],
                                        active_qubits[readout_base + (c % 8)]);
                for (size_t j = 0; j < 8; ++j)
                    if (param_idx < params.size())
                        backend->apply_rz(active_qubits[readout_base + j],
                                          params.data()[param_idx++] * N_t);
            }

            std::vector<double> z(n_q, 0.0);
            for (size_t q = 0; q < n_q; ++q)
                z[q] = backend->expectation_z(active_qubits[q]);

            size_t per_layer = n_q + 8;
            for (size_t i = 0; i < total; ++i)
            {
                size_t within = i % per_layer;
                size_t q = (within < n_q) ? within : (readout_base + (within - n_q));
                if (q < n_q)
                {
                    double g = 0.25 * (1.0 - z[q] * z[q]);
                    diag[i] = (g < 1e-12) ? 1e-12 : g;
                }
            }
            return diag;
        }

        void apply_qng_update(numqk::Tensor<double> &params, double learning_rate,
                              std::shared_ptr<rvdl::QObject> input_data)
        {
            auto grad = params.get_grad();

            auto diag = estimate_fs_diagonal(params, input_data);

            for (size_t i = 0; i < params.size(); ++i)
            {
                double natural_grad = grad->data()[i] / diag[i];
                params.data()[i] -= learning_rate * natural_grad;
                grad->data()[i] = 0.0;
            }
        }

        std::vector<double> sample_photonic(size_t num_samples)
        {
            auto *ph = dynamic_cast<qhal::PhotonicBackend *>(backend);
            if (!ph)
            {
                std::cerr << "[QLM] backend is not photonic; photonic sampling unavailable.\n";
                return {};
            }
            size_t modes = ph->get_num_qubits();
            if (modes == 0)
                return {};
            std::vector<double> samples;
            samples.reserve(num_samples);
            for (size_t i = 0; i < num_samples; ++i)
                samples.push_back(ph->homodyne(i % modes, 0.0));
            return samples;
        }

        void apply_qdp_noise(numqk::Tensor<double> &params, double epsilon)
        {
            std::random_device rd;
            std::mt19937 gen(rd());
            std::normal_distribution<double> sub_gaussian(0.0, epsilon);

            for (size_t i = 0; i < params.size(); ++i)
            {
                params.data()[i] += sub_gaussian(gen);
            }
        }

        qml::RVDLModel<double> train_stream(
            const std::string &export_path,
            size_t total_samples,
            const std::function<std::shared_ptr<rvdl::QObject>(size_t)> &get_sample,
            const TrainOptions &options)
        {
            if (!get_sample)
                throw std::invalid_argument("[QLM] train_stream: get_sample 回调为空");
            if (total_samples == 0)
                throw std::invalid_argument("[QLM] train_stream: total_samples == 0（没有可训练样本）");
            if (options.epochs <= 0)
                throw std::invalid_argument("[QLM] train_stream: epochs 必须 >= 1");
            if (!(options.lr > 0.0) || !std::isfinite(options.lr))
                throw std::invalid_argument("[QLM] train_stream: 学习率必须是有限正数");

            const auto t_start = std::chrono::steady_clock::now();

            const StateDecision state_decision = select_state_backend(
                options.state_backend, options.state_resources,
                options.device_adjoint_sparse_batch_pod != nullptr,
                options.device_adjoint_batch_pod != nullptr,
                options.sparse_epsilon);
            const bool sparse_ready =
                (state_decision.backend == StateBackend::Sparse ||
                 state_decision.backend == StateBackend::Ras) &&
                options.device_adjoint_sparse_batch_pod != nullptr &&
                options.use_adjoint;
            double state_sparse_skipped_all = 0.0, state_sparse_bound_all = 0.0;
            size_t state_sparse_active_all = 0, state_sparse_total_all = 0;

            std::unique_ptr<numqk::Tensor<double>> theta;
            std::vector<double> best_theta;
            std::vector<double> adam_m, adam_v;
            std::vector<double> grad;
            std::vector<double> pred;
            std::vector<train_detail::Complex> phi, lambda, ket;
            std::vector<train_detail::GateOp> gates;
            std::vector<size_t> readout_bits;
            std::vector<unsigned char> pod_type;
            std::vector<size_t> pod_q0, pod_q1, pod_param, pod_expert;
            std::vector<double> pod_scale;

            size_t n_q = 0;
            size_t total_params = 0;
            size_t samples_seen = 0;
            size_t optimizer_step = 0;
            int epochs_completed = 0;
            int best_epoch = 0;
            double best_epoch_loss = std::numeric_limits<double>::infinity();
            double best_sample_loss = std::numeric_limits<double>::infinity();
            double last_epoch_loss = 0.0;
            double last_epoch_accuracy = 0.0;
            int no_improve_epochs = 0;
            bool stopped = false;
            bool early_stopped = false;
            bool stopped_mid_epoch = false;
            bool adjoint_checked = false;
            bool use_adjoint = options.use_adjoint;

            auto init_model = [&](size_t n_qubits)
            {
                if (n_qubits < 8)
                    throw std::runtime_error(
                        "[QLM] train_stream: 需要至少 8 个 qubit（8 位读出），"
                        "当前样本只有 " + std::to_string(n_qubits) + " 个");
                n_q = n_qubits;
                total_params = num_layers * (n_q + 8);
                theta = std::make_unique<numqk::Tensor<double>>(
                    std::vector<size_t>{total_params}, true);
                grad.assign(total_params, 0.0);
                if (options.optimizer == Optimizer::Adam)
                {
                    adam_m.assign(total_params, 0.0);
                    adam_v.assign(total_params, 0.0);
                }

                if (options.init_weights)
                {
                    if (options.init_weights->size() != total_params)
                        throw std::runtime_error(
                            "[QLM] train_stream: 初始权重维度不匹配：模型有 " +
                            std::to_string(options.init_weights->size()) +
                            " 个参数，当前网络需要 " + std::to_string(total_params) +
                            " 个（qubits=" + std::to_string(n_q) +
                            ", layers=" + std::to_string(num_layers) +
                            "）。请用相同结构继续训练，或去掉 --continue 从零开始。");
                    for (size_t i = 0; i < total_params; ++i)
                        theta->data()[i] = (*options.init_weights)[i];
                }
                else
                {
                    std::mt19937 rng(options.seed);
                    std::normal_distribution<double> init(0.0, 0.01);
                    for (size_t i = 0; i < total_params; ++i)
                        theta->data()[i] = init(rng);
                }

                gates = train_detail::build_gates(n_q, num_layers, lapse_functions,
                                                  std::max<size_t>(1, options.moe_experts));
                readout_bits.resize(8);
                std::iota(readout_bits.begin(), readout_bits.end(), n_q - 8);
                pod_type.resize(gates.size());
                pod_q0.resize(gates.size());
                pod_q1.resize(gates.size());
                pod_param.resize(gates.size());
                pod_expert.resize(gates.size());
                pod_scale.resize(gates.size());
                for (size_t i = 0; i < gates.size(); ++i)
                {
                    const auto &g = gates[i];
                    pod_type[i] = static_cast<unsigned char>(
                        g.type == train_detail::GateOp::Type::RY ? 0 :
                        g.type == train_detail::GateOp::Type::RZ ? 1 : 2);
                    pod_q0[i] = g.q0;
                    pod_q1[i] = g.q1;
                    pod_param[i] = g.param;
                    pod_expert[i] = g.expert;
                    pod_scale[i] = g.scale;
                }
            };

            std::vector<size_t> order;
            std::mt19937 shuffle_rng(options.seed);

            double bat_skipped_mass_all = 0.0, bat_error_bound_all = 0.0;
            bool bat_adjoint_used = false;

            for (int epoch = 1; epoch <= options.epochs && !stopped; ++epoch)
            {
                if (options.on_epoch_start)
                    options.on_epoch_start(epoch, total_samples);

                if (options.shuffle)
                {
                    order.resize(total_samples);
                    std::iota(order.begin(), order.end(), size_t(0));
                    std::shuffle(order.begin(), order.end(), shuffle_rng);
                }

                double epoch_loss_sum = 0.0;
                double epoch_acc_sum = 0.0;
                size_t epoch_done = 0;

                const size_t B = std::max<size_t>(1, options.batch_size);
                const bool dense_batched = B > 1 && use_adjoint &&
                                           options.device_adjoint_batch_pod != nullptr;
                const bool batched = dense_batched || sparse_ready;
                std::vector<train_detail::Complex> batch_kets;
                std::vector<uint8_t> batch_targets;
                std::vector<double> batch_loss, batch_acc, batch_grad;
                double bat_skipped_mass = 0.0, bat_error_bound = 0.0;
                const bool minibatch_only = B > 1 && use_adjoint && !batched;
                std::vector<double> mb_grad;
                size_t mb_count = 0;

                auto do_step = [&](const std::vector<double> &g)
                {
                    train_detail::apply_optimizer_step(
                        *theta, g, adam_m, adam_v, optimizer_step, options);
                    for (size_t i = 0; i < total_params; ++i)
                        if (!std::isfinite(theta->data()[i]))
                            throw std::runtime_error(
                                "[QLM] 非有限权重 (NaN/Inf) at param " +
                                std::to_string(i) + ", sample " +
                                std::to_string(samples_seen) + "；训练已中止");
                };

                auto finalize_sample = [&](double sl, double sa) -> bool
                {
                    ++epoch_done;
                    ++samples_seen;
                    epoch_loss_sum += sl;
                    epoch_acc_sum += sa;
                    if (sl < best_sample_loss) best_sample_loss = sl;

                    const auto now = std::chrono::steady_clock::now();
                    const double elapsed =
                        std::chrono::duration<double>(now - t_start).count();
                    TrainProgress prog;
                    prog.epoch = epoch;
                    prog.epochs = options.epochs;
                    prog.sample_in_epoch = epoch_done;
                    prog.total_samples = total_samples;
                    prog.samples_seen = samples_seen;
                    prog.loss = sl;
                    prog.epoch_loss = epoch_loss_sum / static_cast<double>(epoch_done);
                    prog.best_loss = best_sample_loss;
                    prog.accuracy = sa;
                    prog.epoch_accuracy = epoch_acc_sum / static_cast<double>(epoch_done);
                    prog.samples_per_sec = (elapsed > 0.0) ? samples_seen / elapsed : 0.0;
                    prog.elapsed_sec = elapsed;
                    prog.params = total_params;
                    const size_t total_steps =
                        static_cast<size_t>(options.epochs) * total_samples;
                    const size_t remaining =
                        (total_steps > samples_seen) ? (total_steps - samples_seen) : 0;
                    prog.eta_sec = (prog.samples_per_sec > 0.0)
                        ? static_cast<double>(remaining) / prog.samples_per_sec : 0.0;
                    const bool stop_now = options.should_stop && options.should_stop();
                    prog.stopped = stop_now;
                    if (options.progress &&
                        (options.progress_every == 0 ||
                         (samples_seen % options.progress_every) == 0 || stop_now))
                        options.progress(prog);
                    if (!stopped && options.checkpoint && options.checkpoint_every > 0 &&
                        (samples_seen % options.checkpoint_every) == 0)
                        options.checkpoint(*theta, prog);
                    if (stop_now)
                    {
                        stopped = true;
                        stopped_mid_epoch = true;
                        return true;
                    }
                    return false;
                };

                for (size_t step = 0; step < total_samples; ++step)
                {
                    const size_t index = options.shuffle ? order[step] : step;
                    std::shared_ptr<rvdl::QObject> sample = get_sample(index);
                    if (!sample)
                        throw std::runtime_error(
                            "[QLM] train_stream: get_sample(" + std::to_string(index) +
                            ") 返回空样本");
                    if (!sample->qlm_data)
                        throw std::runtime_error(
                            "[QLM] train_stream: 样本 " + std::to_string(index) +
                            " 没有目标 token（qlm_data == nullptr）");

                    if (!theta)
                        init_model(sample->size());
                    else if (sample->size() != n_q)
                        throw std::runtime_error(
                            "[QLM] train_stream: 样本 qubit 数不一致：第一个样本 " +
                            std::to_string(n_q) + "，样本 " + std::to_string(index) +
                            " 是 " + std::to_string(sample->size()));

                    const uint8_t target_token =
                        *static_cast<uint8_t *>(sample->qlm_data);

                    if (options.prepare_sample)
                        options.prepare_sample(sample);
                    else if (samples_seen == 0 && epoch == 1)
                    {
                    }

                    double sample_loss = 0.0;
                    double sample_acc = 0.0;

                    if (use_adjoint)
                    {
                        std::vector<train_detail::Complex> full;
                        if (!adjoint_checked && n_q > 20)
                        {
                            adjoint_checked = true;
                            use_adjoint = false;
                            if (!options.prepare_sample)
                                throw std::runtime_error(
                                    "[QLM] train_stream: qubit 数 > 20，无法使用伴随梯度；"
                                    "parameter-shift 回退需要 prepare_sample 回调来重建每个"
                                    "样本的态；当前未提供。");
                        }
                        if (use_adjoint)
                        {
                            full = backend->get_state_vector();
                            if (!adjoint_checked)
                            {
                                adjoint_checked = true;
                                if (full.empty())
                                {
                                    use_adjoint = false;
                                    if (!options.prepare_sample)
                                        throw std::runtime_error(
                                            "[QLM] train_stream: 后端没有稠密态矢量，"
                                            "parameter-shift 回退需要 prepare_sample 回调来重建每个"
                                            "样本的态；当前未提供。");
                                }
                            }
                        }

                        if (use_adjoint)
                        {
                            if (full.empty())
                                throw std::runtime_error(
                                    "[QLM] train_stream: 训练中途无法读取后端态矢量");

                            const auto &ids = sample->get_ids();
                            const size_t ket_size = size_t(1) << n_q;
                            bool identity = (full.size() == ket_size);
                            if (identity)
                            {
                                for (size_t i = 0; i < n_q; ++i)
                                {
                                    if (ids[i] != i)
                                    {
                                        identity = false;
                                        break;
                                    }
                                }
                            }

                            if (identity)
                            {
                                ket = std::move(full);
                            }
                            else
                            {
                                size_t full_bits = 0;
                                while ((size_t(1) << full_bits) < full.size())
                                    ++full_bits;
                                if ((size_t(1) << full_bits) != full.size())
                                    throw std::runtime_error(
                                        "[QLM] train_stream: 后端态矢量长度不是 2 的幂");

                                ket.resize(ket_size);
                                for (size_t id : ids)
                                {
                                    if (id >= full_bits)
                                        throw std::runtime_error(
                                            "[QLM] train_stream: 样本 qubit id " +
                                            std::to_string(id) +
                                            " 超出后端态矢量范围（" +
                                            std::to_string(full_bits) + " qubits）");
                                }
                                for (size_t s = 0; s < ket_size; ++s)
                                {
                                    size_t full_idx = 0;
                                    for (size_t i = 0; i < n_q; ++i)
                                        if ((s >> i) & 1)
                                            full_idx |= size_t(1) << ids[i];
                                    ket[s] = full[full_idx];
                                }
                            }

                            if (batched)
                            {
                                batch_kets.insert(batch_kets.end(), ket.begin(), ket.end());
                                batch_targets.push_back(target_token);
                                const bool last = (step + 1 >= total_samples);
                                if (batch_targets.size() < B && !last) continue;
                                const size_t cur = batch_targets.size();
                                batch_loss.assign(cur, 0.0);
                                batch_acc.assign(cur, 0.0);
                                batch_grad.assign(total_params, 0.0);
                                double skipped = 0.0, bound = 0.0;
                                size_t active_tiles = 0, total_tiles = 0;
                                if (sparse_ready)
                                {
                                    AdjointRuntimeConfig cfg;
                                    cfg.tile_bits = options.tile_bits;
                                    cfg.tile_epsilon = options.tile_epsilon;
                                    cfg.sparse_tile_bits = options.sparse_tile_bits;
                                    cfg.sparse_epsilon = state_decision.epsilon;
                                    cfg.ras_rank = options.ras_rank;
                                    cfg.ras_epsilon = options.ras_epsilon > 0.0
                                        ? options.ras_epsilon : state_decision.epsilon;
                                    cfg.ras_seed = options.seed;
                                    cfg.moe_experts = std::max<size_t>(1, options.moe_experts);
                                    cfg.active_expert = 0;
                                    cfg.gate_expert = pod_expert.data();
                                    cfg.skipped_mass = &skipped;
                                    cfg.error_bound = &bound;
                                    cfg.active_tiles = &active_tiles;
                                    cfg.total_tiles = &total_tiles;
                                    options.device_adjoint_sparse_batch_pod(
                                        pod_type.data(), pod_q0.data(), pod_q1.data(),
                                        pod_param.data(), pod_scale.data(), gates.size(),
                                        reinterpret_cast<const double *>(batch_kets.data()),
                                        cur, ket.size(), theta->data(),
                                        readout_bits.data(), readout_bits.size(),
                                        batch_targets.data(),
                                        batch_grad.data(), total_params,
                                        batch_loss.data(), batch_acc.data(), &cfg);
                                    state_sparse_skipped_all = std::max(state_sparse_skipped_all, skipped);
                                    state_sparse_bound_all = std::max(state_sparse_bound_all, bound);
                                    state_sparse_active_all = std::max(state_sparse_active_all, active_tiles);
                                    state_sparse_total_all = std::max(state_sparse_total_all, total_tiles);
                                }
                                else
                                    options.device_adjoint_batch_pod(
                                        pod_type.data(), pod_q0.data(), pod_q1.data(),
                                        pod_param.data(), pod_scale.data(), gates.size(),
                                        reinterpret_cast<const double *>(batch_kets.data()),
                                        cur, ket.size(), theta->data(),
                                        readout_bits.data(), readout_bits.size(),
                                        batch_targets.data(),
                                        batch_grad.data(), total_params,
                                        batch_loss.data(), batch_acc.data(),
                                        options.tile_bits, options.tile_epsilon,
                                        &skipped, &bound);
                                bat_skipped_mass = std::max(bat_skipped_mass, skipped);
                                bat_error_bound = std::max(bat_error_bound, bound);
                                bat_skipped_mass_all = std::max(bat_skipped_mass_all, skipped);
                                bat_error_bound_all = std::max(bat_error_bound_all, bound);
                                bat_adjoint_used = true;
                                do_step(batch_grad);
                                bool stop = false;
                                for (size_t k = 0; k < cur; ++k)
                                {
                                    if (finalize_sample(batch_loss[k], batch_acc[k]))
                                    { stop = true; break; }
                                }
                                batch_kets.clear();
                                batch_targets.clear();
                                if (stop) break;
                                continue;
                            }

                            grad.assign(total_params, 0.0);
                            const size_t moe_g = std::max<size_t>(1, options.moe_experts);
                            const size_t active_expert = (moe_g > 1)
                                ? ((options.moe_active_offset + samples_seen) % moe_g) : 0;
                            if (options.device_adjoint_pod)
                            {
                                pred.assign(readout_bits.size(), 0.0);
                                sample_loss = options.device_adjoint_pod(
                                    pod_type.data(), pod_q0.data(), pod_q1.data(),
                                    pod_param.data(), pod_scale.data(), gates.size(),
                                    reinterpret_cast<const double *>(ket.data()), ket.size(),
                                    theta->data(), readout_bits.data(), readout_bits.size(),
                                    target_token,
                                    grad.data(), grad.size(), pred.data(), &sample_acc,
                                    moe_g, active_expert, pod_expert.data());
                            }
                            else
                                sample_loss = train_detail::adjoint_loss_grad(
                                    gates, ket, theta->data(), readout_bits, target_token,
                                    grad, pred, phi, lambda, sample_acc,
                                    moe_g, active_expert);
                        }
                    }

                    if (!use_adjoint)
                    {
                        if (!options.prepare_sample)
                            throw std::runtime_error(
                                "[QLM] train_stream: parameter-shift 回退需要 prepare_sample");

                        numqk::Tensor<double> work({total_params}, false);
                        for (size_t i = 0; i < total_params; ++i)
                            work.data()[i] = theta->data()[i];

                        std::vector<double> base(8, 0.0);
                        std::vector<double> plus(8, 0.0);
                        std::vector<double> minus(8, 0.0);
                        std::vector<double> gz(8, 0.0);

                        options.prepare_sample(sample);
                        this->circuit_expectation(sample, work, base.data());

                        sample_loss = 0.0;
                        sample_acc = 0.0;
                        for (size_t j = 0; j < 8; ++j)
                        {
                            const int bit = (target_token >> j) & 1;
                            const double target = 1.0 - 2.0 * bit;
                            const double d = base[j] - target;
                            sample_loss += d * d;
                            gz[j] = 2.0 * d / 8.0;
                            const int predicted_bit = (base[j] < 0.0) ? 1 : 0;
                            if (predicted_bit == bit)
                                sample_acc += 1.0;
                        }
                        sample_loss /= 8.0;
                        sample_acc /= 8.0;

                        const size_t per_layer = n_q + 8;
                        grad.assign(total_params, 0.0);
                        for (size_t i = 0; i < total_params; ++i)
                        {
                            const size_t layer = i / per_layer;
                            const double scale = lapse_functions[layer];
                            const double delta = (M_PI / 2.0) / scale;
                            const double orig = work.data()[i];

                            work.data()[i] = orig + delta;
                            options.prepare_sample(sample);
                            this->circuit_expectation(sample, work, plus.data());

                            work.data()[i] = orig - delta;
                            options.prepare_sample(sample);
                            this->circuit_expectation(sample, work, minus.data());

                            work.data()[i] = orig;

                            double g = 0.0;
                            for (size_t j = 0; j < 8; ++j)
                                g += 0.5 * (plus[j] - minus[j]) * gz[j];
                            grad[i] = g * scale;
                        }
                    }

                    if (!std::isfinite(sample_loss))
                        throw std::runtime_error(
                            "[QLM] 非有限损失 (NaN/Inf) at sample " +
                            std::to_string(samples_seen) + "；请降低学习率或检查语料");
                    if (minibatch_only)
                    {
                        if (mb_grad.size() != grad.size()) mb_grad.assign(grad.size(), 0.0);
                        for (size_t i = 0; i < grad.size(); ++i) mb_grad[i] += grad[i];
                        ++mb_count;
                        if (mb_count >= B || step + 1 >= total_samples)
                        {
                            for (double &x : mb_grad) x /= static_cast<double>(mb_count);
                            do_step(mb_grad);
                            std::fill(mb_grad.begin(), mb_grad.end(), 0.0);
                            mb_count = 0;
                        }
                    }
                    else do_step(grad);
                    if (finalize_sample(sample_loss, sample_acc))
                        break;
                }

                if (epoch_done == 0)
                    break;

                const bool epoch_complete = (epoch_done == total_samples);
                if (epoch_complete)
                    ++epochs_completed;
                last_epoch_loss = epoch_loss_sum / static_cast<double>(epoch_done);
                last_epoch_accuracy = epoch_acc_sum / static_cast<double>(epoch_done);

                if (epoch_complete &&
                    last_epoch_loss < best_epoch_loss - options.early_stop_min_delta)
                {
                    best_epoch_loss = last_epoch_loss;
                    best_epoch = epoch;
                    best_theta.assign(theta->data(), theta->data() + total_params);
                    no_improve_epochs = 0;
                }
                else if (epoch_complete)
                {
                    ++no_improve_epochs;
                }
                else if (best_epoch == 0)
                {
                    best_epoch_loss = last_epoch_loss;
                }

                const bool early =
                    (!stopped && epoch_complete && options.early_stop_patience > 0 &&
                     no_improve_epochs >= options.early_stop_patience);
                if (early)
                {
                    stopped = true;
                    early_stopped = true;
                }

                const auto now = std::chrono::steady_clock::now();
                const double elapsed =
                    std::chrono::duration<double>(now - t_start).count();

                TrainProgress epoch_prog;
                epoch_prog.epoch = epoch;
                epoch_prog.epochs = options.epochs;
                epoch_prog.sample_in_epoch = epoch_done;
                epoch_prog.total_samples = total_samples;
                epoch_prog.samples_seen = samples_seen;
                epoch_prog.loss = last_epoch_loss;
                epoch_prog.epoch_loss = last_epoch_loss;
                epoch_prog.best_loss = best_sample_loss;
                epoch_prog.accuracy = last_epoch_accuracy;
                epoch_prog.epoch_accuracy = last_epoch_accuracy;
                epoch_prog.samples_per_sec =
                    (elapsed > 0.0) ? samples_seen / elapsed : 0.0;
                epoch_prog.elapsed_sec = elapsed;
                epoch_prog.params = total_params;
                epoch_prog.epoch_end = true;
                epoch_prog.stopped = stopped;
                {
                    const size_t total_steps =
                        static_cast<size_t>(options.epochs) * total_samples;
                    const size_t remaining =
                        (total_steps > samples_seen) ? (total_steps - samples_seen) : 0;
                    epoch_prog.eta_sec =
                        (epoch_prog.samples_per_sec > 0.0)
                            ? static_cast<double>(remaining) / epoch_prog.samples_per_sec
                            : 0.0;
                }
                if (options.progress && !stopped_mid_epoch)
                    options.progress(epoch_prog);

                if (options.checkpoint && options.checkpoint_every == 0)
                    options.checkpoint(*theta, epoch_prog);

                if (early)
                    break;
            }

            if (!theta)
                throw std::runtime_error(
                    "[QLM] train_stream: 在第一个样本之前就停止了，没有可导出的权重");

            if (!stopped_mid_epoch && !best_theta.empty())
            {
                for (size_t i = 0; i < total_params; ++i)
                    theta->data()[i] = best_theta[i];
            }

            const auto t_end = std::chrono::steady_clock::now();
            const double elapsed_sec =
                std::chrono::duration<double>(t_end - t_start).count();

            auto to_str = [](double v)
            {
                if (!std::isfinite(v))
                    throw std::runtime_error(
                        "[QLM] 拒绝把 NaN/Inf 写进 .rvdlm 元数据（训练结果已损坏）");
                std::ostringstream ss;
                ss << std::setprecision(10) << v;
                return ss.str();
            };

            std::unordered_map<std::string, std::string> meta;
            meta["model.architecture"] = "QQNT";
            meta["model.dtype"] = "f64";
            meta["train.seed"] = std::to_string(options.seed);
            meta["train.shuffle"] = options.shuffle ? "1" : "0";
            meta["train.grad_clip"] = to_str(options.grad_clip);
            meta["train.weight_decay"] = to_str(options.weight_decay);
            meta["train.early_stop_patience"] = std::to_string(options.early_stop_patience);
            meta["train.optimizer_beta1"] = to_str(options.beta1);
            meta["train.optimizer_beta2"] = to_str(options.beta2);
            meta["topology.type"] =
                "Cross_Attention_" + std::to_string(n_q) + "Q";
            meta["model.ansatz"] = "RY_RZ_CNOT_v3";
            meta["model.param_layout"] =
                "per_layer:RY(n_q)+CNOT(ctx->readout)+RZ(8)";
            meta["model.readout_base"] = std::to_string(n_q - 8);
            meta["qng.metric"] = use_adjoint ? "Adjoint_MSE" : "ParameterShift_MSE";
            const size_t context_window = ((n_q + 1) / 2) * 4;
            meta["context.window"] = "LF2C_" + std::to_string(context_window) +
                                     "Tokens_" + std::to_string(n_q) + "Q";
            meta["context.encoder"] = "LF2C2";

            meta["train.optimizer"] = (options.optimizer == Optimizer::Adam) ? "adam" : "sgd";
            meta["train.learning_rate"] = to_str(options.lr);
            meta["train.epochs_requested"] = std::to_string(options.epochs);
            meta["train.epochs_completed"] = std::to_string(epochs_completed);
            meta["train.samples_per_epoch"] = std::to_string(total_samples);
            meta["train.total_steps"] =
                std::to_string(static_cast<size_t>(options.epochs) * total_samples);
            meta["train.steps_done"] = std::to_string(samples_seen);
            meta["train.qubits"] = std::to_string(n_q);
            meta["train.layers"] = std::to_string(num_layers);
            meta["train.params"] = std::to_string(total_params);
            meta["train.best_epoch"] = std::to_string(best_epoch);
            meta["train.best_loss"] = to_str(best_epoch_loss);
            meta["train.last_loss"] = to_str(last_epoch_loss);
            meta["train.last_accuracy"] = to_str(last_epoch_accuracy);
            meta["train.duration_sec"] = to_str(elapsed_sec);
            meta["train.stopped"] = stopped ? "1" : "0";
            meta["train.stopped_mid_epoch"] = stopped_mid_epoch ? "1" : "0";
            meta["train.early_stopped"] = early_stopped ? "1" : "0";
            meta["train.gradient"] = use_adjoint ? "adjoint" : "param_shift";
            meta["train.batch_size"] = std::to_string(std::max<size_t>(1, options.batch_size));
            meta["train.batched_adjoint"] = bat_adjoint_used ? "1" : "0";
            meta["train.tile_bits"] = std::to_string(options.tile_bits);
            meta["train.tile_epsilon"] = to_str(options.tile_epsilon);
            meta["train.tile_skipped_mass"] = to_str(bat_skipped_mass_all);
            meta["train.tile_error_bound"] = to_str(bat_error_bound_all);
            meta["train.state_backend"] = state_backend_name(state_decision.backend);
            meta["train.state_reason"] = state_decision.reason;
            meta["train.state_epsilon"] = to_str(state_decision.epsilon);
            meta["train.sparse_tile_bits"] = std::to_string(options.sparse_tile_bits);
            meta["train.sparse_active_backend"] = sparse_ready ? "1" : "0";
            meta["train.sparse_skipped_mass"] = to_str(state_sparse_skipped_all);
            meta["train.sparse_error_bound"] = to_str(state_sparse_bound_all);
            meta["train.sparse_active_tiles"] = std::to_string(state_sparse_active_all);
            meta["train.sparse_total_tiles"] = std::to_string(state_sparse_total_all);

            {
                const std::time_t tt = std::chrono::system_clock::to_time_t(
                    std::chrono::system_clock::now());
                std::tm tm_buf{};
#if defined(_WIN32)
                localtime_s(&tm_buf, &tt);
#else
                localtime_r(&tt, &tm_buf);
#endif
                char time_buf[40] = {0};
                std::strftime(time_buf, sizeof(time_buf), "%Y-%m-%dT%H:%M:%S", &tm_buf);
                meta["train.finished_at"] = time_buf;
            }

            if (options.dp_epsilon > 0.0)
            {
                apply_qdp_noise(*theta, options.dp_epsilon);
                meta["qdp.epsilon"] = to_str(options.dp_epsilon);
            }

            for (const auto &kv : options.extra_meta)
                meta[kv.first] = kv.second;

            qml::RVDLModel<double> finalized_model(*theta, meta);
            qml::ModelExporter<double>::save_atomic(export_path, finalized_model);
            return finalized_model;
        }

        qml::RVDLModel<double> train_and_export(
            int epochs, double lr, const std::string &export_path,
            std::vector<std::shared_ptr<rvdl::QObject>> &dataset,
            const std::vector<double> *init_weights = nullptr,
            const std::function<void(std::shared_ptr<rvdl::QObject> &)> &prepare_sample = nullptr,
            const std::unordered_map<std::string, std::string> &extra_meta = {})
        {
            if (dataset.empty())
                throw std::invalid_argument("[QLM] train_and_export: 数据集为空");
            if (dataset.size() > 1 && !prepare_sample)
                throw std::invalid_argument(
                    "[QLM] train_and_export: 多个样本共享同一批物理 qubit，但未提供 "
                    "prepare_sample。这会让训练在残留态上进行（旧实现的静默错误）。"
                    "请提供每样本重建回调，或改用 train_stream 流式接口。");

            TrainOptions options;
            options.epochs = epochs;
            options.lr = lr;
            options.init_weights = init_weights;
            options.extra_meta = extra_meta;

            if (prepare_sample)
            {
                options.prepare_sample = prepare_sample;
            }
            else
            {
                std::vector<std::complex<double>> captured = backend->get_state_vector();
                size_t captured_qubits = 0;
                if (!captured.empty())
                {
                    while ((size_t(1) << captured_qubits) < captured.size())
                        ++captured_qubits;
                    if ((size_t(1) << captured_qubits) != captured.size())
                        captured.clear();
                }
                if (!captured.empty())
                {
                    auto state = std::make_shared<const std::vector<std::complex<double>>>(
                        std::move(captured));
                    options.prepare_sample =
                        [this, state, captured_qubits](std::shared_ptr<rvdl::QObject> &)
                    {
                        backend->prepare_amplitudes(*state, captured_qubits);
                    };
                }
            }

            const size_t total = dataset.size();
            auto getter = [&dataset](size_t index) -> std::shared_ptr<rvdl::QObject>
            {
                return dataset[index];
            };
            return train_stream(export_path, total, getter, options);
        }

        static double flow_match_loss(const double *pred, uint8_t target_token)
        {
            double loss = 0.0;
            for (size_t j = 0; j < 8; ++j)
            {
                double x1 = 1.0 - 2.0 * ((target_token >> j) & 1);
                double diff = pred[j] - x1;
                loss += diff * diff;
            }
            return loss / 8.0;
        }

        void train_flow(std::vector<std::shared_ptr<rvdl::QObject>> &dataset,
                        int epochs, double lr, bool full_qgt = true)
        {
            size_t n_q = dataset.empty() ? num_qubits_ : dataset[0]->size();
            size_t total_params = num_layers * (n_q + 8);
            numqk::Tensor<double> theta({total_params}, true);
            for (size_t i = 0; i < total_params; ++i)
                theta.data()[i] = 0.01 * static_cast<double>(i);

            qml::VectorQuantumLayer<double> layer(
                backend,
                [this](qhal::IQuantumBackend *,
                       std::shared_ptr<rvdl::QObject> d,
                       numqk::Tensor<double> &p, double *out) {
                    this->circuit_expectation(d, p, out);
                },
                8);

            std::cout << "[QLM] Initiating State-Space Flow-Matching Optimization...\n";
            for (int e = 0; e < epochs; ++e)
            {
                double epoch_loss = 0.0;
                for (auto &sample : dataset)
                {
                    sample->reset_to_ground_state();
                    auto out = layer.forward(sample, theta);

                    uint8_t target_token = *static_cast<uint8_t *>(sample->qlm_data);

                    numqk::Tensor<double> g_out({8}, false);
                    double loss = 0.0;
                    for (size_t j = 0; j < 8; ++j)
                    {
                        double x1 = 1.0 - 2.0 * ((target_token >> j) & 1);
                        double diff = out.data()[j] - x1;
                        g_out.data()[j] = 2.0 * diff / 8.0;
                        loss += diff * diff;
                    }
                    loss /= 8.0;
                    epoch_loss += loss;

                    if (out.get_grad_fn())
                        out.get_grad_fn()->apply_backward(g_out);

                    if (full_qgt) apply_full_qng_update(theta, lr, sample);
                    else apply_qng_update(theta, lr, sample);
                    sample->reset_to_ground_state();
                }
                std::cout << "      [FLOW] Epoch " << e + 1 << "/" << epochs
                          << " | Flow-Match Loss: " << (epoch_loss / dataset.size()) << "\n";
            }
        }

        void train_flow_full_qng(std::vector<std::shared_ptr<rvdl::QObject>> &dataset,
                                 int epochs, double lr)
        {
            size_t n_q = dataset.empty() ? num_qubits_ : dataset[0]->size();
            size_t total_params = num_layers * (n_q + 8);
            numqk::Tensor<double> theta({total_params}, true);
            for (size_t i = 0; i < total_params; ++i)
                theta.data()[i] = 0.01 * static_cast<double>(i);

            qml::VectorQuantumLayer<double> layer(
                backend,
                [this](qhal::IQuantumBackend *,
                       std::shared_ptr<rvdl::QObject> d,
                       numqk::Tensor<double> &p, double *out) {
                    this->circuit_expectation(d, p, out);
                },
                8);

            std::cout << "[QLM] Initiating State-Space Flow-Matching with Full QGT Natural Gradient...\n";
            for (int e = 0; e < epochs; ++e)
            {
                double epoch_loss = 0.0;
                for (auto &sample : dataset)
                {
                    sample->reset_to_ground_state();
                    auto out = layer.forward(sample, theta);

                    uint8_t target_token = *static_cast<uint8_t *>(sample->qlm_data);

                    numqk::Tensor<double> g_out({8}, false);
                    double loss = 0.0;
                    for (size_t j = 0; j < 8; ++j)
                    {
                        double x1 = 1.0 - 2.0 * ((target_token >> j) & 1);
                        double diff = out.data()[j] - x1;
                        g_out.data()[j] = 2.0 * diff / 8.0;
                        loss += diff * diff;
                    }
                    loss /= 8.0;
                    epoch_loss += loss;

                    if (out.get_grad_fn())
                        out.get_grad_fn()->apply_backward(g_out);

                    apply_full_qng_update(theta, lr, sample);
                    sample->reset_to_ground_state();
                }
                std::cout << "      [FLOW-QGT] Epoch " << e + 1 << "/" << epochs
                          << " | Flow-Match Loss: " << (epoch_loss / dataset.size()) << "\n";
            }
        }
    };
}