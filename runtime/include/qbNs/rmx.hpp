#pragma once

#include <vector>
#include <string>
#include <memory>
#include <stdexcept>
#include <iostream>
#include <unordered_map>
#include <map>
#include <queue>
#include <mutex>
#include <atomic>
#include <thread>
#include <condition_variable>
#include <functional>
#include <chrono>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#ifndef M_PI_2
#define M_PI_2 1.57079632679489661923
#endif

#include "../qhal/IQuantumBackend.hpp"
#include "../qhal/QM.hpp"
#include "../qhal/QVM.hpp"
#include "../../src/QObject.hpp"

namespace qbns
{
    // ─── BMI 测量方式 ──────────────────────────────────────────────────────────

    enum class BMIModality
    {
        NonInvasive,    // 脑电图（EEG）/功能性近红外光谱（fNIRS） 头皮电极
        Invasive,       // 脑皮层电图（ECoG）/ 皮层内微电极阵列
        Wireless,       // 无线BMI植入式遥测
        QuantumSensor   // NV中心 / SQUID量子传感器接口
    };

    // ─── QC 节点类型 ──────────────────────────────────────────────────────────

    enum class QCNodeType
    {
        QM,   // 真实量子机
        QVM   // 量子虚拟机
    };

    // ─── 脑节点信息 ────────────────────────────────────────────────

    struct BrainNodeInfo
    {
        size_t node_id = 0;
        std::string subject_label;
        BMIModality modality = BMIModality::NonInvasive;
        double data_rate_hz = 1000.0;
        bool is_active = true;
    };

    // ─── QC节点信息 ───────────────────────────────────────────────────

    struct QCNodeInfo
    {
        size_t node_id = 0;
        QCNodeType type = QCNodeType::QVM;
        qhal::IQuantumBackend *backend_ptr = nullptr;
        size_t available_qubits = 0;
        double latency_ms = 0.0;
        bool is_active = true;
    };

    // ─── 控制指令 ───────────────────────────────────────────────────────

    struct ControlCommand
    {
        enum class Type
        {
            QuantumCompute,
            NeuralStimulation,
            StateFeedback,
            EmergencyStop
        };

        Type type = Type::QuantumCompute;
        size_t target_node_id = 0;
        std::vector<size_t> qubit_targets;
        std::function<void(qhal::IQuantumBackend *)> gate_sequence;
        double priority = 0.5;
    };

    // ─── 量子反馈 ──────────────────────────────────────────────────────

    struct QuantumFeedback
    {
        std::vector<int> measurement_results;
        double compute_latency_ms = 0.0;
        size_t source_node_id = 0;
        bool success = true;
    };

    // ─── Rmx：分布式混合网络 + 实时控制系统  ─────────────
    //
    // 在楔总线（KarmaBus） 分布式量子网络原语的基础上
    // 扩展了多脑和多量子计算节点注册、自适应路由
    // 以及一个实时闭环控制线程，线程负责将编码后的量子对象分发至 QM/QVM 后端

    class Rmx
    {
    private:
        mutable std::mutex topology_mutex;
        std::unordered_map<size_t, BrainNodeInfo> brain_nodes;
        std::unordered_map<size_t, QCNodeInfo> qc_nodes;
        std::map<std::pair<size_t, size_t>, double> link_latency_table;
        size_t next_node_id = 1;

        std::atomic<bool> loop_running{false};
        std::thread control_thread;
        std::mutex queue_mutex;
        std::condition_variable queue_cv;
        std::queue<ControlCommand> command_queue;
        std::function<void(const QuantumFeedback &)> feedback_callback;

        static double timestamp_ms()
        {
            return std::chrono::duration<double, std::milli>(
                       std::chrono::steady_clock::now().time_since_epoch())
                .count();
        }

        void execute_command(const ControlCommand &cmd)
        {
            double t_start = timestamp_ms();

            if (cmd.type == ControlCommand::Type::EmergencyStop)
            {
                std::cout << "[Rmx] EMERGENCY STOP received for node "
                          << cmd.target_node_id << ".\n";
                {
                    std::lock_guard<std::mutex> lock(topology_mutex);
                    auto it = qc_nodes.find(cmd.target_node_id);
                    if (it != qc_nodes.end())
                        it->second.is_active = false;
                }
                QuantumFeedback fb;
                fb.source_node_id = cmd.target_node_id;
                fb.success = true;
                fb.compute_latency_ms = timestamp_ms() - t_start;
                if (feedback_callback) feedback_callback(fb);
                return;
            }

            QuantumFeedback fb;
            fb.source_node_id = cmd.target_node_id;

            qhal::IQuantumBackend *backend = nullptr;
            {
                std::lock_guard<std::mutex> lock(topology_mutex);
                auto it = qc_nodes.find(cmd.target_node_id);
                if (it == qc_nodes.end() || !it->second.is_active)
                {
                    fb.success = false;
                    fb.compute_latency_ms = timestamp_ms() - t_start;
                    if (feedback_callback) feedback_callback(fb);
                    return;
                }
                backend = it->second.backend_ptr;
            }

            if (!backend)
            {
                fb.success = false;
                fb.compute_latency_ms = timestamp_ms() - t_start;
                if (feedback_callback) feedback_callback(fb);
                return;
            }

            if (cmd.type == ControlCommand::Type::QuantumCompute && cmd.gate_sequence)
            {
                cmd.gate_sequence(backend);
            }

            for (size_t qid : cmd.qubit_targets)
            {
                fb.measurement_results.push_back(backend->measure(qid));
            }

            fb.success = true;
            fb.compute_latency_ms = timestamp_ms() - t_start;
            if (feedback_callback) feedback_callback(fb);
        }

