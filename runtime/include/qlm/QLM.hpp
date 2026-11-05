#pragma once
#include "../numqk/Numqk.hpp"
#include "../qhal/IQuantumBackend.hpp"
#include "../qhal/PhotonicBackend.hpp"
#include "../qml/Layer.hpp"
#include "../qml/RVDLModelFormat.hpp"
#include "../qml/QGT.hpp"
#include "../../src/QObject.hpp"
#include "../../src/QDataEncoder.hpp"
#include <memory>
#include <vector>
#include <cmath>
#include <random>
#include <string>
#include <unordered_map>
#include <iostream>

namespace qlm
{
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

            uint8_t target_token = *static_cast<uint8_t*>(input_data->qlm_data);
            size_t param_idx = 0;
            std::mt19937 rng(42);
            std::uniform_real_distribution<double> dropout_dist(0.0, 1.0);

            for (size_t t = 0; t < num_layers; ++t)
            {
                double N_t = lapse_functions[t];

                for (size_t i = 0; i < n_q; ++i) {
                    if (param_idx < params.size()) {
                        backend->apply_rz(active_qubits[i], params.data()[param_idx++] * N_t);
                    }
                }

                for (size_t i = 0; i < 8; ++i) {
                    if (dropout_dist(rng) > 0.15) {
                        backend->apply_cnot(active_qubits[i], active_qubits[i + 8]);
                    }
                    if (param_idx < params.size()) {
                        backend->apply_rz(active_qubits[i + 8], params.data()[param_idx++] * N_t);
                    }
                }
            }

            double loss = 0.0;
            for (size_t i = 0; i < 8; ++i)
            {
                int measured_bit = backend->measure(active_qubits[i + 8]);
                int target_bit = (target_token >> i) & 1;
                
                if (measured_bit != target_bit) {
                    loss += 1.0;
                }
            }
            
