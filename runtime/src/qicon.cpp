// ═══════════════════════════════════════════════════════════════════════
// Bro 图标库实现
// ═══════════════════════════════════════════════════════════════════════
//
// 设计说明见 qicon.hpp 顶部。这里只写数学。
//
// 一个关键的决定：**场固定 32×32**。
// 图标在界面上显示尺寸是 12~28px，32×32 的场在 2x 缩放下仍有余量，
// 而固定尺寸让所有图标可以共用一套栈上缓存 —— 不需要 malloc，
// 也不需要为「图标占用多少内存」这种问题操心。

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

    /// 逐元素 min
    void op_min(qicon_field *a, const qicon_field *b)
    {
        for (int i = 0; i < QICON_CELLS; ++i)
            if (b->d[i] < a->d[i]) a->d[i] = b->d[i];
    }

    /// 逐元素 max
    void op_max(qicon_field *a, const qicon_field *b)
    {
        for (int i = 0; i < QICON_CELLS; ++i)
            if (b->d[i] > a->d[i]) a->d[i] = b->d[i];
    }

    /// 双线性插值采样
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

    /// 覆盖率 → alpha 字节。
    /// 1.5px 的过渡带：比 1px 更平滑，又不会让细线条发虚。
    float coverage(float d) { return 1.0f - clamp01((d + 0.75f) / 1.5f); }

    nk_color scale_alpha(nk_color c, float k)
    {
        return nk_rgba(c.r, c.g, c.b, (nk_byte)(c.a * (k < 0.f ? 0.f : (k > 1.f ? 1.f : k))));
    }
} // namespace

