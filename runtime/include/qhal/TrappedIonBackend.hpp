#pragma once
#include "IQuantumBackend.hpp"
#include "IdealStateCore.hpp"
#include <cstddef>
#include <iostream>
#include <vector>
#include <mutex>
#include <stdexcept>
#include <cmath>
#include <algorithm>
#include <queue>
#include <unordered_map>
#include <random>

namespace qhal
{

    struct LaserPulse
    {
        double frequency_mhz;
        double duration_us;
        double intensity;
    };

    struct PoissonStateDiscriminator
    {
    private:
        double mean_dark_counts;
        double mean_bright_counts;
        int optimal_threshold;
        void recalculate_threshold()
        {
            double numerator = mean_bright_counts - mean_dark_counts;
            double denominator = std::log(mean_bright_counts) - std::log(mean_dark_counts);
            optimal_threshold = static_cast<int>(std::round(numerator / denominator));
        }

    public:
        // 用典型的镱（Yb+）或钡（Ba+）本底计数初始化
        PoissonStateDiscriminator(double dark = 2.5, double bright = 35.0)
            : mean_dark_counts(dark), mean_bright_counts(bright)
        {
            recalculate_threshold();
        }

        int discriminate(int photon_count) const
        {
            return (photon_count >= optimal_threshold) ? 1 : 0;
        }

        // 实际系统通过后台校准流程动态更新此值，以补偿激光功率漂移
        void calibrate(double new_dark, double new_bright)
        {
            mean_dark_counts = new_dark;
            mean_bright_counts = new_bright;
            recalculate_threshold();
        }
    };

    struct DDSController
    {
        double frequency_mhz = 0.0;
        double amplitude = 0.0;
        double last_duration_us = 0.0;
        size_t pulse_count = 0;

        void set_frequency(double freq) { frequency_mhz = freq; }
        void set_amplitude(double amp) { amplitude = amp; }
        void execute_pulse(double duration) { last_duration_us = duration; ++pulse_count; }
    };

    struct EMCCDCamera
    {
        std::mt19937 rng{std::random_device{}()};
        // 荧光读出：亮态光子计数高，暗态接近暗计数。
        int get_pixel_count_for_ion(size_t node_id)
        {
            (void)node_id;
            std::poisson_distribution<int> dist(20.0);
            return dist(rng);
        }
    };

    struct DACController
    {
        std::unordered_map<size_t, double> electrodes;
        bool waveform_dirty = false;

        void update_electrode_voltage(size_t electrode_id, double voltage_v)
        {
            electrodes[electrode_id] = voltage_v;
            waveform_dirty = true;
        }
        void trigger_waveform_update()
        {
            waveform_dirty = false;
        }
    };

    struct QCCDNode
    {
        size_t id;
        bool is_processing_zone;
        std::vector<size_t> adjacent_nodes;
    };

    class TrappedIonBackend : public IQuantumBackend
    {
    private:
        std::mutex trap_mutex;
        size_t active_ions = 0;
        std::vector<bool> is_qubit_allocated;
        std::vector<bool> is_qubit_locked;

        DDSController dds_controller;
        DACController dac_controller;
        EMCCDCamera emccd_camera;
        PoissonStateDiscriminator psd;
        IdealStateCore ideal_;  // 理想态矢量参考（Born 规则坍缩语义）

        std::unordered_map<size_t, QCCDNode> trap_topology;
        std::vector<size_t> ion_positions;

        void fire_aom(const LaserPulse &pulse)
        {
            dds_controller.set_frequency(pulse.frequency_mhz);
            dds_controller.set_amplitude(pulse.intensity);
            dds_controller.execute_pulse(pulse.duration_us);
        }

        void initialize_qccd_topology()
        {
            for (size_t i = 0; i < 25; ++i)
            {
                QCCDNode node;
                node.id = i;
                node.is_processing_zone = (i % 2 == 0);
                if (i > 4)
                    node.adjacent_nodes.push_back(i - 5);
                if (i < 20)
                    node.adjacent_nodes.push_back(i + 5);
                if (i % 5 != 0)
                    node.adjacent_nodes.push_back(i - 1);
                if (i % 5 != 4)
                    node.adjacent_nodes.push_back(i + 1);
                trap_topology[i] = node;
            }
        }

