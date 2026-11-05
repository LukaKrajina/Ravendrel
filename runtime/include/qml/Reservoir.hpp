#pragma once
//
// Reservoir.hpp —— 量子储备池计算（Quantum Reservoir Computing, QRC）
//
// 与 QLM（可训练变分语言模型）互补的另一条量子机器学习路径：
//   • 储备池（固定随机量子电路）**不参与梯度训练**，只做非线性/纠缠特征映射；
//   • 只有线性读出层（W·f + b）参与训练，是凸优化，**从原理上规避贫瘠高原**。
//
// 设计法则（DQNF 法则 1：耗散-可训练性对偶律）：
//   表征（酉演化）固定，学习（线性读出）可训练。固定电路即使退相干/噪声，
//   也仍是合法的特征映射，因此对 NISQ 硬件天然鲁棒。
//
// 与 QLM 的关系：二者并存。QRC 可作为 QLM 的"前端感知器"——
//   时序/连续信号先经 QRC 提取 ⟨Z⟩ 特征，再馈入 QLM 的变分电路做离散建模。
//
//
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

// 全局 qubit 分配器（inline 定义于 qml/Inference.hpp，与 ABI 层 QObject
// 共享同一 qubit id 空间）。储备池从中分配/回收独立 id，避免与 QObject 冲突。
#include "Inference.hpp"

namespace qml
{

    // ─── 量子储备池 ─────────────────────────────────────────────
    //
    // 固定随机电路 + 非破坏 ⟨Z⟩ 观测 + 线性读出（autograd 训练）。
    // 输入 x ∈ R^{num_qubits}（归一化到 [0,1]），经相位编码进入储备池，
    // 固定电路演化后读出每个 qubit 的 ⟨Z⟩ ∈ [-1,1] 作为特征向量。
    class QuantumReservoir
    {
    private:
        qhal::IQuantumBackend *backend;
        size_t num_qubits;   // 储备池 qubit 数（= 特征维度）
        size_t num_layers;   // 固定电路层数
        size_t out_dim;      // 读出维度（= 目标维度）
        uint64_t seed;

        // 固定随机旋转角：layer_angles[l][q]
        std::vector<std::vector<double>> layer_angles;
        // 固定 CNOT 拓扑（环形）：(control, target) 局部索引
        std::vector<std::pair<size_t, size_t>> cnot_topology;
        // 储备池实际占用的硬件 qubit id（从全局 id 空间分配，独立于 QObject）
        std::vector<size_t> qubit_ids_;

        // 线性读出：out = W · f + b，f 为 ⟨Z⟩ 特征列向量
        std::unique_ptr<numqk::Tensor<double>> W; // [out_dim, num_qubits]
        std::unique_ptr<numqk::Tensor<double>> b; // [out_dim, 1]

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

            // 环形 CNOT 拓扑：q -> (q+1) mod N
            cnot_topology.reserve(num_qubits);
            for (size_t q = 0; q < num_qubits; ++q)
                cnot_topology.emplace_back(q, (q + 1) % num_qubits);

            // 读出权重初始化（小随机值，requires_grad=true）
            W = std::make_unique<numqk::Tensor<double>>(
                std::vector<size_t>{this->out_dim, num_qubits}, true);
            b = std::make_unique<numqk::Tensor<double>>(
                std::vector<size_t>{this->out_dim, 1}, true);
            std::normal_distribution<double> wdist(0.0, 0.1);
            for (size_t i = 0; i < W->size(); ++i)
                W->data()[i] = wdist(rng);
            for (size_t i = 0; i < b->size(); ++i)
                b->data()[i] = wdist(rng);

            // 从全局 qubit id 空间分配独立 qubit（复用已释放 id），避免与 QObject 冲突
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
            // 回收储备池 qubit：release_qubit 内部已复位到 |0⟩，再回收到空闲池
            for (size_t q = 0; q < num_qubits; ++q)
            {
                size_t id = qubit_ids_[q];
                backend->release_qubit(id);
                rvdl_free_qubit_id(id);
            }
        }

