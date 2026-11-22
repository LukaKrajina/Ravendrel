#pragma once
#include <vector>
#include "consciousness_controller.hpp"
#include "../qcdrc/teleop_driver.hpp"

namespace rvdlrsp::control
{

    class BrainConsciousnessBridge
    {
    public:
        static qcdrc::TeleopDriver::Config apply_to_pd(
            const std::vector<int> &brain_bits,
            const qcdrc::TeleopDriver::Config &base)
        {
            ConsciousnessModulators m = ConsciousnessController::compute(brain_bits);
            qcdrc::TeleopDriver::Config c = base;
            c.kp = ConsciousnessController::apply_gain(base.kp, m.gain_scale);
            c.kd = ConsciousnessController::apply_damping(base.kd, m.damping_scale);
            return c;
        }

        static qpc::Vec3 target_offset(const std::vector<int> &brain_bits)
        {
            ConsciousnessModulators m = ConsciousnessController::compute(brain_bits);
            return {m.target_offset_x, 0.0, m.target_offset_z};
        }

        struct ModulatedControl
        {
            qcdrc::TeleopDriver::Config pd_config;
            qpc::Vec3 offset;
        };

        static ModulatedControl modulate(const std::vector<int> &brain_bits,
                                         const qcdrc::TeleopDriver::Config &base)
        {
            ModulatedControl out;
            out.pd_config = apply_to_pd(brain_bits, base);
            out.offset = target_offset(brain_bits);
            return out;
        }
    };
}