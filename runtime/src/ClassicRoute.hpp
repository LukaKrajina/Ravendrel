#pragma once
//
// ClassicRoute.hpp —— ClassicGui.cpp / ClassicGfx.cpp 之间的私有接缝
//
// 为什么需要它：cgui_* 与 cgfx_* 是**两个翻译单元**，但必须共享同一个后端
// 决定 ——"本帧谁来画"在 ClassicGui.cpp 里算出来（fox 优先 / Nuklear 兜底，
// 见 qhal/FoxHost.hpp），cgfx_* 只负责问一句"fox 在吗"。
//
// 这里只暴露转发函数，不暴露 FoxHost / GLFW：cgfx 不应该有能力决定后端，
// 也不应该看见窗口类型。属于"内部头文件"，不进 include/。
//
// 坐标约定：**屏幕绝对坐标**。Nuklear 的命令缓冲本身就是屏幕空间（面板不会
// 给手绘命令建立局部原点，见 vendor/Nuklear 的 nk_convert / nk_layout）。fox
// 的原始图元也是屏幕坐标，于是两条路径对 cgfx_* 的调用者语义完全一致。
//
#include <cstdint>

namespace qhal
{
    namespace classic_route
    {
        /// 本帧由 fox 负责吗。false = 调用方走 Nuklear 画布。
        bool fox_active();

        // 原语转发。颜色同 cgfx 约定：0xRRGGBB + alpha(0..255)。
        void rect(float x, float y, float w, float h, int32_t rgb, int32_t alpha);
        void line(float x0, float y0, float x1, float y1, float thickness, int32_t rgb,
                  int32_t alpha);
        /// 折线：xy 是 2*count 个 float，count < 2 时什么都不画。
        /// 逐段调 line() 会在接缝处错位 —— 折线必须作为**一条路径**提交。
        void polyline(const float *xy, int count, float thickness, int32_t rgb, int32_t alpha);
        void ellipse(float cx, float cy, float rx, float ry, int32_t rgb, int32_t alpha);
        void triangle(float x0, float y0, float x1, float y1, float x2, float y2, int32_t rgb,
                      int32_t alpha);
    } // namespace classic_route
} // namespace qhal
