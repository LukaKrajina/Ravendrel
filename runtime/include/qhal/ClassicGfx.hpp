#pragma once
//
// Classic Graphics Engine（经典图形引擎）—— cgfx_* 内建函数的 C ABI
//
// 面向 rvdl 语言的「经典」2D 图形渲染原语（区别于量子图形引擎 qgfx_*）。
// 与 cgui 共享同一 RenderContext（Nuklear + GLFW + Vulkan），单窗口。
// 坐标单位：窗口像素（原点左上）；颜色：单个 int32 十六进制 0xRRGGBB。
//
// 通用原语（与具体应用无关）：
//   cgfx_rect / cgfx_line / cgfx_polyline / cgfx_ellipse / cgfx_triangle
//
// 折线（polyline）为什么必须是独立原语而不是「调用方循环调 cgfx_line」：
//   逐段独立描边在每个接缝处各画各的帽，夹角处出现缺口或重叠；折线作为
//   **一条路径**提交时，接缝由圆角连接统一处理（fox 的 push_path）。
//   图表 / 曲线 / 状态机的迁移线都吃这个差别。
//
#include "RuntimeApi.h"
#include <cstdint>

#ifndef RVDL_HOST_EXPORT
#define RVDL_HOST_EXPORT extern "C" RVDL_RT_API
#endif

namespace qhal
{
    struct ClassicGfxState
    {
        int32_t initialized = 0;
    };

    inline ClassicGfxState &cgfx_state()
    {
        static ClassicGfxState s;
        return s;
    }
} // namespace qhal

extern "C"
{
    // 填充矩形（x,y 左上；w,h 宽高；color 0xRRGGBB）
    RVDL_HOST_EXPORT void rvdl_cgfx_rect(int32_t x, int32_t y, int32_t w, int32_t h, int32_t color);
    // 直线（x0,y0 起点；x1,y1 终点；thickness 线宽像素；color 0xRRGGBB）
    RVDL_HOST_EXPORT void rvdl_cgfx_line(int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t thickness, int32_t color);
    // 折线（连续多段路径，圆帽 + 圆角连接）。pts 是 2*count 个 int32
    // （x0,y0,x1,y1,…），count < 2 时什么都不画。曲线/图表用它，不要用
    // 循环 cgfx_line —— 接缝会错位（见文件头说明）。
    RVDL_HOST_EXPORT void rvdl_cgfx_polyline(const int32_t *pts, int32_t count, int32_t thickness, int32_t color);
    // 填充椭圆（cx,cy 中心；rx,ry 半轴；color 0xRRGGBB）
    RVDL_HOST_EXPORT void rvdl_cgfx_ellipse(int32_t cx, int32_t cy, int32_t rx, int32_t ry, int32_t color);
    // 填充三角形（三个顶点；color 0xRRGGBB）
    RVDL_HOST_EXPORT void rvdl_cgfx_triangle(int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t x2, int32_t y2, int32_t color);

    // ─── 带 alpha 的原语（alpha 0-255：0 全透明，255 不透明）──────────
    // 供粒子特效等做淡出/拖尾（Nuklear 后端已启用 SRC_ALPHA 混合）。
    RVDL_HOST_EXPORT void rvdl_cgfx_rect_a(int32_t x, int32_t y, int32_t w, int32_t h, int32_t color, int32_t alpha);
    RVDL_HOST_EXPORT void rvdl_cgfx_line_a(int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t thickness, int32_t color, int32_t alpha);
    RVDL_HOST_EXPORT void rvdl_cgfx_polyline_a(const int32_t *pts, int32_t count, int32_t thickness, int32_t color, int32_t alpha);
    RVDL_HOST_EXPORT void rvdl_cgfx_ellipse_a(int32_t cx, int32_t cy, int32_t rx, int32_t ry, int32_t color, int32_t alpha);
    RVDL_HOST_EXPORT void rvdl_cgfx_triangle_a(int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t x2, int32_t y2, int32_t color, int32_t alpha);
}
