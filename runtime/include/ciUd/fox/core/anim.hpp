#pragma once

#include "core/types.hpp"
#include <cmath>

namespace fox
{

    inline f32 approach(f32 current, f32 target, f32 dt, f32 seconds)
    {
        if (seconds <= 0.0001f) return target;
        f32 k = 1.0f - std::exp(-dt / seconds);
        if (fx_abs(target - current) < 0.001f) return target;
        return current + (target - current) * k;
    }

    inline f32 approach_hover(f32 current, f32 target, f32 dt, f32 seconds)
    {
        return approach(current, target, dt, seconds);
    }

}