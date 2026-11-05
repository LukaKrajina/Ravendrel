#pragma once
// Nuklear 绘制封装。
//
// 项目内置的 Nuklear（vendor/Nuklear）中 nk_fill_rect 是 3 参数版本：
//
//     NK_API void nk_fill_rect(struct nk_command_buffer*, struct nk_rect,
//                              float rounding, struct nk_color);
//
// 上游 Nuklear 则是 4 参数版本，圆角参数独立。与其在各调用点散落
// 圆角实参（及其缺失），容易混淆两种形式，不如用这些封装统一收口。
// 所有 qqui_ 绘制调用都走这里，将来升级 Nuklear 只需改这个头文件。
#ifndef RVDL_QQUI_NUKLEAR_COMPAT_H
#define RVDL_QQUI_NUKLEAR_COMPAT_H

#include "nuklear_config.h"

namespace qqui
{
    // 轴对齐填充矩形。`rounding` 为圆角像素值；0 = 直角。
    inline void fill_rect(struct nk_command_buffer *cv, float x, float y,
                          float w, float h, float rounding, struct nk_color c)
    {
        nk_fill_rect(cv, nk_rect(x, y, w, h), rounding, c);
    }

    inline void fill_rect(struct nk_command_buffer *cv, float x, float y,
                          float w, float h, struct nk_color c)
    {
        fill_rect(cv, x, y, w, h, 0.0f, c);
    }

    inline void fill_circle(struct nk_command_buffer *cv, float cx, float cy,
                            float r, struct nk_color c)
    {
        nk_fill_circle(cv, nk_rect(cx - r, cy - r, r * 2.0f, r * 2.0f), c);
    }

    inline void stroke_line(struct nk_command_buffer *cv, float x0, float y0,
                            float x1, float y1, float thickness, struct nk_color c)
    {
        nk_stroke_line(cv, x0, y0, x1, y1, thickness, c);
    }
}

#endif