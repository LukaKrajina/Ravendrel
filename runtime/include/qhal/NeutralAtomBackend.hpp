#pragma once
#include "IQuantumBackend.hpp"
#include "IdealStateCore.hpp"
#include "../utils/FastPhaseRetrieval.hpp"
#include <iostream>
#include <vector>
#include <mutex>
#include <cmath>
#include <unordered_map>
#include <random>
#include "math.h"
#include "../stub/nabStub.hpp"

namespace qhal
{

    struct SpatialCoordinate
    {
        double x;
        double y;
    };

    struct OpticalSwitch
    {
        bool chamber_routed = false;
        void route_to_chamber()
        {
            chamber_routed = true;
        }
    };

    struct Laser
    {
        double tx = 0.0, ty = 0.0;
        double last_duration = 0.0;
        double last_intensity = 0.0;
        size_t pulse_count = 0;

        void pulse(double duration, double intensity)
        {
            last_duration = duration;
            last_intensity = intensity;
            ++pulse_count;
        }
        void target(double x, double y)
        {
            tx = x;
            ty = y;
        }
    };

    struct Camera
    {
        size_t frames_captured = 0;
        void expose() { ++frames_captured; }
    };

    struct ImagePipeline
    {
        std::unordered_map<size_t, int> brightness;

        void set_brightness(size_t qubit_id, int b) { brightness[qubit_id] = b; }
        int get_brightness_at(size_t qubit_id) const
        {
            auto it = brightness.find(qubit_id);
            return it == brightness.end() ? 0 : it->second;
        }
    };

    struct AODDriver
    {
    private:
        const double AOD_CENTER_FREQ_MHZ = 100.0;
        const double AOD_BANDWIDTH = 50.0;
        const double SPATIAL_TO_FREQ_FACTOR = 1.5;
        const double MAX_RF_AMPLITUDE_VPP = 2.0;
        double last_freq_x = 0.0, last_freq_y = 0.0;
        double last_amp_x = 0.0, last_amp_y = 0.0;

        void dispatch_to_awg(double freq_x, double freq_y, double amp_x, double amp_y)
        {
            const double DDS_SYS_CLK_MHZ = 1000.0;
            uint32_t ftw_x = static_cast<uint32_t>((freq_x / DDS_SYS_CLK_MHZ) * std::pow(2.0, 32.0));
            uint32_t ftw_y = static_cast<uint32_t>((freq_y / DDS_SYS_CLK_MHZ) * std::pow(2.0, 32.0));
            last_freq_x = freq_x;
            last_freq_y = freq_y;
            last_amp_x = amp_x;
            last_amp_y = amp_y;
            std::cout << "[QHAL.AOD] DDS tune (ftw_x=" << ftw_x << ", ftw_y=" << ftw_y
                      << ", amp_x=" << amp_x << ", amp_y=" << amp_y << ").\n";
        }

    public:
        void set_local_intensity(double x, double y, double local_intensity)
        {
            double freq_x = AOD_CENTER_FREQ_MHZ + x * SPATIAL_TO_FREQ_FACTOR;
            double freq_y = AOD_CENTER_FREQ_MHZ + y * SPATIAL_TO_FREQ_FACTOR;
            double amp_x = std::min(local_intensity, MAX_RF_AMPLITUDE_VPP);
            double amp_y = std::min(local_intensity, MAX_RF_AMPLITUDE_VPP);
            dispatch_to_awg(freq_x, freq_y, amp_x, amp_y);
        }
    };

    class NeutralAtomBackend : public IQuantumBackend, public Stub::Dispatch
    {
    private:
        std::mutex slm_mutex;
        std::vector<SpatialCoordinate> atom_array;
        std::vector<bool> is_qubit_allocated;
        std::vector<bool> is_qubit_locked;
        IdealStateCore ideal_;  // 理想态矢量参考（Born 规则坍缩语义）
        const size_t SLM_WIDTH = 1024;
        const size_t SLM_HEIGHT = 1024;

        OpticalSwitch optical_switch;
        Laser rydberg_laser;
        Laser raman_laser;
        Laser stark_laser;
        Camera fluorescence_camera;
        ImagePipeline image_processing_pipeline;
        AODDriver tweezer_aod_driver;

        const double duration = 1.0;
        const double intensity = 1.0;
        const int CALIBRATED_THRESHOLD = 50;

        void dispatch_buffer_to_slm_hardware(const std::vector<std::vector<double>> &phase_mask) override
        {
            if (phase_mask.empty() || phase_mask[0].empty())
                return;
            size_t height = phase_mask.size();
            size_t width = phase_mask[0].size();
            std::vector<uint8_t> slm_byte_buffer;
            slm_byte_buffer.reserve(width * height);

            for (size_t y = 0; y < height; ++y)
            {
                for (size_t x = 0; x < width; ++x)
                {
                    double raw_phase = std::fmod(phase_mask[y][x], 2 * M_PI);
                    if (raw_phase < 0)
                        raw_phase += 2 * M_PI;
                    uint8_t pixel_value = static_cast<uint8_t>((raw_phase) / (2 * M_PI) * 255);
                    slm_byte_buffer.push_back(pixel_value);
                }
            }
        }

        void update_tweezer_slm()
        {
            std::vector<std::vector<double>> target_amplitude(SLM_HEIGHT, std::vector<double>(SLM_WIDTH, 0.0));

            for (const auto &atom : atom_array)
            {
                size_t px = static_cast<size_t>(std::abs(std::round(atom.x))) % SLM_WIDTH;
                size_t py = static_cast<size_t>(std::abs(std::round(atom.y))) % SLM_HEIGHT;
                if (px < SLM_WIDTH && py < SLM_HEIGHT)
                    target_amplitude[py][px] = 1.0;
            }

            FastCPUPhaseRetrieval phase_retrieval(SLM_WIDTH, SLM_HEIGHT);
            std::vector<std::vector<double>> slm_phase_mask = phase_retrieval.compute_slm_phase_mask(target_amplitude);
            dispatch_buffer_to_slm_hardware(slm_phase_mask);
        }

