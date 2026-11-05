#pragma once
//
// FieldReservoir.hpp —— 耗散量子神经场（DQNF 范式核心组件）
//
// 把皮层神经场方程（球面上的 Amari 型双阱神经场，见 spacetime/NeuralField.hpp）
// 直接作为**连续耗散储备池**：神经场提供无穷维非线性动力学与"意识状态"序参量，
// 量子储备池（QuantumReservoir）作为测量探头，采样场值编码为量子态并读出 ⟨Z⟩ 特征。
//
// 设计法则（DQNF 法则 5：场-储备池统一律）：
//   神经场方程本身就是耗散储备池，量子态是其测量探头——神经信号到量子态
//   无需显式编码，直接物理/数值耦合，消除了 Transducer 的启发式编码误差。
//
// 与 QLM / QRC 的关系：
//   NeuralField（连续场）→ 采样 → QuantumReservoir（量子探头）→ ⟨Z⟩ 特征
//   → 线性读出（可训练）。QLM 可作为下游离散建模器继续消费这些特征。
//
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
        size_t num_probes_;   // 从场格点采样的点数（= 储备池 qubit 数）

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

        // ─── 从当前场值均匀采样 num_probes_ 个点，归一化到 [0,1] ───
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

        // ─── 步进神经场 + 量子探头，返回 ⟨Z⟩ 特征 ────────────────
        std::vector<double> step_and_probe()
        {
            field_->step();
            return reservoir_->evolve(sample_field());
        }

        // ─── 收集 T 步特征序列（供训练 / 下游 QLM 消费）──────────
        std::vector<std::vector<double>> collect_sequence(size_t steps)
        {
            std::vector<std::vector<double>> seq;
            seq.reserve(steps);
            for (size_t t = 0; t < steps; ++t)
                seq.push_back(step_and_probe());
            return seq;
        }

        // ─── 训练线性读出（复用 QRC 的 autograd 训练）────────────
        // inputs 为每步的场采样序列；targets 为对应标签序列。
        void train(const std::vector<std::vector<double>> &inputs,
                   const std::vector<std::vector<double>> &targets,
                   int epochs,
                   double lr)
        {
            reservoir_->train(inputs, targets, epochs, lr);
        }

        // ─── 预测（步进场后读出）────────────────────────────────
        std::vector<double> predict()
        {
            return reservoir_->predict(sample_field());
        }

        // ─── 序参量（意识状态的连续场刻画）──────────────────────
        rvdl::spacetime::NeuralFieldOrderParameter order_parameter() const
        {
            return field_->order_parameter();
        }

        // ─── 底层访问 ────────────────────────────────────────────
        QuantumReservoir *reservoir() { return reservoir_.get(); }
        rvdl::spacetime::NeuralField *field() { return field_.get(); }
        size_t num_probes() const { return num_probes_; }
    };

}