#include "qhal/qicon.hpp"
#include "qhal/BroUi.hpp"

#include <cmath>
#include <cstring>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace
{
    const float kFar = 1e6f;

    void op_min(qicon_field *a, const qicon_field *b)
    {
        for (int i = 0; i < QICON_CELLS; ++i)
            if (b->d[i] < a->d[i]) a->d[i] = b->d[i];
    }

    void op_max(qicon_field *a, const qicon_field *b)
    {
        for (int i = 0; i < QICON_CELLS; ++i)
            if (b->d[i] > a->d[i]) a->d[i] = b->d[i];
    }

    float sample_bilinear(const qicon_field *f, float x, float y)
    {
        if (x < 0) x = 0;
        if (y < 0) y = 0;
        if (x > f->w - 1.001f) x = f->w - 1.001f;
        if (y > f->h - 1.001f) y = f->h - 1.001f;
        const int x0 = (int)x, y0 = (int)y;
        const float fx = x - x0, fy = y - y0;
        const int x1 = x0 + 1 < f->w ? x0 + 1 : x0;
        const int y1 = y0 + 1 < f->h ? y0 + 1 : y0;
        const float a = f->d[y0 * f->w + x0];
        const float b = f->d[y0 * f->w + x1];
        const float c = f->d[y1 * f->w + x0];
        const float d = f->d[y1 * f->w + x1];
        return a * (1 - fx) * (1 - fy) + b * fx * (1 - fy) + c * (1 - fx) * fy + d * fx * fy;
    }

    float clamp01(float v) { return v < 0.f ? 0.f : (v > 1.f ? 1.f : v); }

    float coverage(float d) { return 1.0f - clamp01((d + 0.75f) / 1.5f); }

    nk_color scale_alpha(nk_color c, float k)
    {
        return nk_rgba(c.r, c.g, c.b, (nk_byte)(c.a * (k < 0.f ? 0.f : (k > 1.f ? 1.f : k))));
    }
}