        std::vector<size_t> find_shortest_path(size_t start_node, size_t target_node)
        {
            std::queue<size_t> queue;
            std::unordered_map<size_t, size_t> came_from;
            bool found = false;

            queue.push(start_node);
            came_from[start_node] = start_node;

            while (!queue.empty())
            {
                size_t current = queue.front();
                queue.pop();

                if (current == target_node)
                {
                    found = true;
                    break;
                }

                for (size_t neighbor : trap_topology[current].adjacent_nodes)
                {
                    if (came_from.find(neighbor) == came_from.end())
                    {
                        came_from[neighbor] = current;
                        queue.push(neighbor);
                    }
                }
            }

            if (!found)
            {
                throw std::runtime_error("[QHAL] Topological routing failed. Disconnected trap zones");
            }

            std::vector<size_t> path;
            size_t curr = target_node;
            while (curr != start_node)
            {
                path.push_back(curr);
                curr = came_from[curr];
            }
            path.push_back(start_node);
            std::reverse(path.begin(), path.end());
            return path;
        }

        void dispatch_dc_waveforms_to_electrodes(const std::vector<size_t> &path)
        {
            for (size_t node_id : path)
            {
                double voltage = trap_topology[node_id].is_processing_zone ? 15.0 : 0.0;
                dac_controller.update_electrode_voltage(node_id, voltage);
            }
            dac_controller.trigger_waveform_update();
        }

        void shuttle_ions(size_t ion1, size_t ion2)
        {
            if (ion_positions[ion1] == ion_positions[ion2])
                return;

            std::vector<size_t> path = find_shortest_path(ion_positions[ion1], ion_positions[ion2]);

            ion_positions[ion1] = ion_positions[ion2];

            dispatch_dc_waveforms_to_electrodes(path);
        }

        // 集体纠缠门的基础
        void apply_molmer_sorensen(size_t ion1, size_t ion2)
        {
            shuttle_ions(ion1, ion2);

            // 双色激光脉冲激发集体声子运动模式
            LaserPulse ms_pulse = {400.0, 50.0, 0.8};
            fire_aom(ms_pulse);
        }

        // ─── 脉冲层：仅产生物理副作用（激光 / 电极波形），不更新理想态。
        //     供 apply_cnot/apply_toffoli 等复合门在持锁时调用，避免嵌套加锁死锁。
        void pulse_x(size_t qubit_id)
        {
            // 用 qubit_id 定位离子：拉曼 π 脉冲频率按该离子所在 QCCD 节点的
            // 跃迁偏移设置，并激活对应节点电极，而不是忽略 id 打全局脉冲。
            if (qubit_id >= ion_positions.size())
                return;
            const size_t node = ion_positions[qubit_id];
            LaserPulse raman_pulse = {200.0 + 5.0 * static_cast<double>(node), 10.0, 1.0};
            dac_controller.update_electrode_voltage(node, 12.0);
            fire_aom(raman_pulse);
        }

        void pulse_h(size_t qubit_id)
        {
            if (qubit_id >= ion_positions.size())
                return;
            const size_t node = ion_positions[qubit_id];
            LaserPulse hadamard_pulse = {200.0 + 5.0 * static_cast<double>(node), 10.0, 0.5};
            dac_controller.update_electrode_voltage(node, 12.0);
            fire_aom(hadamard_pulse);
        }

        void pulse_rz(size_t qubit_id, double angle)
        {
            if (qubit_id >= ion_positions.size())
                return;
            const size_t node = ion_positions[qubit_id];
            // Stark 频移 Z 旋转：幅度与旋转角成正比，频率按节点偏移。
            LaserPulse stark_pulse = {200.0 + 5.0 * static_cast<double>(node), 5.0, std::abs(angle)};
            dac_controller.update_electrode_voltage(node, 12.0);
            fire_aom(stark_pulse);
        }