        // ─── 前向演化：输入 → 编码 → 固定电路 → ⟨Z⟩ 特征 ──────────
        //
        // 每次演化前把储备池 reset 到 |0⟩，独立采样。
        // 返回长度为 num_qubits 的 ⟨Z⟩ 特征向量（∈ [-1,1]）。
        std::vector<double> evolve(const std::vector<double> &input)
        {
            // 1) reset 到 |0⟩
            for (size_t q = 0; q < num_qubits; ++q)
            {
                size_t id = qubit_ids_[q];
                if (backend->measure(id) == 1)
                    backend->apply_x(id);
            }

            // 2) H 进入叠加 + 相位编码（input[i] -> Rz 相位）
            for (size_t q = 0; q < num_qubits; ++q)
            {
                size_t id = qubit_ids_[q];
                backend->apply_h(id);
                double x = (q < input.size()) ? input[q] : 0.0;
                // 归一化保护：[0,1] -> [0, 2π]
                if (x < 0.0) x = 0.0;
                if (x > 1.0) x = 1.0;
                backend->apply_rz(id, x * 2.0 * M_PI);
            }

            // 3) 固定随机电路（表征层，不训练）
            for (size_t l = 0; l < num_layers; ++l)
            {
                for (size_t q = 0; q < num_qubits; ++q)
                    backend->apply_rz(qubit_ids_[q], layer_angles[l][q]);
                for (const auto &cn : cnot_topology)
                    backend->apply_cnot(qubit_ids_[cn.first], qubit_ids_[cn.second]);
            }

            // 4) 非破坏 ⟨Z⟩ 观测
            std::vector<double> feats(num_qubits);
            for (size_t q = 0; q < num_qubits; ++q)
                feats[q] = backend->expectation_z(qubit_ids_[q]);

            return feats;
        }

        // ─── 线性读出前向（autograd）────────────────────────────
        // f ∈ R^{num_qubits} -> out = W·f + b ∈ R^{out_dim}
        numqk::Tensor<double> readout_forward(const std::vector<double> &feats)
        {
            numqk::Tensor<double> f({num_qubits, 1}, false);
            for (size_t i = 0; i < num_qubits; ++i)
                f.data()[i] = feats[i];

            numqk::Tensor<double> out = W->matmul(f); // [out_dim, 1]
            out = out.add(*b);
            return out;
        }

        // ─── 训练线性读出（SGD + autograd，MSE 损失）─────────────
        //
        // 储备池部分固定，只有 W、b 更新。因为 MSE 对 W、b 是凸的，
        // 梯度下降必然收敛——这是 QRC"无贫瘠高原"的结构性保证。
        void train(const std::vector<std::vector<double>> &inputs,
                   const std::vector<std::vector<double>> &targets,
                   int epochs,
                   double lr)
        {
            if (inputs.empty() || inputs.size() != targets.size())
                throw std::runtime_error("[QRC] train: input/target count mismatch.");

            // 预提取特征（储备池固定，只算一次）
            std::vector<std::vector<double>> X;
            X.reserve(inputs.size());
            for (const auto &in : inputs)
                X.push_back(evolve(in));

            for (int e = 0; e < epochs; ++e)
            {
                double epoch_loss = 0.0;
                for (size_t s = 0; s < X.size(); ++s)
                {
                    // 目标列向量 [out_dim, 1]
                    numqk::Tensor<double> t({out_dim, 1}, false);
                    for (size_t j = 0; j < out_dim; ++j)
                        t.data()[j] = (j < targets[s].size()) ? targets[s][j] : 0.0;

                    auto out = readout_forward(X[s]);  // [out_dim,1]
                    auto diff = out.sub(t);            // [out_dim,1]
                    auto loss = diff.mul(diff).mean(); // {1} 标量 MSE

                    loss.backward();

                    // SGD 更新 W、b
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

        // ─── 预测 ────────────────────────────────────────────────
        std::vector<double> predict(const std::vector<double> &input)
        {
            auto feats = evolve(input);
            auto out = readout_forward(feats);
            std::vector<double> y(out_dim);
            for (size_t j = 0; j < out_dim; ++j)
                y[j] = out.data()[j];
            return y;
        }

        // ─── 访问器 ──────────────────────────────────────────────
        size_t qubits() const { return num_qubits; }
        size_t layers() const { return num_layers; }
        size_t output_dim() const { return out_dim; }
        const std::vector<std::vector<double>> &fixed_angles() const { return layer_angles; }
    };
}