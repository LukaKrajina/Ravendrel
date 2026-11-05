#pragma once
#include <cstdint>
#include <random>
#include <iostream>
#include "hardware/observability.hpp"

namespace rvdlrsp::pcg
{

    // 程序化内容生成（种子驱动的确定性生成）
    class PCGFramework
    {
    private:
        std::mt19937 rng_;

    public:
        explicit PCGFramework(uint32_t seed) : rng_(seed)
        {
            RVDLRSP_INFO("pcg") << "PCG framework seeded (" << seed << ").";
        }
        double next_unit() { return std::uniform_real_distribution<double>(0, 1)(rng_); }
        int next_int(int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(rng_); }
    };
}