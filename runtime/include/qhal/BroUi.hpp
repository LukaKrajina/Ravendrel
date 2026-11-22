#pragma once
#include "RuntimeApi.h"
#include <cstdint>

#ifndef RVDL_HOST_EXPORT
#define RVDL_HOST_EXPORT extern "C" RVDL_RT_API
#endif


extern "C"
{
    RVDL_HOST_EXPORT int32_t rvdl_ui_begin(void);
    RVDL_HOST_EXPORT int32_t rvdl_ui_frame(void);
    RVDL_HOST_EXPORT double rvdl_ui_dt_ms(void);
    RVDL_HOST_EXPORT double rvdl_ui_time_ms(void);

    RVDL_HOST_EXPORT int32_t rvdl_ui_open(void);
    RVDL_HOST_EXPORT int32_t rvdl_ui_win_w(void);
    RVDL_HOST_EXPORT int32_t rvdl_ui_win_h(void);

    RVDL_HOST_EXPORT int32_t rvdl_ui_text(int32_t x, int32_t y, int32_t size, int32_t color,
                                         const char *text);
    RVDL_HOST_EXPORT int32_t rvdl_ui_textw(int32_t x, int32_t y, int32_t w, int32_t line_height,
                                          int32_t size, int32_t color, const char *text);
    RVDL_HOST_EXPORT int32_t rvdl_ui_text_lines(int32_t w, int32_t size, const char *text);
    RVDL_HOST_EXPORT int32_t rvdl_ui_text_width(int32_t size, const char *text);
    RVDL_HOST_EXPORT int32_t rvdl_ui_text_height(int32_t size);
    RVDL_HOST_EXPORT int32_t rvdl_ui_font_count(void);
    RVDL_HOST_EXPORT int32_t rvdl_ui_font_px(int32_t size);
    RVDL_HOST_EXPORT int32_t rvdl_ui_atlas_info(void);

    RVDL_HOST_EXPORT int32_t rvdl_icon_build(int32_t id);
    RVDL_HOST_EXPORT int32_t rvdl_icon_draw(int32_t x, int32_t y, int32_t w, int32_t h,
                                           int32_t color, float alpha_scale);
    RVDL_HOST_EXPORT int32_t rvdl_icon_draw_count(void);

    RVDL_HOST_EXPORT int32_t rvdl_icon_area(void);
    RVDL_HOST_EXPORT int32_t rvdl_icon_size(int32_t id);


    RVDL_HOST_EXPORT void rvdl_icon_reset(void);
    RVDL_HOST_EXPORT void rvdl_icon_fill(float v);

    RVDL_HOST_EXPORT int32_t rvdl_icon_shape_rect(float x, float y, float w, float h, float r);
    RVDL_HOST_EXPORT int32_t rvdl_icon_shape_circle(float cx, float cy, float r);
    RVDL_HOST_EXPORT int32_t rvdl_icon_shape_ring(float cx, float cy, float r, float thickness);
    RVDL_HOST_EXPORT int32_t rvdl_icon_shape_segment(float x0, float y0, float x1, float y1,
                                                     float thickness);
    RVDL_HOST_EXPORT int32_t rvdl_icon_shape_triangle(float x0, float y0, float x1, float y1,
                                                     float x2, float y2);
    RVDL_HOST_EXPORT int32_t rvdl_icon_shape_star(float cx, float cy, float r_outer, float r_inner,
                                                 int32_t points, float rot);

    RVDL_HOST_EXPORT int32_t rvdl_icon_mix(int32_t op);

    RVDL_HOST_EXPORT int32_t rvdl_icon_rotate(float cx, float cy, float angle);
    RVDL_HOST_EXPORT int32_t rvdl_icon_scale(float cx, float cy, float sx, float sy);

    RVDL_HOST_EXPORT int32_t rvdl_icon_tint(int32_t rgb, float hue_deg, float lightness,
                                           float contrast);

    RVDL_HOST_EXPORT int32_t rvdl_ui_rect_a(int32_t x, int32_t y, int32_t w, int32_t h,
                                           int32_t color, int32_t alpha);
    RVDL_HOST_EXPORT int32_t rvdl_ui_clip(int32_t x, int32_t y, int32_t w, int32_t h);
    RVDL_HOST_EXPORT int32_t rvdl_ui_clip_end(void);

    RVDL_HOST_EXPORT int32_t rvdl_ui_key_down(int32_t key);
    RVDL_HOST_EXPORT int32_t rvdl_ui_key_press(int32_t key);
    RVDL_HOST_EXPORT int32_t rvdl_ui_key_sync(void);
    RVDL_HOST_EXPORT int32_t rvdl_ui_char(void);
    RVDL_HOST_EXPORT int32_t rvdl_ui_char_pending(void);
    RVDL_HOST_EXPORT int32_t rvdl_ui_scroll(void);
    RVDL_HOST_EXPORT int32_t rvdl_ui_mouse_x(void);
    RVDL_HOST_EXPORT int32_t rvdl_ui_mouse_y(void);
    RVDL_HOST_EXPORT int32_t rvdl_ui_mouse_down(void);
    RVDL_HOST_EXPORT int32_t rvdl_ui_mouse_press(void);

    RVDL_HOST_EXPORT int32_t rvdl_ui_probe(void);
}

#define GH_NK_KEY_NONE   0
#define GH_NK_KEY_SHIFT  1
#define GH_NK_KEY_CTRL   2
#define GH_NK_KEY_DEL    3
#define GH_NK_KEY_ENTER  4
#define GH_NK_KEY_TAB    5
#define GH_NK_KEY_BACKSP 6
#define GH_NK_KEY_COPY   7
#define GH_NK_KEY_CUT    8
#define GH_NK_KEY_PASTE  9
#define GH_NK_KEY_UP     10
#define GH_NK_KEY_DOWN   11
#define GH_NK_KEY_LEFT   12
#define GH_NK_KEY_RIGHT  13