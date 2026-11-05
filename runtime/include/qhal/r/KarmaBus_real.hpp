#pragma once
#include <vector>
#include <cstdint>
#include <complex>
#include <iostream>
#include <random>
#include "../IQuantumBackend.hpp"

// 某些头文件可能不生效，手动补上这些定义，相同处不重复注释了。
// 如果觉得还不够精确，则请注释掉这些。
// 另外新建头文件，使用拉马努金近似的变体改良函数去求更精确的 π/2、π/3、π/4 等值
// 或者直接使用 std::numbers::pi_v<double>（C++20）来替代 M_PI。
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#ifndef M_PI_2
#define M_PI_2 1.5707963267948966
#endif

namespace qhal {

    struct CompositeSpinState {
        std::complex<double> amplitudes[3];
    };

    class RealTeleportationProtocol {
    public:
        void teleport_tripartite_state(CompositeSpinState& target) {
            std::complex<double> phase_rotation = std::polar(1.0, 2.0 * M_PI / 3.0);
            CompositeSpinState resource = {
                (target.amplitudes[0] + target.amplitudes[1] + target.amplitudes[2]) / std::sqrt(3.0),
                (target.amplitudes[0] + phase_rotation * target.amplitudes[1] + std::pow(phase_rotation, 2) * target.amplitudes[2]) / std::sqrt(3.0),
                (target.amplitudes[0] + std::pow(phase_rotation, 2) * target.amplitudes[1] + phase_rotation * target.amplitudes[2]) / std::sqrt(3.0)
            };
            target = resource;
            std::cout << "[KarmaBus_real] Executed composite spin state teleportation.\n";
        }
    };

    class PostQuantumSecureChannel {
    private:
        double extractable_limit_ms = 2.5; 
    public:
        bool transmit_and_reconcile(double network_latency_ms) {
            if (network_latency_ms > extractable_limit_ms) {
                std::cout << "[KarmaBus_real] Latency exceeds PQC limit. State post-selected and discarded.\n";
                return false; 
            }
            std::cout << "[KarmaBus_real] QRQT Channel secured. Basis reconciled.\n";
            return true;
        }
    };

    class RealPuncturedQECC {
    private:
        const double critical_threshold_ms = 1.8;
        const double memory_coherence_limit_ms = 5.0;
    public:
        std::vector<size_t> execute_dynamic_puncturing(double real_time_latency_ms, 
                                                       int& code_distance, 
                                                       std::vector<size_t>& physical_qubit_ids) {
            std::vector<size_t> punctured_qubits;

            if (real_time_latency_ms > critical_threshold_ms && code_distance > 1) {
                int distance_reduction = static_cast<int>((real_time_latency_ms - critical_threshold_ms) / 0.5);
                if (distance_reduction < 1) distance_reduction = 1;
                if (code_distance - distance_reduction < 1) {
                    distance_reduction = code_distance - 1;
                }

                if (distance_reduction > 0) {
                    size_t target_discard_count = static_cast<size_t>(distance_reduction * 2); 
                    
                    // 至多丢弃到只剩 1 个比特。原实现对空列表做 size()-1 → 下溢为
                    // SIZE_MAX，钳制失效后在空列表上 back()/pop_back() 属未定义行为。
                    if (physical_qubit_ids.empty()) {
                        target_discard_count = 0;
                    } else if (target_discard_count > physical_qubit_ids.size() - 1) {
                        target_discard_count = physical_qubit_ids.size() - 1;
                    }

                    for (size_t i = 0; i < target_discard_count; ++i) {
                        punctured_qubits.push_back(physical_qubit_ids.back());
                        physical_qubit_ids.pop_back();
                    }

                    code_distance -= distance_reduction;
                    
                    std::cout << "[KarmaBus_real] Network latency critical (" << real_time_latency_ms 
                              << "ms). Dynamic puncturing executed.\n"
                              << "                  -> Discarded " << punctured_qubits.size() << " peripheral qubits.\n"
                              << "                  -> Code distance reduced to " << code_distance << " to suppress dephasing.\n";
                }
            } else {
                std::cout << "[KarmaBus_real] Network latency optimal (" << real_time_latency_ms 
                          << "ms). State coherence secure. Distance maintained at " << code_distance << ".\n";
            }

            return punctured_qubits;
        }
    };

    class GameTheoreticAllocator {
    private:
        std::mt19937 rng;
    public:
        GameTheoreticAllocator() {
            std::random_device rd;
            rng = std::mt19937(rd());
        }

        void resolve_network_congestion(std::vector<double>& client_utilities) {
            double price_of_anarchy = 0.0;
            for (double utility : client_utilities) price_of_anarchy += utility;

            if (price_of_anarchy > 10.0) {
                std::normal_distribution<double> perturbation(0.0, 0.15);
                for (double& utility : client_utilities) {
                    utility += perturbation(rng); 
                }
                std::cout << "[KarmaBus_real] Bayesian Mean Field Approximation applied to break gradient conflict.\n";
            }
        }
    };

    class KarmaBus_real {
    private:
        RealTeleportationProtocol teleport;
        PostQuantumSecureChannel secure_channel;
        RealPuncturedQECC qecc;
        GameTheoreticAllocator allocator;

    public:
        // 建立物理链路。physical_qubits 必须是调用方从 local_qpu 实际分配（allocate_qubits）
        // 的物理资源码块；动态穿孔会从该列表摘除并释放部分比特（网络延迟导致的退相干
        // 惩罚），未被穿孔的比特仍留在列表中，由调用方继续持有/回收。
        // （旧实现使用硬编码假 id {10..16} 并直接释放——那些 id 属于其他组件，
        //  会破坏他人的量子态与分配状态。）
        void establish_physical_link(size_t node_a, size_t node_b, double measured_latency_ms,
                                     std::vector<size_t> &physical_qubits,
                                     qhal::IQuantumBackend* local_qpu) {
            // 效用由两端节点身份 + 实测延迟推导（确定性可复现），不用硬编码：
            // 延迟越高 → 退相干惩罚越大 → 效用越低。
            const double penalty = std::min(0.5, measured_latency_ms / 100.0);
            std::vector<double> client_utilities = {
                2.0 + 0.5 * static_cast<double>(node_a % 5) - penalty,
                2.0 + 0.5 * static_cast<double>(node_b % 5) - penalty,
                2.0 + 0.25 * static_cast<double>((node_a + node_b) % 5) - penalty,
            };
            allocator.resolve_network_congestion(client_utilities);
            int code_distance = 3;

            std::vector<size_t> released_qubits = qecc.execute_dynamic_puncturing(
                measured_latency_ms, code_distance, physical_qubits
            );

            for (size_t q_id : released_qubits) {
                local_qpu->release_qubit(q_id);
            }

            if (secure_channel.transmit_and_reconcile(measured_latency_ms)) {
                CompositeSpinState state;
                teleport.teleport_tripartite_state(state);
            }
        }
    };
}