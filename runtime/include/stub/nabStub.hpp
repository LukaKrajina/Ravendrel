#pragma once

#include <vector>
#include <map>
#include <cstddef>
#include <cmath>
#include <iostream>


namespace Stub {

    class IDispatch {
    public:
        virtual ~IDispatch() = default;

        virtual void dispatch_buffer_to_slm_hardware(
            const std::vector<std::vector<double>>& phase_mask) = 0;

        virtual void increase_tweezer_depth(size_t qubit_id) = 0;
    };

    using Dispatch = IDispatch;

    class SoftwareDispatch : public IDispatch {
    public:
        std::vector<std::vector<double>> last_slm_frame;
        std::map<size_t, double> tweezer_depths;

        void dispatch_buffer_to_slm_hardware(
            const std::vector<std::vector<double>>& phase_mask) override {
            last_slm_frame = phase_mask;

            constexpr double kTwoPi = 6.283185307179586;
            size_t out_of_range = 0;
            for (const auto& row : phase_mask)
                for (double p : row)
                    if (!(p >= 0.0 && p < kTwoPi))
                        ++out_of_range;

            const size_t h = phase_mask.size();
            const size_t w = phase_mask.empty() ? 0 : phase_mask[0].size();
            std::cout << "[SoftwareDispatch] SLM frame captured "
                      << "(SIMULATION - not real hardware): " << w << "x" << h;
            if (out_of_range)
                std::cerr << "  [warn] " << out_of_range << " pixel(s) phase out of [0, 2pi)";
            std::cout << "\n";
        }

        void increase_tweezer_depth(size_t qubit_id) override {
            tweezer_depths[qubit_id] += 1.0;
        }
    };

}