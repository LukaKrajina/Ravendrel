#include "qhal/BroUi.hpp"
#include "qhal/ClassicGui.hpp"
#include "gui/src/render_context.h"
#include "gui/src/nuklear_config.h"
#include "gui/src/font_loader.h"
#include "qhal/qicon.hpp"

#include <chrono>
#include <cstring>
#include <vector>
#include <cstdio>

namespace
{
    nk_context *ui_ctx()
    {
        auto &s = qhal::cgui_state();
        auto *rc = static_cast<qgui::RenderContext *>(s.impl);
        return rc ? rc->ctx() : nullptr;
    }

    nk_command_buffer *ui_canvas()
    {
        auto &s = qhal::cgui_state();
        auto *rc = static_cast<qgui::RenderContext *>(s.impl);
        if (!rc) return nullptr;
        nk_context *c = rc->ctx();
        if (!c) return nullptr;
        auto *win = nk_window_find(c, s.title);
        return win ? &win->buffer : nullptr;
    }

    const nk_user_font *ui_font(int size)
    {
        const nk_user_font *f = qgui::font_at(size);
        if (f) return f;
        f = qgui::font_at(qgui::FONT_BODY);
        if (f) return f;
        nk_context *c = ui_ctx();
        return c ? c->style.font : nullptr;
    }

    nk_color ui_rgba(int32_t color, nk_byte a)
    {
        return nk_rgba((nk_byte)((color >> 16) & 0xFF), (nk_byte)((color >> 8) & 0xFF),
                       (nk_byte)(color & 0xFF), a);
    }

    int utf8_len(unsigned char b)
    {
        if (b < 0x80) return 1;
        if ((b & 0xE0) == 0xC0) return 2;
        if ((b & 0xF0) == 0xE0) return 3;
        if ((b & 0xF8) == 0xF0) return 4;
        return 1;
    }

    float measure(const nk_user_font *f, const char *s, int len)
    {
        if (!f || !s || len <= 0) return 0.f;
        float w = 0.f;
        for (int i = 0; i < len;)
        {
            int cl = utf8_len((unsigned char)s[i]);
            if (i + cl > len) cl = 1;
            w += f->width(f->userdata, f->height, s + i, cl);
            i += cl;
        }
        return w;
    }

    struct Line {
        int off;
        int len;
    };

    int wrap(const nk_user_font *f, const char *s, int len, float maxw, std::vector<Line> &out)
    {
        out.clear();
        if (!f || !s || len <= 0) return 0;
        int start = 0;
        int i = 0;
        float cur = 0.f;
        while (i < len)
        {
            unsigned char ch = (unsigned char)s[i];
            if (ch == '\n' ||
                (ch == '\\' && i + 1 < len && s[i + 1] == 'n'))
            {
                out.push_back({start, i - start});
                i += (ch == '\n') ? 1 : 2;
                start = i;
                cur = 0.f;
                continue;
            }
            int cl = utf8_len(ch);
            if (i + cl > len) cl = 1;
            float cw = f->width(f->userdata, f->height, s + i, cl);
            if (cur > 0.f && cur + cw > maxw)
            {
                out.push_back({start, i - start});
                start = i;
                cur = 0.f;
                continue;
            }
            cur += cw;
            i += cl;
        }
        out.push_back({start, len - start});
        return (int)out.size();
    }

    struct Timing {
        std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
        std::chrono::steady_clock::time_point prev{};
        double dt_ms = 0.0;
        double time_ms = 0.0;
        int64_t frame = 0;
        bool first = true;

        void tick()
        {
            auto now = std::chrono::steady_clock::now();
            if (first) { prev = now; first = false; }
            dt_ms = std::chrono::duration<double, std::milli>(now - prev).count();
            prev = now;
            time_ms = std::chrono::duration<double, std::milli>(now - t0).count();
            frame += 1;
        }
    };

    Timing &timing()
    {
        static Timing t;
        return t;
    }

    struct Edges {
        unsigned char keys[NK_KEY_MAX] = {0};
        unsigned char mouse = 0;
    };

    Edges &edges()
    {
        static Edges e;
        return e;
    }

    int mouse_down()
    {
        nk_context *c = ui_ctx();
        if (!c) return 0;
        return c->input.mouse.buttons[NK_BUTTON_LEFT].down ? 1 : 0;
    }

    constexpr int kClipStackMax = 8;

