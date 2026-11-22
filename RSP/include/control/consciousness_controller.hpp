#pragma once
#include <vector>
#include <cstdint>
#include <iostream>

namespace rvdlrsp::control
{

    struct ConsciousnessModulators
    {
        double gain_scale = 1.0;
        double damping_scale = 1.0;
        double target_offset_x = 0.0;
        double target_offset_z = 0.0;
        double arousal = 0.5;
    };

    class ConsciousnessController
    {
    public:
        static ConsciousnessModulators compute(const std::vector<int> &brain_bits)
        {
            ConsciousnessModulators m;
            if (brain_bits.empty())
                return m;

            double sum = 0.0;
            for (int b : brain_bits)
                sum += (b != 0) ? 1.0 : 0.0;
            m.arousal = sum / static_cast<double>(brain_bits.size());

            m.gain_scale = 0.7 + 0.6 * m.arousal;
            m.damping_scale = 1.3 - 0.6 * m.arousal;

            double neutral = m.arousal - 0.5;
            m.target_offset_x = neutral * 2.0;
            m.target_offset_z = -neutral * 1.5;

            return m;
        }

        static double apply_gain(double base_kp, double gain_scale)
        {
            return base_kp * gain_scale;
        }

        static double apply_damping(double base_kd, double damping_scale)
        {
            return base_kd * damping_scale;
        }
    };
}