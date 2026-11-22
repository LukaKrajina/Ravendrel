#pragma once
#include "RuntimeApi.h"
#include "nuklear_config.h"
#include <cstdint>

#ifndef RVDL_HOST_EXPORT
#define RVDL_HOST_EXPORT extern "C" RVDL_RT_API
#endif


#define QICON_SIZE 32
#define QICON_CELLS (QICON_SIZE * QICON_SIZE)

typedef struct qicon_field
{
    int32_t w, h;
    float d[QICON_CELLS];
} qicon_field;

RVDL_HOST_EXPORT void qicon_clear(qicon_field *f);

RVDL_HOST_EXPORT void qicon_fill(qicon_field *f, float v);


RVDL_HOST_EXPORT int32_t qicon_rect(qicon_field *f, float x, float y, float w, float h, float r);
RVDL_HOST_EXPORT int32_t qicon_circle(qicon_field *f, float cx, float cy, float r);
RVDL_HOST_EXPORT int32_t qicon_ring(qicon_field *f, float cx, float cy, float r, float thickness);
RVDL_HOST_EXPORT int32_t qicon_segment(qicon_field *f, float x0, float y0, float x1, float y1,
                                        float thickness);
RVDL_HOST_EXPORT int32_t qicon_triangle(qicon_field *f, float x0, float y0, float x1, float y1,
                                         float x2, float y2);
RVDL_HOST_EXPORT int32_t qicon_star(qicon_field *f, float cx, float cy, float r_outer, float r_inner,
                                     int32_t points, float rot);
RVDL_HOST_EXPORT int32_t qicon_polygon(qicon_field *f, const float *pts, int32_t count);


RVDL_HOST_EXPORT int32_t qicon_union(qicon_field *a, const qicon_field *b);
RVDL_HOST_EXPORT int32_t qicon_subtract(qicon_field *a, const qicon_field *b);
RVDL_HOST_EXPORT int32_t qicon_intersect(qicon_field *a, const qicon_field *b);
RVDL_HOST_EXPORT int32_t qicon_smooth_union(qicon_field *a, const qicon_field *b, float k);


RVDL_HOST_EXPORT int32_t qicon_transform(qicon_field *dst, const qicon_field *src, const float m[6]);

RVDL_HOST_EXPORT int32_t qicon_rotate(qicon_field *dst, const qicon_field *src, float cx, float cy,
                                        float angle);
RVDL_HOST_EXPORT int32_t qicon_scale(qicon_field *dst, const qicon_field *src, float cx, float cy,
                                       float sx, float sy);


RVDL_HOST_EXPORT void qicon_color_transform(int32_t out[3], const int32_t in[3], const float M[9],
                                              const float off[3]);

RVDL_HOST_EXPORT int32_t qicon_tint(int32_t rgb, float hue_deg, float lightness, float contrast);


RVDL_HOST_EXPORT int32_t qicon_draw(nk_command_buffer *cb, float x, float y, float w, float h,
                                      const qicon_field *f, int32_t color, float alpha_scale);

RVDL_HOST_EXPORT int32_t qicon_draw_count(const qicon_field *f);


enum
{
    QICON_SEND = 0,
    QICON_ATTACH,
    QICON_SEARCH,
    QICON_SETTINGS,
    QICON_USER,
    QICON_TOOL,
    QICON_WARN,
    QICON_OK,
    QICON_FAIL,
    QICON_CHIMERA,
    QICON_PLAN,
    QICON_VERIFY,
    QICON_BOT,
    QICON_HISTORY,
    QICON_SKILL,
    QICON_BRAND,
    QICON_CHAT,
    QICON_FILE,
    QICON_COUNT
};

RVDL_HOST_EXPORT int32_t qicon_total(void);

RVDL_HOST_EXPORT int32_t qicon_build(int32_t id, qicon_field *out);

RVDL_HOST_EXPORT int32_t qicon_default_size(int32_t id);


enum
{
    QICON_OP_UNION = 0,
    QICON_OP_SUBTRACT = 1,
    QICON_OP_INTERSECT = 2,
    QICON_OP_SMOOTH = 3,
};