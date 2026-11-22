#pragma once
#include <cstdint>

namespace qhal
{
    namespace classic_route
    {
        bool fox_active();

        void rect(float x, float y, float w, float h, int32_t rgb, int32_t alpha);
        void line(float x0, float y0, float x1, float y1, float thickness, int32_t rgb,
                  int32_t alpha);
        void polyline(const float *xy, int count, float thickness, int32_t rgb, int32_t alpha);
        void ellipse(float cx, float cy, float rx, float ry, int32_t rgb, int32_t alpha);
        void triangle(float x0, float y0, float x1, float y1, float x2, float y2, int32_t rgb,
                      int32_t alpha);
    }
}