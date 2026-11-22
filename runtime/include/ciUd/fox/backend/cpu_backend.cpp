#include "cpu_backend.hpp"

#include <chrono>
#include "text/font.hpp"

#include <cmath>
#include <cstdio>

namespace fox
{
    static inline float sd_round_box(float px, float py, float bx, float by, float rtl,
                                     float rtr, float rbr, float rbl)
    {
        float r = (px > 0.0f) ? ((py > 0.0f) ? rbr : rtr) : ((py > 0.0f) ? rbl : rtl);
        float qx = std::fabs(px) - bx + r;
        float qy = std::fabs(py) - by + r;
        float outside = std::sqrt(std::max(qx, 0.0f) * std::max(qx, 0.0f) +
                                  std::max(qy, 0.0f) * std::max(qy, 0.0f));
        return std::min(std::max(qx, qy), 0.0f) + outside - r;
    }

    static inline float sd_round_box_uniform(float px, float py, float bx, float by, float r)
    {
        r = std::fmin(r, std::fmin(bx, by));
        float qx = std::fabs(px) - bx + r;
        float qy = std::fabs(py) - by + r;
        float outside = std::sqrt(std::max(qx, 0.0f) * std::max(qx, 0.0f) +
                                  std::max(qy, 0.0f) * std::max(qy, 0.0f));
        return std::min(std::max(qx, qy), 0.0f) + outside - r;
    }

    static inline float smoothstep(float e0, float e1, float x)
    {
        const float t = std::fmin(std::fmax((x - e0) / (e1 - e0), 0.0f), 1.0f);
        return t * t * (3.0f - 2.0f * t);
    }

    static inline float sd_line_segment(float px, float py, float half_len, float half_thick, int cap_style)
    {
        float t = std::fmax(-half_len, std::fmin(half_len, px));
        float dx = px - t;
        float dy = py;
        float d = std::sqrt(dx * dx + dy * dy);

        if (cap_style == 1)
            return d - half_thick;
        else if (cap_style == 2)
        {
            float dx = std::fmax(std::fabs(px) - half_len, 0.0f);
            return std::fmax(dx, d) - half_thick;
        }

        float adx = std::fmax(std::fabs(px) - half_len, 0.0f);
        if (adx > 0.0f) return std::sqrt(adx * adx + d * d) - half_thick;
        return d - half_thick;
    }

    static inline float sd_edge(float px, float py, float x0, float y0, float x1, float y1, float s)
    {
        const float ex = x1 - x0, ey = y1 - y0;
        const float len = std::sqrt(ex * ex + ey * ey);
        if (len < 0.000001f) return -1.0e9f;
        return -s * (ex * (py - y0) - ey * (px - x0)) / len;
    }

    bool CpuBackend::init(int w, int h)
    {
        if (w <= 0 || h <= 0) return false;
        if (w > 16384 || h > 16384) return false;
        width_ = w;
        height_ = h;
        color_.assign((size_t)w * h * 4, 0);
        damage_.reset(w, h);
        tile_tmp_.resize(damage_.tile_count());
        return true;
    }

    bool CpuBackend::resize(int w, int h)
    {
        if (w <= 0 || h <= 0 || w > 16384 || h > 16384) return false;
        if (w == width_ && h == height_) return true;
        width_ = w;
        height_ = h;
        color_.assign((size_t)w * h * 4, 0);
        damage_.reset(w, h);
        tile_tmp_.resize(damage_.tile_count());
        return true;
    }

    void CpuBackend::shutdown()
    {
        color_.clear();
        color_.shrink_to_fit();
        instances_.clear();
        damage_.reset(0, 0);
        width_ = height_ = 0;
    }