extern "C"
{

// ═══════════════════════════════════════════════════════════════════════
// 场操作
// ═══════════════════════════════════════════════════════════════════════

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

// ═══════════════════════════════════════════════════════════════════════
// 形状原语
// ═══════════════════════════════════════════════════════════════════════

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
            // 到矩形边界的距离（内部为负）
            const float dx = (px < x) ? (x - px) : ((px > x + w) ? (px - (x + w)) : 0.f);
            const float dy = (py < y) ? (y - py) : ((py > y + h) ? (py - (y + h)) : 0.f);
            float d;
            if (dx == 0.f && dy == 0.f)
            {
                // 内部：到最近边的距离取负
                float dist = px - x;
                if (x + w - px < dist) dist = x + w - px;
                if (py - y < dist) dist = py - y;
                if (y + h - py < dist) dist = y + h - py;
                d = -dist;
                // 圆角：把内角按半径切掉
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
                // 圆角外扩
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
    // 三条有向边的半平面取 max（内侧为负）
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
            // 统一成「内部为负」
            float d = 0.f;
            if (d0 <= 0 && d1 <= 0 && d2 <= 0) d = std::fmax(std::fmax(d0, d1), d2);
            else if (d0 >= 0 && d1 >= 0 && d2 >= 0) d = -std::fmin(std::fmin(d0, d1), d2);
            else d = 0.f;
            // 半平面法向长度不是单位长度，需要归一化
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

            // 标准多边形 SDF：到各边线段的最短距离，再用奇偶规则定内外。
            //
            // 内外判定**必须**用射线法（奇偶/spnpoly），不能用「对所有边
            // 叉积同号」。后者只对凸多边形成立：凹多边形深处（比如闪电
            // 的尖、五角星的臂）的点，会被远处某条边的叉积判成外部。
            // 踩过一次：五角星面积只剩 90/1024，整个图标缩成一小块，
            // 而「图标非空」这种弱断言照样通过。
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

            // pnpoly：从 (px,py) 向右发一条射线，数它与多少条边相交。奇数 = 内部。
            bool inside = false;
            for (int e = 0; e < count; ++e)
            {
                const int n = (e + 1 == count) ? 0 : (e + 1);
                const float ax = pts[e * 2], ay = pts[e * 2 + 1];
                const float bx = pts[n * 2], by = pts[n * 2 + 1];
                // 边的两个端点跨在 py 两侧，且交点在 px 右侧
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

// ═══════════════════════════════════════════════════════════════════════
// 组合代数
// ═══════════════════════════════════════════════════════════════════════

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

// ═══════════════════════════════════════════════════════════════════════
// 仿射变换
// ═══════════════════════════════════════════════════════════════════════

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
            // 逆变换：dst 的像素 → src 的坐标
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
    // 正向：绕 (cx,cy) 旋转。这里给的是「dst 像素来自 src 的哪里」，
    // 所以用逆矩阵（-angle）。
    const float m[6] = {c, s, cx - c * cx - s * cy, -s, c, cy + s * cx - c * cy};
    return qicon_transform(dst, src, m);
}

int32_t qicon_scale(qicon_field *dst, const qicon_field *src, float cx, float cy, float sx, float sy)
{
    if (sx == 0 || sy == 0) return 0;
    const float m[6] = {1.f / sx, 0.f, cx - cx / sx, 0.f, 1.f / sy, cy - cy / sy};
    return qicon_transform(dst, src, m);
}

// ═══════════════════════════════════════════════════════════════════════
// 颜色
// ═══════════════════════════════════════════════════════════════════════

void qicon_color_transform(int32_t out[3], const int32_t in[3], const float M[9], const float off[3])
{
    const float src[3] = {(float)in[0], (float)in[1], (float)in[2]};
    for (int c = 0; c < 3; ++c)
    {
        // off 可以为 nullptr —— 色相旋转那种纯矩阵变换不需要偏移。
        // 之前直接用 off[c]，传 nullptr 时是空指针解引用。
        const float o = off ? off[c] : 0.f;
        float v = M[c * 3 + 0] * src[0] + M[c * 3 + 1] * src[1] + M[c * 3 + 2] * src[2] + o;
        if (v < 0) v = 0;
        if (v > 255) v = 255;
        // 必须四舍五入，不能截断。矩阵相乘后通道值几乎总是 x.9999
        // （比如恒等变换下 64 → 63.99999），截断成 63 会让「色相转 360°
        // 回到原色」这种断言以 1 的偏差失败，而且画面上的色带会偏。
        out[c] = (int32_t)(v + 0.5f);
    }
}

int32_t qicon_tint(int32_t rgb, float hue_deg, float lightness, float contrast)
{
    // 两个参数都取「1 / 0.5 是不动点」：
    //
    //   contrast   = 1   不变（以 128 为中心缩放）
    //   lightness  = 0.5 不变；1 = 拉满到白，0 = 压到黑
    //
    // 为什么不用「乘个系数」：那会让 0.5 既不是白也不是黑也不是原色，
    // 调主题色时没法预测结果。有明确不动点的变换才好用。
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

    // 色相旋转走 YIQ 空间的旋转矩阵
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

// ═══════════════════════════════════════════════════════════════════════
// 光栅化
// ═══════════════════════════════════════════════════════════════════════
//
// 逐行扫描，把覆盖率 > 0 的连续格子合并成水平条带，每条带一个 nk_fill_rect。
// 合并是关键：32×32 = 1024 格，合并后通常只有 60~120 条命令，
// 比「一格一个矩形」省一个数量级，而且边缘更干净。

int32_t qicon_draw(nk_command_buffer *cb, float x, float y, float w, float h, const qicon_field *f,
                   int32_t color, float alpha_scale)
{
    if (!cb || !f || w <= 0 || h <= 0) return 0;
    // 输出像素 → 场格子的换算。注意方向：场是 32×32，
    // 图标常以 12~20px 显示，所以一个输出像素往往跨多个格子。
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
            // 段内仍取平均：那是面积加权的正确抗锯齿。
            // 只是现在每段至少是一个完整输出像素，不会再像「按格遍历」那样
            // 把 0.5px 宽的笔画打半折、整条发暗。
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

// ═══════════════════════════════════════════════════════════════════════
// 命名图标
// ═══════════════════════════════════════════════════════════════════════
//
// 每个图标由 2~5 个原语组合而成。尺寸都以 32×32 的场坐标为基准
// （0..32），与最终显示大小无关 —— 所以同一份定义在 12px 和 28px
// 下都清晰。

namespace
{
    /// 组合两个场：a = op(a, b)
    void combine(qicon_field *a, const qicon_field &b) { op_min(a, &b); }
} // namespace

int32_t qicon_build(int32_t id, qicon_field *out)
{
    if (!out) return 0;
    qicon_clear(out);
    qicon_field t;

    switch (id)
    {
        case QICON_SEND:
        {
            // 纸飞机：一个朝右的三角形 + 一条机身线
            qicon_triangle(out, 6.f, 11.f, 24.f, 16.f, 6.f, 21.f);
            qicon_field wing;
            qicon_clear(&wing);
            qicon_segment(&wing, 6.f, 21.f, 18.f, 26.f, 2.4f);
            qicon_union(out, &wing);
            break;
        }
        case QICON_ATTACH:
        {
            // 回形针：两条同心圆弧用「圆环减圆环」近似
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
            // 齿轮：外环 + 内孔 + 6 个齿
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
            // 扳手：圆环（开口）+ 手柄
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
            // 双环交叠：两个圆环做交集，中间自然形成「透镜」
            qicon_ring(out, 12.5f, 16.f, 7.5f, 2.8f);
            qicon_field second;
            qicon_clear(&second);
            qicon_ring(&second, 19.5f, 16.f, 7.5f, 2.8f);
            qicon_smooth_union(out, &second, 2.f);
            break;
        }
        case QICON_PLAN:
        {
            // 清单：左边一列复选点 + 右边三条横线
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
                    // 第一行打勾
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
            // 盾牌：五边形 + 中间的勾
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
            // 机器人：圆头 + 方身 + 天线 + 眼睛
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
            // 时钟：环 + 两根指针
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
            // 闪电：多边形
            const float bolt[7 * 2] = {18.5f, 4.f, 10.5f, 18.f, 15.f, 18.f,
                                       13.5f, 28.f, 21.5f, 13.f, 17.f, 13.f};
            qicon_polygon(out, bolt, 7);
            break;
        }
        case QICON_BRAND:
        {
            // 黑乌鸦头（OpenButler / qk 的品牌标记）—— 与 SVG 资产
            // （packages/desktop/icons/raven.svg）同一造型：圆颅 + 厚重
            // 尖喙（带钩）+ 喉羽 + 挖空的眼睛。
            //
            // 组合方式与其它图标一致：原语并集成剪影，眼睛用差集挖空。
            // 喙是乌鸦最识别的特征，所以它被刻意做得厚而前伸。
            qicon_circle(out, 19.f, 13.2f, 7.6f);   // 颅

            qicon_field beak;                        // 喙（五边形，尖在左）
            qicon_clear(&beak);
            const float bk[5 * 2] = {13.2f, 10.2f, 2.6f, 13.3f, 1.0f, 14.2f,
                                     2.2f, 15.6f, 12.6f, 16.6f};
            qicon_polygon(&beak, bk, 5);
            qicon_union(out, &beak);

            qicon_field neck;                        // 后颈 + 喉羽（含一道须棘）
            qicon_clear(&neck);
            const float nk[9 * 2] = {24.5f, 18.5f, 22.5f, 25.0f, 16.0f, 27.6f,
                                     11.5f, 24.5f, 10.2f, 23.2f, 11.5f, 21.9f,
                                     13.2f, 18.6f, 13.0f, 17.6f, 12.4f, 16.6f};
            qicon_polygon(&neck, nk, 9);
            qicon_union(out, &neck);

            qicon_field eye;                         // 眼（挖空 → 任意底色都成立）
            qicon_clear(&eye);
            qicon_circle(&eye, 15.2f, 14.1f, 1.5f);
            qicon_subtract(out, &eye);
            break;
        }
        default:
            return 0;
    }
    return 1;
}

int32_t qicon_default_size(int32_t id)
{
    // 按图标语义返回合理默认尺寸（不再忽略 id 统一返回 20）。
    // 品牌/主图标偏大，状态指示偏小，其余常规；越界返回 0。
    switch (id)
    {
    case QICON_BRAND:   return 24;   // 品牌标记（乌鸦头）视觉重心更大
    case QICON_OK:
    case QICON_FAIL:
    case QICON_WARN:    return 16;   // 状态指示（勾/叉/叹号）通常小尺寸
    case QICON_SETTINGS:
    case QICON_SEARCH:  return 18;   // 工具栏图标略小
    default:
        return (id >= 0 && id < QICON_COUNT) ? 20 : 0;
    }
}

} // extern "C"