    struct ClipEntry {
        float x, y, w, h;
    };

    ClipEntry &clip_stack(int i)
    {
        static ClipEntry s[kClipStackMax];
        return s[i];
    }

    int &clip_depth()
    {
        static int d = 0;
        return d;
    }

    void clip_reset()
    {
        clip_depth() = 0;
    }

    ClipEntry clip_current()
    {
        auto &g = qhal::cgui_state();
        if (clip_depth() > 0) return clip_stack(clip_depth() - 1);
        return ClipEntry{0.f, 0.f, (float)g.width, (float)g.height};
    }

    qicon_field &icon_field()
    {
        static qicon_field f;
        return f;
    }
}

extern "C"
{


int32_t rvdl_ui_begin(void)
{
    timing().tick();
    clip_reset();
    return (int32_t)timing().frame;
}

int32_t rvdl_ui_frame(void) { return (int32_t)timing().frame; }
double rvdl_ui_dt_ms(void) { return timing().dt_ms; }
double rvdl_ui_time_ms(void) { return timing().time_ms; }


int32_t rvdl_ui_open(void)
{
    auto &s = qhal::cgui_state();
    auto *rc = static_cast<qgui::RenderContext *>(s.impl);
    return (rc && rc->window()) ? 1 : 0;
}

int32_t rvdl_ui_win_w(void) { return rvdl_cgui_width(); }
int32_t rvdl_ui_win_h(void) { return rvdl_cgui_height(); }


int32_t rvdl_ui_text(int32_t x, int32_t y, int32_t size, int32_t color, const char *text)
{
    auto *cb = ui_canvas();
    auto *f = ui_font(size);
    if (!cb || !f || !text || !*text) return 0;
    const int len = (int)std::strlen(text);
    const float w = measure(f, text, len);
    nk_draw_text(cb, nk_rect((float)x, (float)y, w + 2.f, f->height), text, len, f,
                 nk_rgba(0, 0, 0, 0), ui_rgba(color, 255));
    return 1;
}

int32_t rvdl_ui_textw(int32_t x, int32_t y, int32_t w, int32_t line_height, int32_t size,
                    int32_t color, const char *text)
{
    if (ui_canvas() == nullptr) return -1;
    if (ui_font(size) == nullptr) return -2;
    if (text == nullptr) return -3;
    if (*text == '\0') return -4;
    if (w <= 0) return -5;

    auto *cb = ui_canvas();
    auto *f = ui_font(size);
    const int len = (int)std::strlen(text);
    std::vector<Line> lines;
    const int n = wrap(f, text, len, (float)w, lines);
    const float lh = line_height > 0 ? (float)line_height : f->height;
    for (int i = 0; i < n; ++i)
    {
        if (lines[i].len <= 0) continue;
        nk_draw_text(cb, nk_rect((float)x, (float)(y + i * (int)lh), (float)w, f->height),
                     text + lines[i].off, lines[i].len, f,
                     nk_rgba(0, 0, 0, 0), ui_rgba(color, 255));
    }
    return n;
}

int32_t rvdl_ui_text_lines(int32_t w, int32_t size, const char *text)
{
    auto *f = ui_font(size);
    if (!f || !text || !*text || w <= 0) return 0;
    std::vector<Line> lines;
    return wrap(f, text, (int)std::strlen(text), (float)w, lines);
}

int32_t rvdl_ui_text_width(int32_t size, const char *text)
{
    auto *f = ui_font(size);
    if (!f || !text) return 0;
    return (int32_t)measure(f, text, (int)std::strlen(text));
}

int32_t rvdl_ui_text_height(int32_t size)
{
    auto *f = ui_font(size);
    return f ? (int32_t)f->height : 16;
}

int32_t rvdl_ui_font_count(void)
{
    return qgui::font_count();
}

int32_t rvdl_ui_font_px(int32_t size)
{
    return (int32_t)qgui::font_px(size);
}

int32_t rvdl_ui_atlas_info(void)
{
    return qgui::atlas_info();
}


int32_t rvdl_ui_rect_a(int32_t x, int32_t y, int32_t w, int32_t h, int32_t color, int32_t alpha)
{
    auto *cb = ui_canvas();
    if (!cb || w <= 0 || h <= 0) return 0;
    nk_byte a = (nk_byte)(alpha < 0 ? 0 : (alpha > 255 ? 255 : alpha));
    nk_fill_rect(cb, nk_rect((float)x, (float)y, (float)w, (float)h), 0.f, ui_rgba(color, a));
    return 1;
}

int32_t rvdl_ui_clip(int32_t x, int32_t y, int32_t w, int32_t h)
{
    auto *cb = ui_canvas();
    if (!cb || w <= 0 || h <= 0) return 0;
    if (clip_depth() < kClipStackMax)
    {
        clip_stack(clip_depth()) = clip_current();
        clip_depth() = clip_depth() + 1;
    }
    nk_push_scissor(cb, nk_rect((float)x, (float)y, (float)w, (float)h));
    return 1;
}

int32_t rvdl_ui_clip_end(void)
{
    auto *cb = ui_canvas();
    if (!cb) return 0;
    if (clip_depth() > 0) clip_depth() = clip_depth() - 1;
    const ClipEntry r = clip_current();
    nk_push_scissor(cb, nk_rect(r.x, r.y, r.w, r.h));
    return 1;
}


int32_t rvdl_ui_key_down(int32_t key)
{
    nk_context *c = ui_ctx();
    if (!c || key < 0 || key >= NK_KEY_MAX) return 0;
    return nk_input_is_key_down(&c->input, (enum nk_keys)key) ? 1 : 0;
}

int32_t rvdl_ui_key_press(int32_t key)
{
    if (key < 0 || key >= NK_KEY_MAX) return 0;
    auto &e = edges();
    const int down = rvdl_ui_key_down(key);
    const int was = e.keys[key] ? 1 : 0;
    e.keys[key] = (unsigned char)(down ? 1 : 0);
    return (down && !was) ? 1 : 0;
}

int32_t rvdl_ui_key_sync(void)
{
    nk_context *c = ui_ctx();
    if (!c) return 0;
    auto &e = edges();
    for (int k = 0; k < NK_KEY_MAX; ++k)
        e.keys[k] = (unsigned char)(nk_input_is_key_down(&c->input, (enum nk_keys)k) ? 1 : 0);
    return 1;
}

int32_t rvdl_ui_char(void)
{
    static int cursor = 0;
    static int64_t seen = -1;
    const int64_t f = timing().frame;
    if (seen != f) { seen = f; cursor = 0; }

    nk_context *c = ui_ctx();
    if (!c) return -1;
    auto &kb = c->input.keyboard;
    if (cursor >= kb.text_len) return -1;

    const unsigned char b0 = (unsigned char)kb.text[cursor];
    int cl = utf8_len(b0);
    if (cursor + cl > kb.text_len) cl = 1;
    nk_rune rune = 0;
    switch (cl)
    {
        case 2:
            rune = (nk_rune)(((b0 & 0x1Fu) << 6) |
                             ((unsigned char)kb.text[cursor + 1] & 0x3Fu));
            break;
        case 3:
            rune = (nk_rune)(((b0 & 0x0Fu) << 12) |
                             (((unsigned char)kb.text[cursor + 1] & 0x3Fu) << 6) |
                             ((unsigned char)kb.text[cursor + 2] & 0x3Fu));
            break;
        case 4:
            rune = (nk_rune)(((b0 & 0x07u) << 18) |
                             (((unsigned char)kb.text[cursor + 1] & 0x3Fu) << 12) |
                             (((unsigned char)kb.text[cursor + 2] & 0x3Fu) << 6) |
                             ((unsigned char)kb.text[cursor + 3] & 0x3Fu));
            break;
        default:
            rune = b0;
            break;
    }
    cursor += cl;
    return (int32_t)rune;
}

int32_t rvdl_ui_char_pending(void)
{
    nk_context *c = ui_ctx();
    return c ? (int32_t)c->input.keyboard.text_len : 0;
}

int32_t rvdl_ui_scroll(void)
{
    nk_context *c = ui_ctx();
    if (!c) return 0;
    auto &sd = c->input.mouse.scroll_delta;
    const float d = (sd.x != 0.f) ? sd.x : sd.y;
    sd.x = 0.f;
    sd.y = 0.f;
    return (int32_t)(-d);
}

int32_t rvdl_ui_mouse_x(void)
{
    nk_context *c = ui_ctx();
    return c ? (int32_t)c->input.mouse.pos.x : 0;
}

int32_t rvdl_ui_mouse_y(void)
{
    nk_context *c = ui_ctx();
    return c ? (int32_t)c->input.mouse.pos.y : 0;
}

int32_t rvdl_ui_mouse_down(void) { return mouse_down(); }

int32_t rvdl_ui_mouse_press(void)
{
    auto &e = edges();
    const int down = mouse_down();
    const int was = e.mouse ? 1 : 0;
    e.mouse = (unsigned char)(down ? 1 : 0);
    return (down && !was) ? 1 : 0;
}


int32_t rvdl_ui_probe(void)
{
    int32_t r = 0;
    if (ui_canvas()) r |= 1;
    auto *f = ui_font(qgui::FONT_BODY);
    if (f)
    {
        r |= 2;
        r |= ((int32_t)f->height & 0xFF) << 8;
    }
    r |= (qgui::font_count() & 0xF) << 16;
    return r;
}


int32_t rvdl_icon_build(int32_t id)
{
    return qicon_build(id, &icon_field());
}

int32_t rvdl_icon_draw(int32_t x, int32_t y, int32_t w, int32_t h, int32_t color, float alpha_scale)
{
    auto *cb = ui_canvas();
    if (!cb) return 0;
    return qicon_draw(cb, (float)x, (float)y, (float)w, (float)h, &icon_field(), color, alpha_scale);
}

int32_t rvdl_icon_draw_count(void) { return qicon_draw_count(&icon_field()); }

int32_t rvdl_icon_area(void)
{
    int32_t n = 0;
    const qicon_field &f = icon_field();
    for (int i = 0; i < QICON_CELLS; ++i)
        if (f.d[i] < 0.f) n++;
    return n;
}

int32_t rvdl_icon_size(int32_t id) { return qicon_default_size(id); }


static qicon_field &icon_scratch()
{
    static qicon_field f;
    return f;
}

void rvdl_icon_reset(void)
{
    qicon_clear(&icon_field());
    qicon_clear(&icon_scratch());
}

void rvdl_icon_fill(float v) { qicon_fill(&icon_field(), v); }

int32_t rvdl_icon_shape_rect(float x, float y, float w, float h, float r)
{
    return qicon_rect(&icon_scratch(), x, y, w, h, r);
}

int32_t rvdl_icon_shape_circle(float cx, float cy, float r)
{
    return qicon_circle(&icon_scratch(), cx, cy, r);
}

int32_t rvdl_icon_shape_ring(float cx, float cy, float r, float thickness)
{
    return qicon_ring(&icon_scratch(), cx, cy, r, thickness);
}

int32_t rvdl_icon_shape_segment(float x0, float y0, float x1, float y1, float thickness)
{
    return qicon_segment(&icon_scratch(), x0, y0, x1, y1, thickness);
}

int32_t rvdl_icon_shape_triangle(float x0, float y0, float x1, float y1, float x2, float y2)
{
    return qicon_triangle(&icon_scratch(), x0, y0, x1, y1, x2, y2);
}

int32_t rvdl_icon_shape_star(float cx, float cy, float r_outer, float r_inner, int32_t points,
                           float rot)
{
    return qicon_star(&icon_scratch(), cx, cy, r_outer, r_inner, points, rot);
}

int32_t rvdl_icon_mix(int32_t op)
{
    qicon_field *a = &icon_field();
    qicon_field *b = &icon_scratch();
    switch (op)
    {
        case QICON_OP_SUBTRACT: qicon_subtract(a, b); break;
        case QICON_OP_INTERSECT: qicon_intersect(a, b); break;
        case QICON_OP_SMOOTH: qicon_smooth_union(a, b, 2.f); break;
        default: qicon_union(a, b); break;
    }
    qicon_clear(&icon_scratch());
    return rvdl_icon_area();
}

int32_t rvdl_icon_rotate(float cx, float cy, float angle)
{
    qicon_field dst;
    if (!qicon_rotate(&dst, &icon_field(), cx, cy, angle)) return 0;
    std::memcpy(icon_field().d, dst.d, sizeof(dst.d));
    return 1;
}

int32_t rvdl_icon_scale(float cx, float cy, float sx, float sy)
{
    qicon_field dst;
    if (!qicon_scale(&dst, &icon_field(), cx, cy, sx, sy)) return 0;
    std::memcpy(icon_field().d, dst.d, sizeof(dst.d));
    return 1;
}

int32_t rvdl_icon_tint(int32_t rgb, float hue_deg, float lightness, float contrast)
{
    return qicon_tint(rgb, hue_deg, lightness, contrast);
}

}