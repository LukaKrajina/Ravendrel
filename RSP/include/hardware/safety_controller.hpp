#pragma once
#include <vector>
#include <memory>
#include <utility>
#include <cmath>
#include <iostream>
#include "observability.hpp"
#include "bio_signal.hpp"
#include "actuator.hpp"
#include "control/prosthetic.hpp"

namespace rvdlrsp::hardware
{

    // ─────────────────────────────────────────────────────────────
    // 安全控制器 + EMG/EEG 桥接（真实硬件闭环）
    //
    // 安全控制器负责力矩/速度/位置限位、死区、标定与急停，
    // 桥接控制器把生物信号（EMG/EEG）映射为义肢/义眼意图，
    // 经安全钳制后下发到执行器，形成「信号 → 意图 → 安全 → 执行」
    // 的硬件闭环链路。
    // ─────────────────────────────────────────────────────────────

    // 安全配置
    struct SafetyConfig
    {
        double position_min = -1.5;  // 位置下限（弧度）
        double position_max = 1.5;   // 位置上限
        double max_velocity = 3.0;   // 速度上限（弧度/秒）
        double max_torque = 5.0;     // 力矩上限
        double deadzone = 0.05;      // 死区（防抖动）
    };

    // ─── 安全控制器：钳制指令 + 急停 ─────────────────────────
    class SafetyController
    {
    private:
        SafetyConfig cfg_;
        bool estop_ = false;

    public:
        explicit SafetyController(SafetyConfig cfg = SafetyConfig{}) : cfg_(cfg) {}

        // 位置钳制（含死区与急停）
        double clamp_position(double cmd) const
        {
            if (estop_)
                return 0.0;
            if (std::fabs(cmd) < cfg_.deadzone)
                return 0.0;
            return std::max(cfg_.position_min, std::min(cfg_.position_max, cmd));
        }

        double clamp_velocity(double cmd) const
        {
            if (estop_)
                return 0.0;
            return std::max(-cfg_.max_velocity, std::min(cfg_.max_velocity, cmd));
        }

        double clamp_torque(double cmd) const
        {
            if (estop_)
                return 0.0;
            return std::max(-cfg_.max_torque, std::min(cfg_.max_torque, cmd));
        }

        void emergency_stop()
        {
            estop_ = true;
            RVDLRSP_WARN("hw") << "Safety controller emergency stop.";
        }
        void resume() { estop_ = false; }
        bool is_stopped() const { return estop_; }
        const SafetyConfig &config() const { return cfg_; }
    };

    // ─── EMG 义肢控制器：肌电 → 关节意图 → 执行器 ────────────
    // 把 EMG 信号（sample_bits 阈值化）经 ConsciousnessController
    // 映射为义肢关节目标角，再经安全钳制下发到各关节执行器。
    class EmgProstheticController
    {
    private:
        IBioSignalSource *emg_;
        control::ProstheticLimb &limb_;
        SafetyController safety_;
        std::vector<std::shared_ptr<IActuator>> actuators_;
        double threshold_ = 0.3;

    public:
        EmgProstheticController(IBioSignalSource *emg,
                                control::ProstheticLimb &limb,
                                std::vector<std::shared_ptr<IActuator>> actuators,
                                SafetyConfig safety = SafetyConfig{})
            : emg_(emg), limb_(limb),
              safety_(safety), actuators_(std::move(actuators))
        {
            RVDLRSP_INFO("hw") << "EMG prosthetic controller online ("
                                << actuators_.size() << " actuators).";
        }

        // 每步：EMG → 意图 → 义肢 → 安全钳制 → 执行器
        void step()
        {
            if (!emg_ || !emg_->is_connected())
                return;
            auto bits = emg_->sample_bits(threshold_);
            limb_.set_intent(bits); // 肌电意图 → 关节目标角
            limb_.step();           // 义肢关节推进（含量子 RL 辅助）

            const auto &angles = limb_.angles();
            for (size_t i = 0; i < actuators_.size() && i < angles.size(); ++i)
            {
                double cmd = safety_.clamp_position(angles[i]);
                actuators_[i]->set_position(cmd);
            }
        }

        void emergency_stop()
        {
            safety_.emergency_stop();
            for (auto &a : actuators_)
                a->emergency_stop();
        }

        void resume() { safety_.resume(); }
        bool is_stopped() const { return safety_.is_stopped(); }
    };

    // ─── EEG 义眼控制器：脑电 → 注视意图 ─────────────────────
    // 把 EEG 信号映射为义眼 pan/tilt 注视目标，经安全钳制后
    // 驱动 BionicEye（脑意识 + 量子 RL 追踪）。
    class EegEyeController
    {
    private:
        IBioSignalSource *eeg_;
        control::BionicEye &eye_;
        SafetyController safety_;
        double threshold_ = 0.5;

    public:
        EegEyeController(IBioSignalSource *eeg, control::BionicEye &eye,
                         SafetyConfig safety = SafetyConfig{})
            : eeg_(eeg), eye_(eye), safety_(safety)
        {
            RVDLRSP_INFO("hw") << "EEG eye controller online.";
        }

        // 每步：EEG → 注视意图 → 义眼
        void step()
        {
            if (!eeg_ || !eeg_->is_connected())
                return;
            auto bits = eeg_->sample_bits(threshold_);
            eye_.set_intent(bits); // 脑电意图 → 注视目标
            eye_.step();           // 义眼注视推进（含量子 RL 追踪）
        }

        void emergency_stop() { safety_.emergency_stop(); }
        void resume() { safety_.resume(); }
        bool is_stopped() const { return safety_.is_stopped(); }
    };
}