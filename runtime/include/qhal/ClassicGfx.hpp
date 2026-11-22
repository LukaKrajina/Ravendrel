#pragma once
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
}

extern "C"
{
    RVDL_HOST_EXPORT void rvdl_cgfx_rect(int32_t x, int32_t y, int32_t w, int32_t h, int32_t color);
    RVDL_HOST_EXPORT void rvdl_cgfx_line(int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t thickness, int32_t color);
    RVDL_HOST_EXPORT void rvdl_cgfx_polyline(const int32_t *pts, int32_t count, int32_t thickness, int32_t color);
    RVDL_HOST_EXPORT void rvdl_cgfx_ellipse(int32_t cx, int32_t cy, int32_t rx, int32_t ry, int32_t color);
    RVDL_HOST_EXPORT void rvdl_cgfx_triangle(int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t x2, int32_t y2, int32_t color);

    RVDL_HOST_EXPORT void rvdl_cgfx_rect_a(int32_t x, int32_t y, int32_t w, int32_t h, int32_t color, int32_t alpha);
    RVDL_HOST_EXPORT void rvdl_cgfx_line_a(int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t thickness, int32_t color, int32_t alpha);
    RVDL_HOST_EXPORT void rvdl_cgfx_polyline_a(const int32_t *pts, int32_t count, int32_t thickness, int32_t color, int32_t alpha);
    RVDL_HOST_EXPORT void rvdl_cgfx_ellipse_a(int32_t cx, int32_t cy, int32_t rx, int32_t ry, int32_t color, int32_t alpha);
    RVDL_HOST_EXPORT void rvdl_cgfx_triangle_a(int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t x2, int32_t y2, int32_t color, int32_t alpha);
}