extern "C"
{


void qicon_clear(qicon_field *f)
{
    f->w = QICON_SIZE;
    f->h = QICON_SIZE;
    for (int i = 0; i < QICON_CELLS; ++i) f->d[i] = kFar;
}

void qicon_fill(qicon_field *f, float v)
{
    f->w = QICON_SIZE;
    f->h = QICON_SIZE;
    for (int i = 0; i < QICON_CELLS; ++i) f->d[i] = v;
}


int32_t qicon_rect(qicon_field *f, float x, float y, float w, float h, float r)
{
    if (w <= 0 || h <= 0) return 0;
    if (r < 0) r = 0;
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;

    qicon_field tmp;
    tmp.w = f->w;
    tmp.h = f->h;

    for (int j = 0; j < f->h; ++j)
    {
        for (int i = 0; i < f->w; ++i)
        {
            const float px = (float)i + 0.5f;
            const float py = (float)j + 0.5f;
            const float dx = (px < x) ? (x - px) : ((px > x + w) ? (px - (x + w)) : 0.f);
            const float dy = (py < y) ? (y - py) : ((py > y + h) ? (py - (y + h)) : 0.f);
            float d;
            if (dx == 0.f && dy == 0.f)
            {
                float dist = px - x;
                if (x + w - px < dist) dist = x + w - px;
                if (py - y < dist) dist = py - y;
                if (y + h - py < dist) dist = y + h - py;
                d = -dist;
                if (r > 0.f)
                {
                    float ax = 0.f, ay = 0.f;
                    if (px < x + r) ax = x + r - px;
                    else if (px > x + w - r) ax = px - (x + w - r);
                    if (py < y + r) ay = y + r - py;
                    else if (py > y + h - r) ay = py - (y + h - r);
                    if (ax > 0.f && ay > 0.f) d = -r + std::sqrt(ax * ax + ay * ay);
                }
            }
            else
            {
                d = std::sqrt(dx * dx + dy * dy);
                if (r > 0.f)
                {
                    float ax = 0.f, ay = 0.f;
                    if (px < x + r) ax = x + r - px;
                    else if (px > x + w - r) ax = px - (x + w - r);
                    if (py < y + r) ay = y + r - py;
                    else if (py > y + h - r) ay = py - (y + h - r);
                    if (ax > 0.f && ay > 0.f) d = std::sqrt(ax * ax + ay * ay) - r;
                }
            }
            tmp.d[j * f->w + i] = d;
        }
    }
    std::memcpy(f->d, tmp.d, sizeof(f->d));
    return 1;
}

int32_t qicon_circle(qicon_field *f, float cx, float cy, float r)
{
    if (r <= 0) return 0;
    qicon_field tmp;
    tmp.w = f->w;
    tmp.h = f->h;
    for (int j = 0; j < f->h; ++j)
        {
        for (int i = 0; i < f->w; ++i)
        {
            const float px = (float)i + 0.5f;
            const float py = (float)j + 0.5f;
            tmp.d[j * f->w + i] = std::sqrt((px - cx) * (px - cx) + (py - cy) * (py - cy)) - r;
        }
    }
    std::memcpy(f->d, tmp.d, sizeof(f->d));
    return 1;
}

int32_t qicon_ring(qicon_field *f, float cx, float cy, float r, float thickness)
{
    qicon_field c;
    qicon_clear(&c);
    if (!qicon_circle(&c, cx, cy, r)) return 0;
    if (thickness <= 0) return 0;
    for (int i = 0; i < QICON_CELLS; ++i)
        f->d[i] = std::fabs(c.d[i]) - thickness * 0.5f;
    return 1;
}

int32_t qicon_segment(qicon_field *f, float x0, float y0, float x1, float y1, float thickness)
{
    qicon_field tmp;
    tmp.w = f->w;
    tmp.h = f->h;
    const float dx = x1 - x0, dy = y1 - y0;
    const float len2 = dx * dx + dy * dy;
    for (int j = 0; j < f->h; ++j)
    {
        for (int i = 0; i < f->w; ++i)
        {
            const float px = (float)i + 0.5f;
            const float py = (float)j + 0.5f;
            float t = len2 > 0.f ? ((px - x0) * dx + (py - y0) * dy) / len2 : 0.f;
            if (t < 0.f) t = 0.f;
            if (t > 1.f) t = 1.f;
            const float qx = x0 + t * dx, qy = y0 + t * dy;
            tmp.d[j * f->w + i] = std::sqrt((px - qx) * (px - qx) + (py - qy) * (py - qy)) -
                                  thickness * 0.5f;
        }
    }
    std::memcpy(f->d, tmp.d, sizeof(f->d));
    return 1;
}

int32_t qicon_triangle(qicon_field *f, float x0, float y0, float x1, float y1, float x2, float y2)
{
    auto edge = [](float px, float py, float ax, float ay, float bx, float by) {
        const float ex = bx - ax, ey = by - ay;
        return (px - ax) * ey - (py - ay) * ex;
    };
    qicon_field tmp;
    tmp.w = f->w;
    tmp.h = f->h;
    for (int j = 0; j < f->h; ++j)
    {
        for (int i = 0; i < f->w; ++i)
        {
            const float px = (float)i + 0.5f;
            const float py = (float)j + 0.5f;
            float d0 = edge(px, py, x0, y0, x1, y1);
            float d1 = edge(px, py, x1, y1, x2, y2);
            float d2 = edge(px, py, x2, y2, x0, y0);
            float d = 0.f;
            if (d0 <= 0 && d1 <= 0 && d2 <= 0) d = std::fmax(std::fmax(d0, d1), d2);
            else if (d0 >= 0 && d1 >= 0 && d2 >= 0) d = -std::fmin(std::fmin(d0, d1), d2);
            else d = 0.f;
            const float n0 = std::sqrt((x1 - x0) * (x1 - x0) + (y1 - y0) * (y1 - y0));
            const float n1 = std::sqrt((x2 - x1) * (x2 - x1) + (y2 - y1) * (y2 - y1));
            const float n2 = std::sqrt((x0 - x2) * (x0 - x2) + (y0 - y2) * (y0 - y2));
            const float n = std::fmin(std::fmin(n0, n1), n2);
            tmp.d[j * f->w + i] = n > 0.f ? d / n : 0.f;
        }
    }
    std::memcpy(f->d, tmp.d, sizeof(f->d));
    return 1;
}

int32_t qicon_polygon(qicon_field *f, const float *pts, int32_t count)
{
    if (!pts || count < 3) return 0;
    qicon_field tmp;
    tmp.w = f->w;
    tmp.h = f->h;

    for (int j = 0; j < f->h; ++j)
    {
        for (int i = 0; i < f->w; ++i)
        {
            const float px = (float)i + 0.5f;
            const float py = (float)j + 0.5f;

            float d = kFar;
            for (int e = 0; e < count; ++e)
            {
                const float ax = pts[e * 2], ay = pts[e * 2 + 1];
                const float bx = pts[((e + 1) % count) * 2], by = pts[((e + 1) % count) * 2 + 1];
                const float ex = bx - ax, ey = by - ay;
                const float wx = px - ax, wy = py - ay;
                const float len2 = ex * ex + ey * ey;
                float h = len2 > 0.f ? (wx * ex + wy * ey) / len2 : 0.f;
                if (h < 0.f) h = 0.f;
                if (h > 1.f) h = 1.f;
                const float dx = wx - h * ex, dy = wy - h * ey;
                const float seg = std::sqrt(dx * dx + dy * dy);
                if (seg < d) d = seg;
            }

            bool inside = false;
            for (int e = 0; e < count; ++e)
            {
                const int n = (e + 1 == count) ? 0 : (e + 1);
                const float ax = pts[e * 2], ay = pts[e * 2 + 1];
                const float bx = pts[n * 2], by = pts[n * 2 + 1];
                if (((ay > py) != (by > py)) &&
                    (px < (bx - ax) * (py - ay) / (by - ay) + ax))
                {
                    inside = !inside;
                }
            }
            tmp.d[j * f->w + i] = inside ? -d : d;
        }
    }
    std::memcpy(f->d, tmp.d, sizeof(f->d));
    return 1;
}

int32_t qicon_star(qicon_field *f, float cx, float cy, float r_outer, float r_inner, int32_t points,
                   float rot)
{
    if (points < 3) points = 5;
    float pts[32 * 2];
    const float step = (float)M_PI / (float)points;
    float a = rot;
    for (int i = 0; i < points * 2; ++i)
    {
        const float r = (i % 2 == 0) ? r_outer : r_inner;
        pts[i * 2] = cx + std::cos(a) * r;
        pts[i * 2 + 1] = cy + std::sin(a) * r;
        a += step;
    }
    return qicon_polygon(f, pts, points * 2);
}


int32_t qicon_union(qicon_field *a, const qicon_field *b)
{
    op_min(a, b);
    return 1;
}

int32_t qicon_subtract(qicon_field *a, const qicon_field *b)
{
    qicon_field neg;
    neg.w = b->w;
    neg.h = b->h;
    for (int i = 0; i < QICON_CELLS; ++i) neg.d[i] = -b->d[i];
    op_max(a, &neg);
    return 1;
}

int32_t qicon_intersect(qicon_field *a, const qicon_field *b)
{
    op_max(a, b);
    return 1;
}

int32_t qicon_smooth_union(qicon_field *a, const qicon_field *b, float k)
{
    if (k <= 0) return qicon_union(a, b);
    qicon_field tmp;
    tmp.w = a->w;
    tmp.h = a->h;
    for (int i = 0; i < QICON_CELLS; ++i)
    {
        const float h = clamp01(0.5f + 0.5f * (b->d[i] - a->d[i]) / k);
        tmp.d[i] = a->d[i] * (1.f - h) + b->d[i] * h - k * h * (1.f - h);
    }
    std::memcpy(a->d, tmp.d, sizeof(a->d));
    return 1;
}


int32_t qicon_transform(qicon_field *dst, const qicon_field *src, const float m[6])
{
    if (!dst || !src || !m) return 0;
    dst->w = src->w;
    dst->h = src->h;
    for (int j = 0; j < src->h; ++j)
    {
        for (int i = 0; i < src->w; ++i)
        {
            const float px = (float)i + 0.5f;
            const float py = (float)j + 0.5f;
            const float sx = m[0] * px + m[1] * py + m[2];
            const float sy = m[3] * px + m[4] * py + m[5];
            dst->d[j * src->w + i] = sample_bilinear(src, sx, sy);
        }
    }
    return 1;
}

int32_t qicon_rotate(qicon_field *dst, const qicon_field *src, float cx, float cy, float angle)
{
    const float c = std::cos(angle), s = std::sin(angle);
    const float m[6] = {c, s, cx - c * cx - s * cy, -s, c, cy + s * cx - c * cy};
    return qicon_transform(dst, src, m);
}

int32_t qicon_scale(qicon_field *dst, const qicon_field *src, float cx, float cy, float sx, float sy)
{
    if (sx == 0 || sy == 0) return 0;
    const float m[6] = {1.f / sx, 0.f, cx - cx / sx, 0.f, 1.f / sy, cy - cy / sy};
    return qicon_transform(dst, src, m);
}


void qicon_color_transform(int32_t out[3], const int32_t in[3], const float M[9], const float off[3])
{
    const float src[3] = {(float)in[0], (float)in[1], (float)in[2]};
    for (int c = 0; c < 3; ++c)
    {
        const float o = off ? off[c] : 0.f;
        float v = M[c * 3 + 0] * src[0] + M[c * 3 + 1] * src[1] + M[c * 3 + 2] * src[2] + o;
        if (v < 0) v = 0;
        if (v > 255) v = 255;
        out[c] = (int32_t)(v + 0.5f);
    }
}

int32_t qicon_tint(int32_t rgb, float hue_deg, float lightness, float contrast)
{
    const int32_t in[3] = {(rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF};
    float work[3];
    for (int c = 0; c < 3; ++c)
    {
        float v = ((float)in[c] - 128.f) * contrast + 128.f;
        v += (lightness - 0.5f) * 2.f * 255.f;
        if (v < 0.f) v = 0.f;
        if (v > 255.f) v = 255.f;
        work[c] = v;
    }

    int32_t base[3] = {(int32_t)work[0], (int32_t)work[1], (int32_t)work[2]};

    if (hue_deg != 0.f)
    {
        const float a = hue_deg * (float)M_PI / 180.f;
        const float cosA = std::cos(a), sinA = std::sin(a);
        const float H[9] = {0.299f + 0.701f * cosA + 0.168f * sinA,
                            0.587f - 0.587f * cosA + 0.330f * sinA,
                            0.114f - 0.114f * cosA - 0.497f * sinA,
                            0.299f - 0.299f * cosA - 0.328f * sinA,
                            0.587f + 0.413f * cosA + 0.035f * sinA,
                            0.114f - 0.114f * cosA + 0.292f * sinA,
                            0.299f - 0.300f * cosA + 1.250f * sinA,
                            0.587f - 0.588f * cosA - 1.050f * sinA,
                            0.114f + 0.886f * cosA - 0.203f * sinA};
        qicon_color_transform(base, base, H, nullptr);
    }
    return (base[0] << 16) | (base[1] << 8) | base[2];
}


int32_t qicon_draw(nk_command_buffer *cb, float x, float y, float w, float h, const qicon_field *f,
                   int32_t color, float alpha_scale)
{
    if (!cb || !f || w <= 0 || h <= 0) return 0;
    const float sx = (float)f->w / w;
    const float sy = (float)f->h / h;
    if (sx <= 0 || sy <= 0) return 0;

    const nk_color base = nk_rgba((nk_byte)((color >> 16) & 0xFF), (nk_byte)((color >> 8) & 0xFF),
                                  (nk_byte)(color & 0xFF), 255);
    int32_t n = 0;

    const int px0 = (int)std::floor(x);
    const int py0 = (int)std::floor(y);
    const int px1 = (int)std::ceil(x + w);
    const int py1 = (int)std::ceil(y + h);

    for (int py = py0; py < py1; ++py)
    {
        float fy = (float)(py + 0.5 - y) * sy;
        if (fy < 0.f) fy = 0.f;
        if (fy >= (float)f->h) fy = (float)f->h - 0.001f;

        int px = px0;
        while (px < px1)
        {
            while (px < px1 &&
                   coverage(sample_bilinear(f, (float)(px + 0.5 - x) * sx, fy)) * alpha_scale <= 0.004f)
                px++;
            if (px >= px1) break;
            const int start = px;
            float acc = 0.f;
            while (px < px1 &&
                   coverage(sample_bilinear(f, (float)(px + 0.5 - x) * sx, fy)) * alpha_scale > 0.004f)
            {
                acc += coverage(sample_bilinear(f, (float)(px + 0.5 - x) * sx, fy));
                px++;
            }
            const float cov = (acc / (float)(px - start)) * alpha_scale;
            nk_fill_rect(cb, nk_rect((float)start, (float)py, (float)(px - start), 1.f), 0.f,
                         scale_alpha(base, cov));
            n++;
        }
    }
    return n;
}

int32_t qicon_draw_count(const qicon_field *f)
{
    if (!f) return 0;
    int32_t n = 0;
    for (int j = 0; j < f->h; ++j)
    {
        int i = 0;
        while (i < f->w)
        {
            while (i < f->w && coverage(f->d[j * f->w + i]) <= 0.004f) i++;
            if (i >= f->w) break;
            while (i < f->w && coverage(f->d[j * f->w + i]) > 0.004f) i++;
            n++;
        }
    }
    return n;
}


namespace
{
    void combine(qicon_field *a, const qicon_field &b) { op_min(a, &b); }
}

int32_t qicon_build(int32_t id, qicon_field *out)
{
    if (!out) return 0;
    qicon_clear(out);
    qicon_field t;

    switch (id)
    {
        case QICON_SEND:
        {
            qicon_triangle(out, 6.f, 11.f, 24.f, 16.f, 6.f, 21.f);
            qicon_field wing;
            qicon_clear(&wing);
            qicon_segment(&wing, 6.f, 21.f, 18.f, 26.f, 2.4f);
            qicon_union(out, &wing);
            break;
        }
        case QICON_ATTACH:
        {
            qicon_ring(out, 16.f, 16.f, 9.f, 2.6f);
            qicon_field inner;
            qicon_clear(&inner);
            qicon_ring(&inner, 16.f, 16.f, 4.5f, 2.6f);
            qicon_subtract(out, &inner);
            break;
        }
        case QICON_SEARCH:
        {
            qicon_ring(out, 14.f, 14.f, 7.f, 3.f);
            qicon_field handle;
            qicon_clear(&handle);
            qicon_segment(&handle, 19.f, 19.f, 27.f, 27.f, 3.2f);
            qicon_union(out, &handle);
            break;
        }
        case QICON_SETTINGS:
        {
            qicon_ring(out, 16.f, 16.f, 10.5f, 3.f);
            qicon_field hole;
            qicon_clear(&hole);
            qicon_circle(&hole, 16.f, 16.f, 4.2f);
            qicon_subtract(out, &hole);
            for (int k = 0; k < 6; ++k)
            {
                const float a = (float)k * (float)M_PI / 3.f;
                const float cx = 16.f + std::cos(a) * 12.5f;
                const float cy = 16.f + std::sin(a) * 12.5f;
                qicon_field tooth;
                qicon_clear(&tooth);
                qicon_rect(&tooth, cx - 1.8f, cy - 1.8f, 3.6f, 3.6f, 1.f);
                qicon_union(out, &tooth);
            }
            break;
        }
        case QICON_USER:
        {
            qicon_circle(out, 16.f, 11.f, 5.5f);
            qicon_field body;
            qicon_clear(&body);
            qicon_rect(&body, 7.f, 19.f, 18.f, 10.f, 4.5f);
            qicon_union(out, &body);
            break;
        }
        case QICON_TOOL:
        {
            qicon_ring(out, 13.f, 13.f, 6.f, 3.4f);
            qicon_field hole;
            qicon_clear(&hole);
            qicon_circle(&hole, 13.f, 13.f, 2.6f);
            qicon_subtract(out, &hole);
            qicon_field grip;
            qicon_clear(&grip);
            qicon_segment(&grip, 17.5f, 17.5f, 26.f, 26.f, 4.2f);
            qicon_union(out, &grip);
            break;
        }
        case QICON_WARN:
        {
            qicon_triangle(out, 16.f, 4.5f, 28.f, 27.f, 4.f, 27.f);
            qicon_field ex;
            qicon_clear(&ex);
            qicon_segment(&ex, 16.f, 12.f, 16.f, 19.f, 2.6f);
            qicon_union(out, &ex);
            qicon_field dot;
            qicon_clear(&dot);
            qicon_circle(&dot, 16.f, 23.5f, 1.4f);
            qicon_union(out, &dot);
            break;
        }
        case QICON_OK:
        {
            qicon_ring(out, 16.f, 16.f, 10.5f, 3.2f);
            qicon_field tick;
            qicon_clear(&tick);
            qicon_segment(&tick, 10.5f, 16.5f, 14.5f, 20.5f, 3.f);
            qicon_segment(&tick, 14.5f, 20.5f, 22.f, 11.5f, 3.f);
            qicon_union(out, &tick);
            break;
        }
        case QICON_FAIL:
        {
            qicon_ring(out, 16.f, 16.f, 10.5f, 3.2f);
            qicon_field cross;
            qicon_clear(&cross);
            qicon_segment(&cross, 10.5f, 10.5f, 21.5f, 21.5f, 3.f);
            qicon_segment(&cross, 21.5f, 10.5f, 10.5f, 21.5f, 3.f);
            qicon_union(out, &cross);
            break;
        }
        case QICON_CHIMERA:
        {
            qicon_ring(out, 12.5f, 16.f, 7.5f, 2.8f);
            qicon_field second;
            qicon_clear(&second);
            qicon_ring(&second, 19.5f, 16.f, 7.5f, 2.8f);
            qicon_smooth_union(out, &second, 2.f);
            break;
        }
        case QICON_PLAN:
        {
            for (int r = 0; r < 3; ++r)
            {
                const float ry = 9.f + r * 7.5f;
                qicon_field line;
                qicon_clear(&line);
                qicon_segment(&line, 11.f, ry, 26.f, ry, 2.4f);
                qicon_union(out, &line);
                qicon_field mark;
                qicon_clear(&mark);
                if (r == 0)
                {
                    qicon_segment(&mark, 6.5f, ry + 0.5f, 8.2f, ry + 2.2f, 2.2f);
                    qicon_segment(&mark, 8.2f, ry + 2.2f, 10.8f, ry - 1.6f, 2.2f);
                }
                else
                {
                    qicon_circle(&mark, 8.5f, ry, 1.8f);
                }
                qicon_union(out, &mark);
            }
            break;
        }
        case QICON_VERIFY:
        {
            const float shield[5 * 2] = {16.f, 4.f, 27.f, 10.f, 23.f, 27.f,
                                          9.f,  27.f, 5.f, 10.f};
            qicon_polygon(out, shield, 5);
            qicon_field tick;
            qicon_clear(&tick);
            qicon_segment(&tick, 11.f, 16.5f, 14.8f, 20.5f, 2.8f);
            qicon_segment(&tick, 14.8f, 20.5f, 21.f, 11.5f, 2.8f);
            qicon_subtract(out, &tick);
            break;
        }
        case QICON_BOT:
        {
            qicon_circle(out, 16.f, 12.f, 6.f);
            qicon_field ears;
            qicon_clear(&ears);
            qicon_circle(&ears, 8.5f, 12.f, 1.6f);
            qicon_union(&ears, &t);
            qicon_clear(&t);
            qicon_circle(&t, 23.5f, 12.f, 1.6f);
            qicon_union(&ears, &t);
            qicon_union(out, &ears);

            qicon_field body;
            qicon_clear(&body);
            qicon_rect(&body, 8.f, 19.f, 16.f, 9.f, 2.5f);
            qicon_union(out, &body);

            qicon_field eyes;
            qicon_clear(&eyes);
            qicon_segment(&eyes, 12.5f, 12.5f, 14.5f, 12.5f, 1.6f);
            qicon_segment(&eyes, 17.5f, 12.5f, 19.5f, 12.5f, 1.6f);
            qicon_subtract(out, &eyes);

            qicon_field antenna;
            qicon_clear(&antenna);
            qicon_segment(&antenna, 16.f, 6.f, 16.f, 9.f, 1.6f);
            qicon_union(out, &antenna);
            break;
        }
        case QICON_HISTORY:
        {
            qicon_ring(out, 16.f, 16.f, 10.5f, 2.8f);
            qicon_field hands;
            qicon_clear(&hands);
            qicon_segment(&hands, 16.f, 16.f, 16.f, 9.5f, 2.4f);
            qicon_segment(&hands, 16.f, 16.f, 20.5f, 18.f, 2.4f);
            qicon_union(out, &hands);
            break;
        }
        case QICON_SKILL:
        {
            const float bolt[7 * 2] = {18.5f, 4.f, 10.5f, 18.f, 15.f, 18.f,
                                       13.5f, 28.f, 21.5f, 13.f, 17.f, 13.f};
            qicon_polygon(out, bolt, 7);
            break;
        }
        case QICON_BRAND:
        {
            qicon_circle(out, 19.f, 13.2f, 7.6f);

            qicon_field beak;
            qicon_clear(&beak);
            const float bk[5 * 2] = {13.2f, 10.2f, 2.6f, 13.3f, 1.0f, 14.2f,
                                     2.2f, 15.6f, 12.6f, 16.6f};
            qicon_polygon(&beak, bk, 5);
            qicon_union(out, &beak);

            qicon_field neck;
            qicon_clear(&neck);
            const float nk[9 * 2] = {24.5f, 18.5f, 22.5f, 25.0f, 16.0f, 27.6f,
                                     11.5f, 24.5f, 10.2f, 23.2f, 11.5f, 21.9f,
                                     13.2f, 18.6f, 13.0f, 17.6f, 12.4f, 16.6f};
            qicon_polygon(&neck, nk, 9);
            qicon_union(out, &neck);

            qicon_field eye;
            qicon_clear(&eye);
            qicon_circle(&eye, 15.2f, 14.1f, 1.5f);
            qicon_subtract(out, &eye);
            break;
        }
        case QICON_CHAT:
        {
            qicon_rect(out, 4.f, 6.5f, 24.f, 16.5f, 5.f);
            qicon_field tail;
            qicon_clear(&tail);
            qicon_triangle(&tail, 9.f, 22.f, 15.5f, 22.f, 8.5f, 27.5f);
            qicon_union(out, &tail);
            for (int k = 0; k < 3; ++k)
            {
                qicon_field dot;
                qicon_clear(&dot);
                qicon_circle(&dot, 10.5f + 5.5f * k, 14.8f, 1.5f);
                qicon_subtract(out, &dot);
            }
            break;
        }
        case QICON_FILE:
        {
            qicon_rect(out, 7.f, 4.f, 18.f, 24.f, 2.5f);
            qicon_field fold;
            qicon_clear(&fold);
            qicon_triangle(&fold, 25.f, 4.f, 25.f, 11.f, 18.f, 4.f);
            qicon_union(out, &fold);

            qicon_field crease;
            qicon_clear(&crease);
            qicon_segment(&crease, 18.f, 4.f, 18.f, 11.f, 1.2f);
            qicon_segment(&crease, 18.f, 11.f, 25.f, 11.f, 1.2f);
            qicon_subtract(out, &crease);

            qicon_field rows;
            qicon_clear(&rows);
            qicon_segment(&rows, 10.5f, 16.f, 21.5f, 16.f, 1.8f);
            qicon_segment(&rows, 10.5f, 20.f, 21.5f, 20.f, 1.8f);
            qicon_segment(&rows, 10.5f, 24.f, 18.f, 24.f, 1.8f);
            qicon_subtract(out, &rows);
            break;
        }
        default:
            return 0;
    }
    return 1;
}

int32_t qicon_total(void) { return QICON_COUNT; }

int32_t qicon_default_size(int32_t id)
{
    switch (id)
    {
    case QICON_BRAND: return 24;
    case QICON_CHAT: return 18;
    case QICON_FILE: return 16;
    case QICON_OK:
    case QICON_FAIL:
    case QICON_WARN: return 16;
    case QICON_SETTINGS:
    case QICON_SEARCH: return 18;
    default:
        return (id >= 0 && id < QICON_COUNT) ? 20 : 0;
    }
}

}