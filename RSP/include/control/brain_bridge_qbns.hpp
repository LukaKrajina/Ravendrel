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
        // 从脑量子波测量所有量子流的帧，得到 bits 序列
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

        // 测量脑波 → 调制控制
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