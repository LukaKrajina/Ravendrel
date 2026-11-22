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


    struct SafetyConfig
    {
        double position_min = -1.5;
        double position_max = 1.5;
        double max_velocity = 3.0;
        double max_torque = 5.0;
        double deadzone = 0.05;
    };

    class SafetyController
    {
    private:
        SafetyConfig cfg_;
        bool estop_ = false;

    public:
        explicit SafetyController(SafetyConfig cfg = SafetyConfig{}) : cfg_(cfg) {}

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

        void step()
        {
            if (!emg_ || !emg_->is_connected())
                return;
            auto bits = emg_->sample_bits(threshold_);
            limb_.set_intent(bits);
            limb_.step();

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

        void step()
        {
            if (!eeg_ || !eeg_->is_connected())
                return;
            auto bits = eeg_->sample_bits(threshold_);
            eye_.set_intent(bits);
            eye_.step();
        }

        void emergency_stop() { safety_.emergency_stop(); }
        void resume() { safety_.resume(); }
        bool is_stopped() const { return safety_.is_stopped(); }
    };
}