    void CpuBackend::blend(int x, int y, f32 r, f32 g, f32 b, f32 a)
    {
        if (a <= 0.0f || x < 0 || y < 0 || x >= width_ || y >= height_) return;
        if (a > 1.0f) a = 1.0f;

        u8 *p = &color_[((size_t)y * width_ + x) * 4];
        const f32 inv = 1.0f - a;
        p[0] = (u8)(std::fmin(std::fmax(r * a + p[0] * (inv / 255.0f), 0.0f), 1.0f) * 255.0f + 0.5f);
        p[1] = (u8)(std::fmin(std::fmax(g * a + p[1] * (inv / 255.0f), 0.0f), 1.0f) * 255.0f + 0.5f);
        p[2] = (u8)(std::fmin(std::fmax(b * a + p[2] * (inv / 255.0f), 0.0f), 1.0f) * 255.0f + 0.5f);
        p[3] = (u8)(std::fmin(a + p[3] * inv, 1.0f) * 255.0f + 0.5f);
    }

    void CpuBackend::render(const DisplayList &dl, Context &ctx)
    {
        build_instances(ctx, dl, instances_);
        raster(instances_, ctx, ctx.scale());
    }

    void CpuBackend::raster(const InstanceSet &set, Context &ctx, f32 scale)
    {
        (void)scale;
        ++stats_.frames;
        stats_.hidden_instances += set.culled;

        const Color &bg = ctx.theme().window_bg;
        const Theme &th = ctx.theme();
        (void)th;

        const u64 bg_hash = pa::hash_pod(bg);
        const u64 frame_key = pa::mix64(bg_hash) ^ pa::mix64((u64)ctx.texture_generation()) ^
                              pa::mix64((u64)fonts().atlas_generation());

        const bool track = accel_;
        if (track) damage_.begin_frame(frame_key);

        struct View
        {
            const Batch *batch = nullptr;
            const InstanceData *d = nullptr;
            const GlyphAtlas *atlas = nullptr;
            const Context::Texture *tex = nullptr;
            pa::TileRect clip;
            u64 content = 0;
        };

        auto for_each_instance = [&](auto &&fn)
        {
            for (const Batch &batch : set.batches)
            {
                const GlyphAtlas *atlas = nullptr;
                const Context::Texture *tex = nullptr;
                if (batch.font != FONT_INVALID) atlas = fonts().atlas(batch.font);
                else if (batch.tex != TEX_NONE) tex = ctx.texture(batch.tex);
                if (batch.font != FONT_INVALID && !atlas) continue;
                if (batch.tex != TEX_NONE && !tex) continue;

                for (u32 k = 0; k < batch.count; ++k)
                {
                    const InstanceData &d = set.instances[batch.first + k];
                    if (d.rect[2] <= 0.0f || d.rect[3] <= 0.0f) continue;
                    pa::TileRect clip;
                    clip.x0 = (int)std::floor(d.rect[0]);
                    clip.y0 = (int)std::floor(d.rect[1]);
                    clip.x1 = (int)std::ceil(d.rect[0] + d.rect[2]);
                    clip.y1 = (int)std::ceil(d.rect[1] + d.rect[3]);
                    if (clip.x0 < 0) clip.x0 = 0;
                    if (clip.y0 < 0) clip.y0 = 0;
                    if (clip.x1 > width_) clip.x1 = width_;
                    if (clip.y1 > height_) clip.y1 = height_;
                    if (clip.empty()) continue;

                    if (batch.clip.w > 0.0f && batch.clip.h > 0.0f)
                    {
                        const int cx0 = (int)std::floor(batch.clip.x);
                        const int cy0 = (int)std::floor(batch.clip.y);
                        const int cx1 = (int)std::ceil(batch.clip.x + batch.clip.w);
                        const int cy1 = (int)std::ceil(batch.clip.y + batch.clip.h);
                        if (clip.x0 < cx0) clip.x0 = cx0;
                        if (clip.y0 < cy0) clip.y0 = cy0;
                        if (clip.x1 > cx1) clip.x1 = cx1;
                        if (clip.y1 > cy1) clip.y1 = cy1;
                        if (clip.empty()) continue;
                    }

                    View v;
                    v.batch = &batch;
                    v.d = &d;
                    v.atlas = atlas;
                    v.tex = tex;
                    v.clip = clip;
                    v.content = pa::mix64(pa::hash_pod(d) ^ ((u64)batch.font << 40) ^
                                          ((u64)batch.tex << 16));
                    fn(v);
                }
            }
        };

        using PaClock = std::chrono::steady_clock;
        const auto pa_ns = [](PaClock::time_point a, PaClock::time_point b)
        { return (u64)std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count(); };

        const auto t_track0 = PaClock::now();
        if (track)
        {
            for_each_instance([&](const View &v)
                              { damage_.touch(v.clip.x0, v.clip.y0, v.clip.x1, v.clip.y1,
                                              v.content); });
        }

        stats_.ns_track += pa_ns(t_track0, PaClock::now());

        const auto t_clear0 = PaClock::now();
        auto clear_tile = [&](const pa::TileRect &t)
        {
            for (int y = t.y0; y < t.y1; ++y)
            {
                pa::fill_row(&color_[(size_t)y * width_ * 4], t.x0, t.x1, bg.r, bg.g, bg.b, 255);
            }
        };
        if (track) damage_.for_each_dirty(clear_tile);
        else clear_tile(pa::TileRect{0, 0, width_, height_});
        stats_.ns_clear += pa_ns(t_clear0, PaClock::now());

        const auto t_draw0 = PaClock::now();
        for_each_instance(
            [&](const View &v)
            {
                const InstanceData &d = *v.d;
                const f32 rx = d.rect[0], ry = d.rect[1], rw = d.rect[2], rh = d.rect[3];
                const f32 ptype = d.params[0];
                const f32 half_w = rw * 0.5f, half_h = rh * 0.5f;
                const f32 cx = rx + half_w, cy = ry + half_h;
                const bool uniform_radii = std::fabs(d.radii[0] - d.radii[1]) < 0.001f &&
                                           std::fabs(d.radii[1] - d.radii[2]) < 0.001f;
                
                f32 img_lod = 0.0f;
                if (ptype == PT_IMAGE && v.tex && v.tex->mip_levels() > 1 && rw > 0.0f &&
                    rh > 0.0f)
                {
                    const f32 dx = std::fabs(d.color2[2] - d.color2[0]) * (f32)v.tex->w / rw;
                    const f32 dy = std::fabs(d.color2[3] - d.color2[1]) * (f32)v.tex->h / rh;
                    img_lod = img::mip_lod(std::fmax(dx, dy));
                }

                auto shade = [&](int px, int py)
                {
                    const f32 sx = (f32)px + 0.5f - cx;
                    const f32 sy = (f32)py + 0.5f - cy;
                    const f32 u = (sx + half_w) / rw;
                    const f32 vv = (sy + half_h) / rh;

                    if (ptype == PT_TEXT)
                    {
                        const f32 uv_x = d.params2[2] + (d.color2[0] - d.params2[2]) * u;
                        const f32 uv_y = d.params2[3] + (d.color2[1] - d.params2[3]) * vv;
                        if (!v.atlas) return;
                        const float cov = v.atlas->sample(uv_x, uv_y);
                        if (cov < 0.004f) return;
                        blend(px, py, d.color[0], d.color[1], d.color[2],
                              d.color[3] * cov);
                        return;
                    }

                    if (ptype == PT_IMAGE)
                    {
                        if (!v.tex) return;
                        const f32 uv_x = d.color2[0] + (d.color2[2] - d.color2[0]) * u;
                        const f32 uv_y = d.color2[1] + (d.color2[3] - d.color2[1]) * vv;
                        u8 s4[4];
                        ctx.sample_rgba_lod(v.batch->tex, uv_x, uv_y, img_lod, s4);
                        const u8 *s = s4;
                        if (!s) return;
                        const f32 sa = d.color[3] * (s[3] / 255.0f);
                        if (sa < 0.003f) return;
                        blend(px, py, d.color[0] * (s[0] / 255.0f),
                              d.color[1] * (s[1] / 255.0f), d.color[2] * (s[2] / 255.0f), sa);
                        return;
                    }

                    const f32 dist = uniform_radii
                                         ? sd_round_box_uniform(sx, sy, half_w, half_h,
                                                                d.radii[0])
                                         : sd_round_box(sx, sy, half_w, half_h, d.radii[0],
                                                        d.radii[1], d.radii[2],
                                                        d.radii[3]);

                    if (ptype == PT_SHADOW)
                    {
                        const f32 blur = std::fmax(d.params[2], 0.5f);
                        const f32 a = 1.0f - smoothstep(-blur, 0.0f, dist);
                        if (a < 0.003f) return;
                        blend(px, py, d.color[0], d.color[1], d.color[2], d.color[3] * a);
                        return;
                    }

                    if (ptype == PT_BORDER)
                    {
                        const f32 t = std::fmax(d.params[1], 0.5f);
                        const f32 ht = t * 0.5f;
                        const f32 a = 1.0f - smoothstep(ht - 0.5f, ht + 0.5f, std::fabs(dist));
                        if (a < 0.003f) return;
                        blend(px, py, d.color[0], d.color[1], d.color[2], d.color[3] * a);
                        return;
                    }

                    if (ptype == PT_GLASS)
                    {
                        if (!glass_) return;
                        u8 *p = &color_[((size_t)py * width_ + px) * 4];
                        const f32 back_r = p[0] / 255.0f, back_g = p[1] / 255.0f,
                                    back_b = p[2] / 255.0f;
                        const f32 sat = d.params2[0];
                        const f32 lum = back_r * 0.2126f + back_g * 0.7152f + back_b * 0.0722f;
                        const f32 vr = lum + (back_r - lum) * sat;
                        const f32 vg = lum + (back_g - lum) * sat;
                        const f32 vb = lum + (back_b - lum) * sat;
                        const f32 tint_amt = d.params[3];
                        const f32 a = 1.0f - smoothstep(-0.5f, 0.5f, dist);
                        const f32 rim = 1.0f - smoothstep(-2.5f, 0.0f, dist);
                        blend(px, py, vr + (d.color[0] - vr) * tint_amt + rim * 0.14f,
                              vg + (d.color[1] - vg) * tint_amt + rim * 0.14f,
                              vb + (d.color[2] - vb) * tint_amt + rim * 0.14f,
                              d.color[3] * a);
                        return;
                    }

                    if (ptype == PT_PATH)
                    {
                        const f32 dir_x = d.params[3], dir_y = d.params2[3];
                        const f32 lx = sx * dir_x + sy * dir_y;
                        const f32 ly = -sx * dir_y + sy * dir_x;
                        const f32 half_len = d.params[2] * 0.5f;
                        const f32 half_thick = d.params[1] * 0.5f;
                        const int cap_style = (int)d.params2[0];
                        float d_path = sd_line_segment(lx, ly, half_len, half_thick, cap_style);
                        const f32 a = 1.0f - smoothstep(-0.5f, 0.5f, d_path);
                        if (a < 0.003f) return;
                        blend(px, py, d.color[0], d.color[1], d.color[2], d.color[3] * a);
                        return;
                    }

                    if (ptype == PT_ELLIPSE)
                    {
                        const f32 rxx = std::fmax(half_w, 0.001f);
                        const f32 ryy = std::fmax(half_h, 0.001f);
                        const f32 qx = sx / rxx, qy = sy / ryy;
                        const f32 k1 = std::sqrt(qx * qx + qy * qy);
                        const f32 k2x = qx / rxx, k2y = qy / ryy;
                        const f32 k2 = std::sqrt(k2x * k2x + k2y * k2y);
                        const f32 dist_e = k2 > 0.000001f ? k1 * (k1 - 1.0f) / k2 : k1 - 1.0f;
                        const f32 a = 1.0f - smoothstep(-0.5f, 0.5f, dist_e);
                        if (a < 0.003f) return;
                        blend(px, py, d.color[0], d.color[1], d.color[2], d.color[3] * a);
                        return;
                    }

                    if (ptype == PT_TRIANGLE)
                    {
                        const f32 ax = d.params[1], ay = d.params[2];
                        const f32 bx = d.params[3], by = d.params2[0];
                        const f32 cxv = d.params2[1], cyv = d.params2[2];
                        const f32 orient = (bx - ax) * (cyv - ay) - (by - ay) * (cxv - ax);
                        const f32 s = orient > 0.0f ? 1.0f : -1.0f;
                        float dist_t = sd_edge(sx, sy, ax, ay, bx, by, s);
                        const float d2 = sd_edge(sx, sy, bx, by, cxv, cyv, s);
                        const float d3 = sd_edge(sx, sy, cxv, cyv, ax, ay, s);
                        if (d2 > dist_t) dist_t = d2;
                        if (d3 > dist_t) dist_t = d3;
                        const f32 a = 1.0f - smoothstep(-0.5f, 0.5f, dist_t);
                        if (a < 0.003f) return;
                        blend(px, py, d.color[0], d.color[1], d.color[2], d.color[3] * a);
                        return;
                    }

                    const f32 a = 1.0f - smoothstep(-0.5f, 0.5f, dist);
                    if (a < 0.003f) return;
                    f32 cr = d.color[0], cg = d.color[1], cb = d.color[2];
                    if (d.color2[3] > 0.0f)
                    {
                        f32 t = (sy + half_h) / (2.0f * half_h);
                        if (d.params2[1] > 0.5f) t = (sx + half_w) / (2.0f * half_w);
                        t = std::fmin(std::fmax(t, 0.0f), 1.0f);
                        cr = d.color[0] + (d.color2[0] - d.color[0]) * t;
                        cg = d.color[1] + (d.color2[1] - d.color[1]) * t;
                        cb = d.color[2] + (d.color2[2] - d.color[2]) * t;
                    }
                    blend(px, py, cr, cg, cb, d.color[3] * a);
                };

                const bool core_ok = (ptype == PT_RECT) && uniform_radii;
                const f32 core_r = d.radii[0];
                const bool want_gradient = d.color2[3] > 0.0f;
                const bool grad_h = d.params2[1] > 0.5f;

                pa::TileRect *tiles = tile_tmp_.data();
                int nt = 1;
                tiles[0] = v.clip;
                if (track)
                {
                    nt = damage_.dirty_tiles_for(v.clip.x0, v.clip.y0, v.clip.x1, v.clip.y1,
                                                 tiles, (int)tile_tmp_.size());
                    if (nt == 0)
                    {
                        ++stats_.instances_skipped;
                        return;
                    }
                }
                ++stats_.instances_drawn;

                u64 shaded_local = 0;
                u8 *const base = color_.data();
                auto draw_span = [&](int sx0, int sx1, int sy0, int sy1)
                {
                    for (int py = sy0; py < sy1; ++py)
                    {
                        u8 *row = base + (size_t)py * width_ * 4;
                        if (core_ok)
                        {
                            const pa::Span sp =
                                pa::core_span_row(cx, half_w, half_h, core_r, py, cy);
                            const int lo = sp.lo > sx0 ? sp.lo : sx0;
                            const int hi = sp.hi < sx1 ? sp.hi : sx1;
                            if (hi > lo)
                            {
                                for (int px = sx0; px < lo; ++px) shade(px, py);
                                const f32 alpha = d.color[3];
                                if (!want_gradient && alpha >= 1.0f)
                                {
                                    pa::fill_row(row, lo, hi,
                                                 (u8)(std::fmin(std::fmax(d.color[0], 0.0f), 1.0f) * 255.0f + 0.5f),
                                                 (u8)(std::fmin(std::fmax(d.color[1], 0.0f), 1.0f) * 255.0f + 0.5f),
                                                 (u8)(std::fmin(std::fmax(d.color[2], 0.0f), 1.0f) * 255.0f + 0.5f));
                                }
                                else if (!want_gradient)
                                {
                                    pa::blend_row(row, lo, hi, d.color[0], d.color[1], d.color[2], alpha);
                                }
                                else if (!grad_h)
                                {
                                    const f32 sy = (f32)py + 0.5f - cy;
                                    f32 tt = (sy + half_h) / (2.0f * half_h);
                                    tt = std::fmin(std::fmax(tt, 0.0f), 1.0f);
                                    const f32 cr = d.color[0] + (d.color2[0] - d.color[0]) * tt;
                                    const f32 cg = d.color[1] + (d.color2[1] - d.color[1]) * tt;
                                    const f32 cb = d.color[2] + (d.color2[2] - d.color[2]) * tt;
                                    if (alpha >= 1.0f)
                                        pa::fill_row(row, lo, hi,
                                                     (u8)(std::fmin(std::fmax(cr, 0.0f), 1.0f) * 255.0f + 0.5f),
                                                     (u8)(std::fmin(std::fmax(cg, 0.0f), 1.0f) * 255.0f + 0.5f),
                                                     (u8)(std::fmin(std::fmax(cb, 0.0f), 1.0f) * 255.0f + 0.5f));
                                    else
                                        pa::blend_row(row, lo, hi, cr, cg, cb, alpha);
                                }
                                else
                                {
                                    const f32 inv2h = 1.0f / (2.0f * half_w);
                                    auto col = [&](int px, f32 *out)
                                    {
                                        const f32 sx = (f32)px + 0.5f - cx;
                                        f32 tt = (sx + half_w) * inv2h;
                                        tt = std::fmin(std::fmax(tt, 0.0f), 1.0f);
                                        out[0] = d.color[0] + (d.color2[0] - d.color[0]) * tt;
                                        out[1] = d.color[1] + (d.color2[1] - d.color[1]) * tt;
                                        out[2] = d.color[2] + (d.color2[2] - d.color[2]) * tt;
                                    };
                                    if (alpha >= 1.0f)
                                        pa::fill_row_cb(row, lo, hi, col);
                                    else
                                        pa::blend_row_cb(row, lo, hi, alpha, col);
                                }
                                for (int px = hi; px < sx1; ++px) shade(px, py);
                                stats_.core_pixels += (u64)(hi - lo);
                                continue;
                            }
                        }
                        for (int px = sx0; px < sx1; ++px)
                        {
                            shade(px, py);
                            ++shaded_local;
                        }
                    }
                };

                for (int t = 0; t < nt;)
                {
                    const int band_y0 = tiles[t].y0;
                    const int band_y1 = tiles[t].y1;
                    int sx0 = tiles[t].x0;
                    int sx1 = tiles[t].x1;
                    int u = t + 1;
                    while (u < nt && tiles[u].y0 == band_y0 && tiles[u].y1 == band_y1 &&
                           tiles[u].x0 == sx1)
                    {
                        sx1 = tiles[u].x1;
                        ++u;
                    }
                    draw_span(sx0, sx1, band_y0, band_y1);
                    t = u;
                }
                stats_.shaded_pixels += shaded_local;
            });

        stats_.ns_draw += pa_ns(t_draw0, PaClock::now());

        if (track)
        {
            damage_.end_frame();
            stats_.pixels_skipped = damage_.skipped_area();
        }
    }


    size_t CpuBackend::non_background_pixels() const
    {
        if (color_.size() < 4) return 0;
        const u8 bg[3] = {color_[0], color_[1], color_[2]};
        size_t n = 0;
        for (size_t i = 0; i + 3 < color_.size(); i += 4)
            if (color_[i] != bg[0] || color_[i + 1] != bg[1] || color_[i + 2] != bg[2]) ++n;
        return n;
    }

    bool CpuBackend::write_ppm(const char *path) const
    {
        if (!path || color_.empty()) return false;

        FILE *f = std::fopen(path, "wb");
        if (!f) return false;
        fprintf(f, "P6\n%d %d\n255\n", width_, height_);
        for (size_t i = 0; i + 3 < color_.size(); i += 4)
        {
            fputc(color_[i], f);
            fputc(color_[i + 1], f);
            fputc(color_[i + 2], f);
        }
        fclose(f);
        return true;
    }
}