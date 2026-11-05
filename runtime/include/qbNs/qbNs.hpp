#pragma once

#include <memory>
#include <stdexcept>
#include <iostream>
#include <functional>
#include <atomic>
#include <csignal>

#include "Transducer.hpp"
#include "rmx.hpp"
#include "../qhal/QM.hpp"
#include "../qhal/QVM.hpp"
#include "../qhal/IQuantumBackend.hpp"
#include "../../src/QObject.hpp"

namespace qbns
{
    // ─── QbNS：量子脑网络 混合架构（表层） ─────────────────────
    //
    // 由以下组件构成的顶级暴露接口：
    //   • Transducer  — 将神经信号编码为量子对象
    //   • Rmx         — 分布式混合网络 + 实时控制
    //   • QM / QVM    — 计算后端（真实/虚拟量子机）
    //
    // 支持四种脑机接口（BMI）模式：非侵入式、侵入式、无线、量子传感器。
    class QbNS
    {
    public:
        struct Config
        {
            BMIModality modality = BMIModality::NonInvasive;
            qhal::HardwareModality hardware_modality = qhal::HardwareModality::Superconducting;
            bool use_real_quantum_machine = false;
            size_t local_qubit_budget = 32;
        };

    private:
        std::unique_ptr<qhal::IQuantumBackend> compute_backend;
        std::unique_ptr<Transducer> transducer;
        std::unique_ptr<Rmx> network;
        BMIModality active_modality;
        std::atomic<bool> is_streaming{false};
        Config config;

        // 用于 QM/QVM 特定实时控制的原始指针（非所有权模式）
        qhal::QM *qm_ptr = nullptr;
        qhal::QVM *qvm_ptr = nullptr;
        size_t local_qc_node_id = 0;

        explicit QbNS(const Config &cfg) : active_modality(cfg.modality), config(cfg)
        {
            if (cfg.use_real_quantum_machine)
            {
                auto qm = std::make_unique<qhal::QM>(cfg.hardware_modality, 0);
                qm_ptr = qm.get();
                compute_backend = std::move(qm);
            }
            else
            {
                auto qvm = std::make_unique<qhal::QVM>();
                qvm_ptr = qvm.get();
                compute_backend = std::move(qvm);
            }

            transducer = std::make_unique<Transducer>(compute_backend.get());
            network = std::make_unique<Rmx>();

            // 自动注册本地 QC 节点
            QCNodeInfo local_node;
            local_node.type = cfg.use_real_quantum_machine ? QCNodeType::QM : QCNodeType::QVM;
            local_node.backend_ptr = compute_backend.get();
            local_node.available_qubits = cfg.local_qubit_budget;
            local_node.latency_ms = 0.0;
            local_node.is_active = true;
            local_qc_node_id = network->register_qc_node(local_node);

            std::cout << "[qbNS] Hybrid architecture unit online (modality: "
                      << static_cast<int>(active_modality)
                      << ", backend: " << (cfg.use_real_quantum_machine ? "QM" : "QVM")
                      << ").\n";
        }

    public:
        static std::unique_ptr<QbNS> create_with_qm(
            BMIModality modality,
            qhal::HardwareModality hw_modality = qhal::HardwareModality::Superconducting)
        {
            Config cfg;
            cfg.modality = modality;
            cfg.hardware_modality = hw_modality;
            cfg.use_real_quantum_machine = true;
            return std::unique_ptr<QbNS>(new QbNS(cfg));
        }

        static std::unique_ptr<QbNS> create_with_qvm(BMIModality modality)
        {
            Config cfg;
            cfg.modality = modality;
            cfg.use_real_quantum_machine = false;
            return std::unique_ptr<QbNS>(new QbNS(cfg));
        }

        ~QbNS()
        {
            stop_realtime_loop();
            std::cout << "[qbNS] Hybrid architecture unit shut down.\n";
        }

        QbNS(const QbNS &) = delete;
        QbNS &operator=(const QbNS &) = delete;

        // ─── 神经信号采集与编码 ──────────────────────────────
        //
        // 对每种神经信号类型的重载方法。
        // 当前活动的 BMIModality 决定了预期使用的信号类型，但所有重载方法均可用。

