#pragma once
// Fox core/anim.hpp - 共享的动画辅助函数
//
// 悬停/按压的插值动画由所有控件和窗口边框共用，统一放在这里实现，
// 避免每个控件文件各写一份。单一实现也能保证动效一致——各控件缓动
// 速率稍有出入就会让人觉得不对劲。

#include "core/types.hpp"
#include <cmath>

namespace fox
{

    /// 让 `current` 朝 `target`（0..1）逼近，约 `seconds` 秒走完剩余距离。
    /// 指数逼近，与帧率无关：
    ///
    ///   k = 1 - exp(-dt / seconds)
    ///
    /// 简单的 `current += (target-current) * dt * speed` 依赖帧率——
    /// 同一控件在 144Hz 下比 60Hz 下收敛更快，高刷新率显示器上会显得
    /// 不对劲。本写法没有这个问题。
    inline f32 approach(f32 current, f32 target, f32 dt, f32 seconds)
    {
        if (seconds <= 0.0001f) return target;
        f32 k = 1.0f - std::exp(-dt / seconds);
        // 足够接近时直接取目标值，避免渐近尾部的抖动。
        if (fx_abs(target - current) < 0.001f) return target;
        return current + (target - current) * k;
    }

    /// 逼近动画，0->1 全程耗时约 `seconds` 秒。
    inline f32 approach_hover(f32 current, f32 target, f32 dt, f32 seconds)
    {
        return approach(current, target, dt, seconds);
    }

} // namespace fox