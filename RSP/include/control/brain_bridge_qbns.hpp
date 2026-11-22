#pragma once
#include <vector>
#include <iostream>
#include "hardware/observability.hpp"
#include "brain_bridge.hpp"
#include "qbNs/qbw.hpp"

namespace rvdlrsp::control
{

    class QbnsBrainBridge
    {
    public:
        static std::vector<int> measure_brain_wave(qbns::BrainQuantumWave &wave)
        {
            std::vector<int> bits;
            for (const auto &stream : wave.list_streams())
            {
                if (!stream)
                    continue;
                for (const auto &frame : stream->frames)
                {
                    if (!frame)
                        continue;
                    auto m = frame->measure();
                    bits.insert(bits.end(), m.begin(), m.end());
                }
            }
            return bits;
        }

        static BrainConsciousnessBridge::ModulatedControl bridge(
            qbns::BrainQuantumWave &wave,
            const qcdrc::TeleopDriver::Config &base)
        {
            std::vector<int> bits = measure_brain_wave(wave);
            RVDLRSP_INFO("brain") << "Measured " << bits.size()
                                   << " brain quantum bits.";
            return BrainConsciousnessBridge::modulate(bits, base);
        }
    };
}