        std::shared_ptr<rvdl::QDataState> acquire_and_encode(const NeuralStream &signal)
        {
            is_streaming.store(true);
            auto encoded = transducer->amplitude_encode(signal);
            std::cout << "[qbNS] Acquired NeuralStream -> encoded QDataState ("
                      << encoded->size() << " qubits).\n";
            return encoded;
        }

        std::shared_ptr<rvdl::QDataState> acquire_and_encode(const SpikeTrain &signal)
        {
            is_streaming.store(true);
            auto encoded = transducer->spike_to_basis(signal);
            std::cout << "[qbNS] Acquired SpikeTrain -> encoded QDataState ("
                      << encoded->size() << " qubits).\n";
            return encoded;
        }

        std::shared_ptr<rvdl::QDataState> acquire_and_encode(const LocalFieldPotential &signal)
        {
            is_streaming.store(true);
            auto encoded = transducer->lfp_to_phase(signal);
            std::cout << "[qbNS] Acquired LFP -> encoded QDataState ("
                      << encoded->size() << " qubits).\n";
            return encoded;
        }

        std::shared_ptr<rvdl::QDataState> acquire_and_encode(const EEGSpectrum &signal)
        {
            is_streaming.store(true);
            auto encoded = transducer->eeg_to_entangled(signal);
            std::cout << "[qbNS] Acquired EEGSpectrum -> encoded QDataState ("
                      << encoded->size() << " qubits).\n";
            return encoded;
        }

        std::shared_ptr<rvdl::QDataState> acquire_and_encode(const QuantumSensorReading &signal)
        {
            is_streaming.store(true);
            auto encoded = transducer->sensor_to_state(signal);
            std::cout << "[qbNS] Acquired QuantumSensorReading -> encoded QDataState ("
                      << encoded->size() << " qubits).\n";
            return encoded;
        }

        // ─── 神经计算执行 ──────────────────────────────────────

        QuantumFeedback execute_neural_computation(std::shared_ptr<rvdl::QDataState> encoded)
        {
            auto qc_nodes = network->list_qc_nodes();
            if (qc_nodes.empty())
                throw std::runtime_error("[qbNS] No QC nodes available for computation.");

            QuantumFeedback fb = network->dispatch_to_qc(local_qc_node_id, encoded);
            is_streaming.store(false);
            std::cout << "[qbNS] Neural computation complete (latency: "
                      << fb.compute_latency_ms << " ms).\n";
            return fb;
        }

        // ─── 实时循环 ────────────────────────────────────────────────────

        void start_realtime_loop(std::function<void(const QuantumFeedback &)> callback)
        {
            network->start_realtime_loop(std::move(callback));
        }

        void stop_realtime_loop()
        {
            network->stop_realtime_loop();
            is_streaming.store(false);
        }

        void submit_control_command(const ControlCommand &cmd)
        {
            network->submit_command(cmd);
        }

        // ─── 网络节点管理 ───────────────────────────────────────────

        size_t connect_brain_node(const BrainNodeInfo &info)
        {
            return network->register_brain_node(info);
        }

        size_t connect_qc_node(const QCNodeInfo &info)
        {
            return network->register_qc_node(info);
        }

        void establish_network_link(size_t node_a, size_t node_b, double latency_ms)
        {
            network->establish_link(node_a, node_b, latency_ms);
        }

        // ─── 实时 QM/QVM 直接控制 ───────────────────────────────────

        void realtime_quantum_control(size_t remote_node, double latency_ms,
                                      size_t target_qubit = 0)
        {
            if (qm_ptr)
            {
                network->realtime_qm_control(qm_ptr, remote_node, latency_ms);
            }
            else if (qvm_ptr)
            {
                network->realtime_qvm_control(qvm_ptr, remote_node, target_qubit, latency_ms);
            }
        }

        // ─── 访问器 ─────────────────────────────────────────────────────────

        BMIModality get_modality() const { return active_modality; }
        bool get_is_streaming() const { return is_streaming.load(); }
        size_t get_local_qc_node_id() const { return local_qc_node_id; }
        qhal::IQuantumBackend *get_backend() const { return compute_backend.get(); }
        Transducer *get_transducer() const { return transducer.get(); }
        Rmx *get_network() const { return network.get(); }
    };
}