        void control_loop()
        {
            std::cout << "[Rmx] Real-time control loop thread entered.\n";
            while (loop_running.load())
            {
                ControlCommand cmd;
                {
                    std::unique_lock<std::mutex> lock(queue_mutex);
                    queue_cv.wait(lock, [this] {
                        return !command_queue.empty() || !loop_running.load();
                    });
                    if (!loop_running.load() && command_queue.empty())
                        break;
                    if (command_queue.empty())
                        continue;
                    cmd = command_queue.front();
                    command_queue.pop();
                }
                execute_command(cmd);
            }
            std::cout << "[Rmx] Real-time control loop thread exited.\n";
        }

    public:
        Rmx()
        {
            std::cout << "[Rmx] Distributed hybrid network fabric initialized.\n";
        }

        ~Rmx()
        {
            stop_realtime_loop();
        }

        // ─── 节点注册 ─────────────────────────────────────────────────

        size_t register_brain_node(const BrainNodeInfo &info)
        {
            std::lock_guard<std::mutex> lock(topology_mutex);
            BrainNodeInfo node = info;
            node.node_id = next_node_id++;
            brain_nodes[node.node_id] = node;
            std::cout << "[Rmx] Registered brain node " << node.node_id
                      << " (subject: " << node.subject_label
                      << ", modality: " << static_cast<int>(node.modality) << ").\n";
            return node.node_id;
        }

        size_t register_qc_node(const QCNodeInfo &info)
        {
            std::lock_guard<std::mutex> lock(topology_mutex);
            QCNodeInfo node = info;
            node.node_id = next_node_id++;
            qc_nodes[node.node_id] = node;
            std::cout << "[Rmx] Registered QC node " << node.node_id
                      << " (type: " << (node.type == QCNodeType::QM ? "QM" : "QVM")
                      << ", qubits: " << node.available_qubits << ").\n";
            return node.node_id;
        }

        void unregister_node(size_t node_id)
        {
            std::lock_guard<std::mutex> lock(topology_mutex);
            brain_nodes.erase(node_id);
            qc_nodes.erase(node_id);

            for (auto it = link_latency_table.begin(); it != link_latency_table.end();)
            {
                if (it->first.first == node_id || it->first.second == node_id)
                    it = link_latency_table.erase(it);
                else
                    ++it;
            }
            std::cout << "[Rmx] Unregistered node " << node_id << ".\n";
        }

        // ─── 网络拓扑 ──────────────────────────────────────────────────

        void establish_link(size_t node_a, size_t node_b, double measured_latency_ms)
        {
            std::lock_guard<std::mutex> lock(topology_mutex);

            auto qa = qc_nodes.find(node_a);
            auto qb = qc_nodes.find(node_b);

            if (qa != qc_nodes.end() && qb != qc_nodes.end())
            {
                // 两者都是量子计算节点——利用Karma量子网络原语
                if (qa->second.type == QCNodeType::QM && qb->second.type == QCNodeType::QM)
                {
                    auto *qm_a = dynamic_cast<qhal::QM *>(qa->second.backend_ptr);
                    if (qm_a)
                        qm_a->establish_remote_entanglement(node_b, measured_latency_ms);
                }
                else
                {
                    // 至少有一个 QVM — 使用 QVM 分布式链接
                    auto *qvm = dynamic_cast<qhal::QVM *>(
                        (qa->second.type == QCNodeType::QVM) ? qa->second.backend_ptr
                                                             : qb->second.backend_ptr);
                    if (qvm)
                        qvm->establish_network_link(node_b, measured_latency_ms);
                }
            }

            size_t lo = (node_a < node_b) ? node_a : node_b;
            size_t hi = (node_a < node_b) ? node_b : node_a;
            auto key = std::make_pair(lo, hi);
            link_latency_table[key] = measured_latency_ms;
            std::cout << "[Rmx] Link established: " << node_a << " <-> " << node_b
                      << " (latency: " << measured_latency_ms << " ms).\n";
        }

        double get_link_latency(size_t node_a, size_t node_b) const
        {
            std::lock_guard<std::mutex> lock(topology_mutex);
            size_t lo = (node_a < node_b) ? node_a : node_b;
            size_t hi = (node_a < node_b) ? node_b : node_a;
            auto key = std::make_pair(lo, hi);
            auto it = link_latency_table.find(key);
            return (it != link_latency_table.end()) ? it->second : -1.0;
        }

        // ─── 计算调度 ──────────────────────────────────────────────────