        // ─── 硬件噪声注入（测量前）───────────
        // 离子阱加热 / 集体声子模去相干 → 去极化。
        void inject_measurement_noise(size_t qubit_id)
        {
            ideal_.apply_depolarizing(qubit_id, 0.01);
        }

        // ─── @[noise]/@[coherence] 噪声通道分发（复用 IdealStateCore 的通道）───
        void apply_noise(size_t qubit_id, int channel, double param) override
        {
            ideal_.apply_noise_channel(qubit_id, channel, param);
        }

        // 非破坏 Z 期望（不坍缩态矢量）
        double expectation_z(size_t qubit_id) override
        {
            return ideal_.expectation_z(qubit_id);
        }

    public:
        TrappedIonBackend()
        {
            initialize_qccd_topology();
        }

        void allocate_qubits(size_t num_qubits) override
        {
            std::lock_guard<std::mutex> lock(trap_mutex);
            active_ions = num_qubits;
            ion_positions.resize(num_qubits, 0);
            is_qubit_allocated.resize(num_qubits, true);
            is_qubit_locked.resize(num_qubits, false);
            ideal_.allocate(num_qubits);
        }

        int measure(size_t qubit_id) override
        {
            std::lock_guard<std::mutex> lock(trap_mutex);

            // 触发探测激光（保留硬件副作用）。
            LaserPulse readout_laser = {369.5, 100.0, 1.0};
            fire_aom(readout_laser);

            inject_measurement_noise(qubit_id);
            return ideal_.measure(qubit_id);
        }

        void apply_x(size_t qubit_id) override
        {
            std::lock_guard<std::mutex> lock(trap_mutex);
            pulse_x(qubit_id);
            ideal_.apply_x(qubit_id);
        }

        void apply_h(size_t qubit_id) override
        {
            std::lock_guard<std::mutex> lock(trap_mutex);
            pulse_h(qubit_id);
            ideal_.apply_h(qubit_id);
        }

        void apply_rz(size_t qubit_id, double angle) override
        {
            std::lock_guard<std::mutex> lock(trap_mutex);
            pulse_rz(qubit_id, angle);
            ideal_.apply_rz(qubit_id, angle);
        }

        void apply_cnot(size_t control, size_t target) override
        {
            std::lock_guard<std::mutex> lock(trap_mutex);
            // 局部旋转把 MS 门映射为标准 CNOT（仅硬件副作用）。
            pulse_rz(control, M_PI / 2);
            pulse_x(target);
            apply_molmer_sorensen(control, target);
            pulse_x(target);
            ideal_.apply_cnot(control, target);
        }

        void apply_toffoli(size_t control1, size_t control2, size_t target) override
        {
            std::lock_guard<std::mutex> lock(trap_mutex);
            // 用 Mølmer-Sørensen 门做的 Toffoli 分解（仅硬件副作用）。
            shuttle_ions(control1, control2);
            shuttle_ions(control2, target);

            apply_molmer_sorensen(control2, target);
            pulse_rz(target, M_PI / 4);
            apply_molmer_sorensen(control1, target);
            pulse_rz(target, -M_PI / 4);
            apply_molmer_sorensen(control2, target);
            pulse_rz(target, M_PI / 4);
            apply_molmer_sorensen(control1, target);

            pulse_rz(control2, M_PI / 4);
            apply_molmer_sorensen(control1, control2);
            pulse_rz(control1, M_PI / 4);
            pulse_rz(control2, -M_PI / 4);
            apply_molmer_sorensen(control1, control2);

            ideal_.apply_toffoli(control1, control2, target);
        }

        void release_qubit(size_t qubit_id) override
        {
            if (qubit_id < is_qubit_allocated.size())
                is_qubit_allocated[qubit_id] = false;
        }

        void lock_hardware_id(size_t qubit_id) override
        {
            if (qubit_id < is_qubit_locked.size())
                is_qubit_locked[qubit_id] = true;
        }

        void unlock_hardware_id(size_t qubit_id) override
        {
            if (qubit_id < is_qubit_locked.size())
                is_qubit_locked[qubit_id] = false;
        }
    };
}