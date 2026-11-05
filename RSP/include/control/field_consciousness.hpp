#pragma once
#include <cmath>
#include "consciousness_controller.hpp"
#include "spacetime/NeuralField.hpp"

namespace rvdlrsp::control
{
    class FieldConsciousnessController
    {
    public:
        static ConsciousnessModulators compute(
            const rvdl::spacetime::NeuralFieldOrderParameter &op)
        {
            ConsciousnessModulators m;
            m.arousal = 1.0 / (1.0 + std::exp(-4.0 * op.mean_field));
            m.gain_scale = 0.7 + 0.6 * m.arousal;
            m.damping_scale = 1.3 - 0.6 * m.arousal + 0.3 * op.domain_wall_density;

            double neutral = m.arousal - 0.5;
            m.target_offset_x = neutral * 2.0;
            m.target_offset_z = -neutral * 1.5;
            return m;
        }
    };
}