            return loss / 8.0;
        }

        // ── 连续期望通道，输出 8 维 ⟨Z⟩，不坍缩态 ──────────
        // 门序列与 circuit_ansatz 完全一致，区别仅在最后读取的是
        // 非破坏 Z 期望 ⟨Z⟩ ∈ [-1,1] 而非坍缩比特，作为流匹配软信号。
        // 签名匹配 qml::VectorQuantumLayer 的 circuit_fn。
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

            size_t param_idx = 0;
            std::mt19937 rng(42);
            std::uniform_real_distribution<double> dropout_dist(0.0, 1.0);

            for (size_t t = 0; t < num_layers; ++t)
            {
                double N_t = lapse_functions[t];
                for (size_t i = 0; i < n_q; ++i)
                {
                    if (param_idx < params.size())
                        backend->apply_rz(active_qubits[i], params.data()[param_idx++] * N_t);
                }
                for (size_t i = 0; i < 8; ++i)
                {
                    if (dropout_dist(rng) > 0.15)
                        backend->apply_cnot(active_qubits[i], active_qubits[i + 8]);
                    if (param_idx < params.size())
                        backend->apply_rz(active_qubits[i + 8], params.data()[param_idx++] * N_t);
                }
            }

            for (size_t j = 0; j < 8; ++j)
                out[j] = backend->expectation_z(active_qubits[j + 8]);
        }

        // ── 态矢量通道：reset + 跑电路 + 读态矢量（非坍缩）──────
        // 供完整 QGT 计算（parameter-shift 需要重跑电路读态）。
        // 门序列与 circuit_expectation 一致（dropout 固定 seed 42 可复现）。
        numqk::CTensor circuit_state_ket(std::shared_ptr<rvdl::QObject> input_data,
                                         const numqk::Tensor<double> &params) const
        {
            const auto &active_qubits = input_data->get_ids();
            const size_t n_q = active_qubits.size();
            input_data->reset_to_ground_state();
            if (n_q == 0)
                return numqk::CTensor({0}, false);

            size_t param_idx = 0;
            std::mt19937 rng(42);
            std::uniform_real_distribution<double> dropout_dist(0.0, 1.0);

            for (size_t t = 0; t < num_layers; ++t)
            {
                double N_t = lapse_functions[t];
                for (size_t i = 0; i < n_q; ++i)
                    if (param_idx < params.size())
                        backend->apply_rz(active_qubits[i], params.data()[param_idx++] * N_t);
                for (size_t i = 0; i < 8; ++i)
                {
                    if (dropout_dist(rng) > 0.15)
                        backend->apply_cnot(active_qubits[i], active_qubits[i + 8]);
                    if (param_idx < params.size())
                        backend->apply_rz(active_qubits[i + 8], params.data()[param_idx++] * N_t);
                }
            }

            std::vector<std::complex<double>> sv = backend->get_state_vector();
            numqk::CTensor ket({sv.size()}, false);
            for (size_t i = 0; i < sv.size(); ++i)
                ket.data()[i] = sv[i];
            return ket;
        }

        // ── 完整量子几何张量（Fubini-Study 度量）────────────────
        // 用 qml::quantum_geometric_tensor + 态矢量 parameter-shift 计算完整
        // QGT（含对角与非对角元），替代 estimate_fs_diagonal 的对角近似。
        numqk::Tensor<double> compute_full_qgt(std::shared_ptr<rvdl::QObject> input_data,
                                               numqk::Tensor<double> &params)
        {
            return qml::quantum_geometric_tensor<double>(
                [this, input_data](const numqk::Tensor<double> &p) {
                    return this->circuit_state_ket(input_data, p);
                },
                params);
        }

        // ── 完整 QGT 自然梯度更新 ────────────────────────────────
        // θ ← θ - lr · G⁺ ∇L，其中 G 为完整 Fubini-Study 度量，∇L 为欧氏梯度
        // （由 parameter-shift 内积回传累积在 params.grad 中）。
        void apply_full_qng_update(numqk::Tensor<double> &params, double learning_rate,
                                   std::shared_ptr<rvdl::QObject> input_data)
        {
            auto grad = params.get_grad();

            // 拷贝欧氏梯度（compute_full_qgt 会临时移位 params，但不动 grad）
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

        // ── 真实 Fubini-Study 对角度量（基于 ⟨Z⟩ 方差估计）──────
        // 替换旧的硬编码 g_ii = 0.25·N_t²：对 RZ 门，Fubini-Study 度量对角元
        // 为 g_ii = (1/4)(1 - ⟨Z_q⟩²)，其中 q 是参数 i 作用的 qubit。
        // 无纠缠时该式精确；有纠缠后是对角近似，仍比硬编码 0.25·N_t² 更贴合真实度量。
        // 参数→qubit 映射：within = i % (n_q + 8)；
        //   q = within < n_q ? within : 8 + (within - n_q)
        std::vector<double> estimate_fs_diagonal(numqk::Tensor<double> &params,
                                                 std::shared_ptr<rvdl::QObject> input_data)
        {
            const auto &active_qubits = input_data->get_ids();
            size_t n_q = active_qubits.size();
            size_t total = params.size();
            std::vector<double> diag(total, 0.25);
            if (n_q == 0)
                return diag;

            // 运行电路（与 circuit_ansatz 一致），读所有 n_q 个 qubit 的 ⟨Z⟩
            input_data->reset_to_ground_state();
            size_t param_idx = 0;
            std::mt19937 rng(42);
            std::uniform_real_distribution<double> dropout_dist(0.0, 1.0);

            for (size_t t = 0; t < num_layers; ++t)
            {
                double N_t = lapse_functions[t];
                for (size_t i = 0; i < n_q; ++i)
                    if (param_idx < params.size())
                        backend->apply_rz(active_qubits[i], params.data()[param_idx++] * N_t);
                for (size_t i = 0; i < 8; ++i)
                {
                    if (dropout_dist(rng) > 0.15)
                        backend->apply_cnot(active_qubits[i], active_qubits[i + 8]);
                    if (param_idx < params.size())
                        backend->apply_rz(active_qubits[i + 8], params.data()[param_idx++] * N_t);
                }
            }

            std::vector<double> z(n_q, 0.0);
            for (size_t q = 0; q < n_q; ++q)
                z[q] = backend->expectation_z(active_qubits[q]);

            size_t per_layer = n_q + 8;
            for (size_t i = 0; i < total; ++i)
            {
                size_t within = i % per_layer;
                size_t q = (within < n_q) ? within : (8 + (within - n_q));
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

            // Fubini-Study 对角元（基于 ⟨Z⟩ 方差）做自然梯度
            auto diag = estimate_fs_diagonal(params, input_data);

            for (size_t i = 0; i < params.size(); ++i)
            {
                double natural_grad = grad->data()[i] / diag[i];
                params.data()[i] -= learning_rate * natural_grad;
                grad->data()[i] = 0.0;                 // 清零，避免跨步梯度累积
            }
        }

        // ── 光子后端生成采样（GKP / 连续变量高斯采样）──────────
        // 若 backend 是 PhotonicBackend，用其 homodyne 正交分量采样产生
        // 连续变量样本（高斯玻色采样风格）。这是"量子采样优势"最实的路径：
        // 光子 GBS 是实验验证的量子优势候选。
        // 非光子后端返回空，调用方应回退到 ⟨Z⟩ 期望通道。
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

        qml::RVDLModel<double> train_and_export(int epochs, double lr, const std::string &export_path, std::vector<std::shared_ptr<rvdl::QObject>>& dataset, const std::vector<double>* init_weights = nullptr)
        {
            size_t n_q = dataset[0]->size();
            size_t total_params = num_layers * (n_q + 8); 
            numqk::Tensor<double> theta({total_params}, true);

            // 增量训练（「烧录」）：若提供已有权重（从 .rvdlm 加载），从它继续训练，
            // 否则从零初始化。这样每次烧录在现有能力上渐进增强，而非推倒重来。
            if (init_weights && init_weights->size() == total_params)
            {
                for (size_t i = 0; i < total_params; ++i)
                    theta.data()[i] = (*init_weights)[i];
            }
            else
            {
                for (size_t i = 0; i < total_params; ++i)
                    theta.data()[i] = 0.01 * static_cast<double>(i);
            }

            auto bound_circuit = [this](qhal::IQuantumBackend *be, std::shared_ptr<rvdl::QObject> data, numqk::Tensor<double> &p)
            {
                return this->circuit_ansatz(data, p);
            };
            qml::QuantumLayer<double> layer(backend, bound_circuit);

            std::cout << "[QLM] Initiating Quantum Attention Optimization...\n";
            for (int e = 0; e < epochs; ++e)
            {
                double epoch_loss = 0.0;
                for (auto& sample : dataset) {
                    numqk::Tensor<double> out = layer.forward(sample, theta);
                    out.backward();
                    apply_qng_update(theta, lr, sample);
                    epoch_loss += out.data()[0];
                    sample->reset_to_ground_state();
                }
                std::cout << "      Epoch " << e+1 << "/" << epochs << " | Hamming Loss: " << (epoch_loss / dataset.size()) << "\n";
            }

            std::cout << "[QLM] Applying ITA Sub-Gaussian Privacy Perturbation...\n";
            apply_qdp_noise(theta, 0.015);
            std::unordered_map<std::string, std::string> meta;
            meta["model.architecture"] = "QQNT";
            meta["topology.type"] = "Cross_Attention_16Q";
            meta["qdp.epsilon"] = "0.015";
            meta["qng.metric"] = "Diagonal_Fubini_Study";
            // 上下文窗口：LF2C 无损特征快速上下文压缩。
            // 原来是 "2_Tokens"（硬编码两个 token = 16 qubit），现在同样
            // 16 个 qubit 装的是**压缩特征**，覆盖 32 个 token 的历史
            // （qlm::lf2c2::ContextConfig 的默认：块 4 token / 每块 2 维
            // 特征 ⇒ 16 qubit × 2 token/特征 = 32 token）。
            // 训练与推理两侧用同一个 qlm::lf2c2::encode_context，编码一致。
            meta["context.window"] = "LF2C_32Tokens_16Q";
            meta["context.encoder"] = "LF2C2";
            
            qml::RVDLModel<double> finalized_model(theta, meta);
            qml::ModelExporter<double>::save(export_path, finalized_model);

            return finalized_model;
        }

        // ── 态空间流匹配损失 ────────────────────────────
        // 端点参数化 + 态空间线性插值使 L_fm 退化为「时间加权的端点回归」：
        //   L_fm = (1/8) Σ_j (x̂_1[j] - x_1[j])² ,  x_1 = 2·target - 1 ∈ {±1}
        // 返回损失标量，梯度由调用方构造 g_out 后经向量化 backward 回传。
        static double flow_match_loss(const double *pred, uint8_t target_token)
        {
            double loss = 0.0;
            for (size_t j = 0; j < 8; ++j)
            {
                double x1 = 2.0 * ((target_token >> j) & 1) - 1.0;
                double diff = pred[j] - x1;
                loss += diff * diff;
            }
            return loss / 8.0;
        }

        // ── 连续通道训练：态空间流匹配 ───────────────
        // 用 VectorQuantumLayer 输出 8 维 ⟨Z⟩ 期望，流匹配损失为端点回归，
        // 梯度通过向量化 parameter-shift 内积回传。
        // 绕过 Tensor::backward() 中 grad=1 的硬编码，直接调用
        // grad_fn->apply_backward(g_out)，其中 g_out = ∂L_fm/∂⟨Z⟩。
        // full_qgt（默认 true）—— 自然梯度所用的 Fubini-Study 度量精度：
        //   true  : apply_full_qng_update，完整度量（含对角与非对角元），
        //           精确体现参数空间的量子几何，成本 O(p²)（p = 参数数）。
        //   false : apply_qng_update，仅对角元估计（estimate_fs_diagonal，
        //           基于 ⟨Z⟩ 方差），成本 O(p)，但有纠缠时只是近似。
        //           这是原实现的默认路径，现降级为显式选择的快速路径。
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
                    auto out = layer.forward(sample, theta);   // Tensor({8}) = ⟨Z⟩

                    uint8_t target_token = *static_cast<uint8_t *>(sample->qlm_data);

                    numqk::Tensor<double> g_out({8}, false);   // ∂L_fm/∂⟨Z⟩
                    double loss = 0.0;
                    for (size_t j = 0; j < 8; ++j)
                    {
                        double x1 = 2.0 * ((target_token >> j) & 1) - 1.0;
                        double diff = out.data()[j] - x1;
                        g_out.data()[j] = 2.0 * diff / 8.0;     // 平方误差梯度
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

        // ── 连续通道训练：态空间流匹配 + 完整 QGT 自然梯度 ─────
        // 与 train_flow 相同，但用 apply_full_qng_update（完整 Fubini-Study 度量，
        // 含对角与非对角元）替代 apply_qng_update（对角近似），体现 TQNF
        // 「几何-贫瘠高原对偶律」。欧氏梯度经 vector parameter-shift 内积回传。
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
                    auto out = layer.forward(sample, theta);   // Tensor({8}) = ⟨Z⟩

                    uint8_t target_token = *static_cast<uint8_t *>(sample->qlm_data);

                    numqk::Tensor<double> g_out({8}, false);   // ∂L_fm/∂⟨Z⟩
                    double loss = 0.0;
                    for (size_t j = 0; j < 8; ++j)
                    {
                        double x1 = 2.0 * ((target_token >> j) & 1) - 1.0;
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