        QuantumFeedback dispatch_to_qc(size_t qc_node_id,
                                       std::shared_ptr<rvdl::QDataState> encoded_data)
        {
            double t_start = timestamp_ms();
            QuantumFeedback fb;
            fb.source_node_id = qc_node_id;

            qhal::IQuantumBackend *backend = nullptr;
            {
                std::lock_guard<std::mutex> lock(topology_mutex);
                auto it = qc_nodes.find(qc_node_id);
                if (it == qc_nodes.end() || !it->second.is_active || !it->second.backend_ptr)
                {
                    fb.success = false;
                    fb.compute_latency_ms = timestamp_ms() - t_start;
                    std::cerr << "[Rmx] dispatch_to_qc: QC node " << qc_node_id
                              << " unavailable.\n";
                    return fb;
                }
                backend = it->second.backend_ptr;
            }

            if (encoded_data)
            {
                const auto &ids = encoded_data->get_ids();
                for (size_t id : ids)
                {
                    fb.measurement_results.push_back(backend->measure(id));
                }
            }

            fb.success = true;
            fb.compute_latency_ms = timestamp_ms() - t_start;
            return fb;
        }

        std::vector<QuantumFeedback> distributed_compute(
            const std::vector<size_t> &qc_node_ids,
            std::shared_ptr<rvdl::QDataState> encoded_data)
        {
            std::vector<QuantumFeedback> results;
            results.reserve(qc_node_ids.size());

            // 建立所有QC节点两两之间的连接
            for (size_t i = 0; i < qc_node_ids.size(); ++i)
            {
                for (size_t j = i + 1; j < qc_node_ids.size(); ++j)
                {
                    if (get_link_latency(qc_node_ids[i], qc_node_ids[j]) < 0.0)
                        establish_link(qc_node_ids[i], qc_node_ids[j], 1.0);
                }
            }

            // 分发到每个节点
            for (size_t node_id : qc_node_ids)
            {
                results.push_back(dispatch_to_qc(node_id, encoded_data));
            }

            std::cout << "[Rmx] distributed_compute: dispatched to " << qc_node_ids.size()
                      << " QC nodes.\n";
            return results;
        }

        // ─── 实时控制回路 ────────────────────────────────────────────

        void start_realtime_loop(std::function<void(const QuantumFeedback &)> callback)
        {
            if (loop_running.load())
            {
                std::cout << "[Rmx] Real-time loop already running.\n";
                return;
            }
            feedback_callback = std::move(callback);
            loop_running.store(true);
            control_thread = std::thread(&Rmx::control_loop, this);
            std::cout << "[Rmx] Real-time control loop started.\n";
        }

        void stop_realtime_loop()
        {
            if (!loop_running.load())
                return;
            loop_running.store(false);
            queue_cv.notify_all();
            if (control_thread.joinable())
                control_thread.join();
            std::cout << "[Rmx] Real-time control loop stopped.\n";
        }

        void submit_command(const ControlCommand &cmd)
        {
            {
                std::lock_guard<std::mutex> lock(queue_mutex);
                command_queue.push(cmd);
            }
            queue_cv.notify_one();
        }

        // ─── QM / QVM 实时直接控制 ──────────────────────────────────

        void realtime_qm_control(qhal::QM *qm, size_t remote_node, double latency_ms)
        {
            if (!qm)
                throw std::runtime_error("[Rmx] realtime_qm_control: null QM pointer.");

            std::cout << "[Rmx] Real-time QM control: establishing entanglement with node "
                      << remote_node << ".\n";
            qm->establish_remote_entanglement(remote_node, latency_ms);
        }

        void realtime_qvm_control(qhal::QVM *qvm, size_t remote_node,
                                  size_t target_qubit, double latency_ms)
        {
            if (!qvm)
                throw std::runtime_error("[Rmx] realtime_qvm_control: null QVM pointer.");

            std::cout << "[Rmx] Real-time QVM control: distributed gate to node "
                      << remote_node << ", qubit " << target_qubit << ".\n";
            qvm->establish_network_link(remote_node, latency_ms);
            qvm->execute_distributed_gate(0, remote_node, target_qubit, latency_ms);
        }

        // ─── 拓扑检测 ───────────────────────────────────────────────

        size_t brain_node_count() const
        {
            std::lock_guard<std::mutex> lock(topology_mutex);
            return brain_nodes.size();
        }

        size_t qc_node_count() const
        {
            std::lock_guard<std::mutex> lock(topology_mutex);
            return qc_nodes.size();
        }

        std::vector<size_t> list_qc_nodes() const
        {
            std::lock_guard<std::mutex> lock(topology_mutex);
            std::vector<size_t> ids;
            ids.reserve(qc_nodes.size());
            for (const auto &p : qc_nodes)
                ids.push_back(p.first);
            return ids;
        }

        std::vector<size_t> list_brain_nodes() const
        {
            std::lock_guard<std::mutex> lock(topology_mutex);
            std::vector<size_t> ids;
            ids.reserve(brain_nodes.size());
            for (const auto &p : brain_nodes)
                ids.push_back(p.first);
            return ids;
        }
    };
}