        void fire_rydberg_laser(const std::vector<size_t> &targets)
        {
            // 只对 targets 指定的原子打 Rydberg 激光：经声光偏转器把激光定位
            // 到每个目标原子上再发脉冲，而不是无差别全局脉冲。
            if (targets.empty())
                return;
            optical_switch.route_to_chamber();
            for (size_t t : targets)
            {
                if (t >= atom_array.size())
                    continue;
                rydberg_laser.target(atom_array[t].x, atom_array[t].y);
                rydberg_laser.pulse(duration, intensity);
            }
        }

        // ─── 脉冲层：仅产生物理副作用（激光 / 光镊），不更新理想态。
        //     供 apply_cnot/apply_toffoli 等复合门在持锁时调用，避免嵌套加锁死锁。
        void pulse_x(size_t qubit_id)
        {
            raman_laser.target(atom_array[qubit_id].x, atom_array[qubit_id].y);
            raman_laser.pulse(duration, intensity);
            std::cout << "[QHAL] X Gate applied -> Raman Pi-pulse delivered to Qubit "
                      << qubit_id << " at (" << atom_array[qubit_id].x << ", "
                      << atom_array[qubit_id].y << ").\n";
        }

        void pulse_rz(size_t qubit_id, double angle)
        {
            stark_laser.target(atom_array[qubit_id].x, atom_array[qubit_id].y);
            double phase_duration = (std::abs(angle) / M_PI) * duration;
            stark_laser.pulse(phase_duration, intensity);
            std::cout << "[QHAL] Rz(" << angle << ") applied to Qubit " << qubit_id << ".\n";
        }

        // ─── 硬件噪声注入（测量前）───────────
        // 里德堡态有限寿命 → 振幅阻尼（弛豫到基态）。
        void inject_measurement_noise(size_t qubit_id)
        {
            ideal_.apply_amplitude_damping(qubit_id, 0.01);
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
        void allocate_qubits(size_t num_qubits) override
        {
            std::lock_guard<std::mutex> lock(slm_mutex);
            atom_array.resize(num_qubits);
            is_qubit_allocated.resize(num_qubits, true);
            is_qubit_locked.resize(num_qubits, false);
            for (size_t i = 0; i < num_qubits; ++i)
                atom_array[i] = {static_cast<double>(i % 10) * 5.0, static_cast<double>(i / 10) * 5.0};
            update_tweezer_slm();
            ideal_.allocate(num_qubits);
        }

        void increase_tweezer_depth(size_t qubit_id) override
        {
            if (qubit_id >= atom_array.size())
                return;
            double target_x = atom_array[qubit_id].x;
            double target_y = atom_array[qubit_id].y;
            const double READOUT_DEPTH_MULTIPLIER = 5.0;
            tweezer_aod_driver.set_local_intensity(target_x, target_y, intensity * READOUT_DEPTH_MULTIPLIER);
        }

        void apply_h(size_t qubit_id) override
        {
            std::lock_guard<std::mutex> lock(slm_mutex);
            ideal_.apply_h(qubit_id);
        }

        int measure(size_t qubit_id) override
        {
            std::lock_guard<std::mutex> lock(slm_mutex);
            increase_tweezer_depth(qubit_id);
            fluorescence_camera.expose();
            inject_measurement_noise(qubit_id);
            return ideal_.measure(qubit_id);
        }

        void apply_x(size_t qubit_id) override
        {
            std::lock_guard<std::mutex> lock(slm_mutex);
            pulse_x(qubit_id);
            ideal_.apply_x(qubit_id);
        }

        void apply_rz(size_t qubit_id, double angle) override
        {
            std::lock_guard<std::mutex> lock(slm_mutex);
            pulse_rz(qubit_id, angle);
            ideal_.apply_rz(qubit_id, angle);
        }

        void apply_cnot(size_t control, size_t target) override
        {
            std::lock_guard<std::mutex> lock(slm_mutex);
            atom_array[target].x = atom_array[control].x + 4.0;
            atom_array[target].y = atom_array[control].y;
            update_tweezer_slm();
            pulse_rz(target, M_PI / 2);
            fire_rydberg_laser({control, target});
            pulse_rz(target, -M_PI / 2);
            ideal_.apply_cnot(control, target);
        }

        void apply_toffoli(size_t control1, size_t control2, size_t target) override
        {
            std::lock_guard<std::mutex> lock(slm_mutex);
            atom_array[control2].x = atom_array[control1].x + 3.0;
            atom_array[target].x = atom_array[control1].x + 6.0;
            update_tweezer_slm();
            pulse_rz(target, M_PI / 2);
            fire_rydberg_laser({control1, control2, target});
            pulse_rz(target, -M_PI / 2);
            ideal_.apply_toffoli(control1, control2, target);
        }

        void release_qubit(size_t qubit_id) override {
            if (qubit_id < is_qubit_allocated.size()) is_qubit_allocated[qubit_id] = false;
        }

        void lock_hardware_id(size_t qubit_id) override {
            if (qubit_id < is_qubit_locked.size()) is_qubit_locked[qubit_id] = true;
        }

        void unlock_hardware_id(size_t qubit_id) override {
            if (qubit_id < is_qubit_locked.size()) is_qubit_locked[qubit_id] = false;
        }
    };
}