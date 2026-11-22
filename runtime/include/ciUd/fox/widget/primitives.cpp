#include "primitives.hpp"
#include "core/anim.hpp"
#include "text/soft_wrap.hpp"
#include <algorithm>
#include <cstring>
#include <cstdio>
#include <cmath>
#include <unordered_map>
#include <vector>

namespace fox
{

    static inline Id widget_id(const Context &c, Id salt, const Rect &r)
    {
        return c.make_id(salt, (Id)id_hash_rect(r));
    }


    static inline int encode_utf8(u32 cp, char *out)
    {
        if (cp > 0x10FFFFu) return 0;
        if (cp >= 0xD800u && cp <= 0xDFFFu) return 0;

        int n = 0;
        if (cp < 0x80)
        {
            out[n++] = (char)cp;
        }
        else if (cp < 0x800)
        {
            out[n++] = (char)(0xC0 | (cp >> 6));
            out[n++] = (char)(0x80 | (cp & 0x3F));
        }
        else if (cp < 0x10000)
        {
            out[n++] = (char)(0xE0 | (cp >> 12));
            out[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            out[n++] = (char)(0x80 | (cp & 0x3F));
        }
        else
        {
            out[n++] = (char)(0xF0 | (cp >> 18));
            out[n++] = (char)(0x80 | ((cp >> 12) & 0x3F));
            out[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            out[n++] = (char)(0x80 | (cp & 0x3F));
        }
        return n;
    }

    static inline void caret_restart(Context &c, WidgetState &st)
    {
        st.caret = (i32)c.time();
        c.request_redraw();
    }

    static inline bool caret_visible(const Context &c, const WidgetState &st)
    {
        const f32 since = c.time() - (f32)st.caret;
        if (since < 0.0f) return true;
        return fmod(since, 1.0f) < 0.55f;
    }

    static inline Key ascii_key(char ch) { return (Key)(u16)(unsigned char)ch; }


    f32 label(Context &c, const Rect &r, const char *text, Color color, Align align)
    {
        if (!text) return 0.0f;
        Theme &th = c.theme();
        FontId f = c.font(th.font_body);
        f32 w = text_width(f, text);

        f32 tx = r.x;
        if (align == Align::Center) tx = r.x + (r.w - w) * 0.5f;
        else if (align == Align::End) tx = r.x + r.w - w;

        f32 lh = fonts().line_height(f);
        f32 ty = r.y + (r.h - lh) * 0.5f;

        u32 gs = 0;
        shape_text(c.dl(), f, text, {tx, ty + fonts().face(f)->ascent}, color, 0.0f, gs);
        return w;
    }

    f32 label_layout(Context &c, FontId font, const char *text, Color color, f32 height)
    {
        if (height <= 0.0f) height = fonts().line_height(font);
        Rect r = c.layout().allocate(Vec2{0.0f, height}, Align::Stretch);
        return label(c, r, text, color, Align::Start);
    }


    bool image(Context &c, const Rect &r, TexId tex, Color tint)
    {
        const Context::Texture *t = c.texture(tex);
        if (!t || r.w <= 0.0f || r.h <= 0.0f) return false;
        c.dl().push_image(r, tex, tint);
        return true;
    }

    bool image_region(Context &c, const Rect &r, TexId tex, const Rect &src, Color tint)
    {
        const Context::Texture *t = c.texture(tex);
        if (!t || r.w <= 0.0f || r.h <= 0.0f) return false;
        Rect s{src.x, src.y, src.w, src.h};
        s.x = s.x < 0.0f ? 0.0f : (s.x > 1.0f ? 1.0f : s.x);
        s.y = s.y < 0.0f ? 0.0f : (s.y > 1.0f ? 1.0f : s.y);
        if (s.w < 0.0f) s.w = 0.0f;
        if (s.h < 0.0f) s.h = 0.0f;
        if (s.x + s.w > 1.0f) s.w = 1.0f - s.x;
        if (s.y + s.h > 1.0f) s.h = 1.0f - s.y;
        if (s.w <= 0.0f || s.h <= 0.0f) return false;
        c.dl().push_image(r, tex, tint, s);
        return true;
    }


    void panel(Context &c, const Rect &r, Color fill, float radius, bool glass)
    {
        CornerRadii rad(radius);
        if (glass)
            c.dl().push_glass(r, rad, theme().glass_tint, theme().glass_blur, theme().glass_opacity,
                              theme().glass_saturation, -1.0f);
        else
            c.dl().push_rounded(r, fill, rad, -1.0f);
    }

    void glass_bar(Context &c, const Rect &r, float radius)
    {
        Theme &th = theme();
        CornerRadii rad(radius);
        c.dl().push_glass(r, rad, th.glass_tint, th.glass_blur, th.glass_opacity,
                          th.glass_saturation, -2.0f);
    }

    void separator(Context &c, const Rect &r, bool horizontal)
    {
        Rect rr = horizontal ? Rect{r.x, r.y + r.h * 0.5f, r.w, theme().hairline}
                             : Rect{r.x + r.w * 0.5f, r.y, theme().hairline, r.h};
        c.dl().push(rr, theme().separator, -0.5f);
    }


    void shadow_rect(Context &c, const Rect &r, const CornerRadii &radii, const Shadow &sh, f32 z)
    {
        if (sh.blur <= 0.0f || sh.color.a == 0) return;
        Cmd cmd;
        cmd.type = CmdType::Rect;
        cmd.rect = r;
        cmd.radii = radii;
        cmd.shadow = sh;
        cmd.color = Color{0, 0, 0, 0};
        cmd.z = z;
        c.dl().push(cmd);
    }

    bool button(Context &c, const Rect &r, const char *text, ButtonStyle style, bool enabled)
    {
        Theme &th = c.theme();
        Id id = widget_id(c, 0xB0000001u, r);
        WidgetState &st = c.state_touch(id);

        bool over = enabled && c.hover(r, id);
        if (over) c.request_redraw();

        f32 target_hover = over ? 1.0f : 0.0f;
        f32 target_active = (over && c.input().buttons[(int)MouseButton::Left].down) ? 1.0f : 0.0f;
        st.hover = approach(st.hover, target_hover, c.anim_dt(), th.anim_fast);
        st.active = approach(st.active, target_active, c.anim_dt(), th.anim_fast * 0.6f);

        Color base, hover_c, press_c, fg;
        switch (style)
        {
        case ButtonStyle::Filled:
            base = th.accent_color;
            hover_c = th.accent_hover;
            press_c = th.accent_active;
            fg = th.text_on_accent;
            break;
        case ButtonStyle::Tinted:
            base = Color::mix(th.accent_color, th.window_bg, 0.85f);
            hover_c = Color::mix(th.accent_color, th.window_bg, 0.7f);
            press_c = Color::mix(th.accent_color, th.window_bg, 0.55f);
            fg = th.accent_color;
            break;
        case ButtonStyle::Bezel:
            base = th.control_bg;
            hover_c = th.control_bg_hover;
            press_c = th.control_bg_active;
            fg = th.text;
            break;
        default:
            base = Color{0, 0, 0, 0};
            hover_c = th.control_bg_hover;
            press_c = th.control_bg_active;
            fg = th.accent_color;
            break;
        }
        if (!enabled)
        {
            base = th.control_bg_disabled;
            fg = th.text_tertiary;
            hover_c = base;
            press_c = base;
        }

        Color fill = st.active > 0.01f ? Color::mix(hover_c, press_c, st.active)
                                       : Color::mix(base, hover_c, st.hover);
        CornerRadii rad(th.corner_control);

        if (style == ButtonStyle::Filled || style == ButtonStyle::Bezel)
        {
            Shadow sh;
            sh.offset_y = 1.0f;
            sh.blur = 3.0f;
            sh.color = Color{0, 0, 0, (u8)(0.30f * (1.0f - st.active * 0.5f))};
            Cmd shadow_cmd;
            shadow_cmd.type = CmdType::Rect;
            shadow_cmd.rect = r.translated({0.0f, sh.offset_y});
            shadow_cmd.radii = rad;
            shadow_cmd.shadow = sh;
            shadow_cmd.color = sh.color;
            shadow_cmd.z = -1.1f;
            c.dl().push(shadow_cmd);
        }

        c.dl().push_rounded(r, fill, rad, -1.0f);

        if (style != ButtonStyle::Plain)
            c.dl().push_border(r, th.control_border, th.hairline, rad, -0.9f);

        label(c, r, text ? text : "", fg, Align::Center);

        bool clicked = false;
        if (enabled && over)
        {
            if (c.input().buttons[(int)MouseButton::Left].pressed)
            {
                st.pressed = true;
                clicked = false;
            }
            if (st.pressed && c.input().buttons[(int)MouseButton::Left].released)
            {
                st.pressed = false;
                clicked = true;
            }
        }
        else
        {
            st.pressed = false;
        }
        return clicked;
    }


    bool toggle_switch(Context &c, const Rect &r, bool &value)
    {
        Theme &th = c.theme();
        Id id = widget_id(c, 0xB0000002u, r);
        WidgetState &st = c.state_touch(id);
        bool over = c.hover(r, id);

        f32 target = value ? 1.0f : 0.0f;
        st.anim = approach(st.anim, target, c.anim_dt(), th.anim_fast * 1.2f);
        if (fx_abs(st.anim - target) > 0.001f) c.request_redraw();

        f32 track_w = r.w;
        f32 track_h = fx_min(r.h, track_w * 0.5f);
        Rect track{r.x, r.y + (r.h - track_h) * 0.5f, track_w, track_h};
        CornerRadii kr(track_h * 0.5f);

        Color off = th.control_bg;
        Color on = th.accent_color;
        Color fill = Color::mix(off, on, st.anim);
        c.dl().push_rounded(track, fill, kr, 0.0f);

        f32 knob_pad = 2.0f;
        f32 knob_d = track_h - knob_pad * 2.0f;
        f32 knob_x = track.x + knob_pad + st.anim * (track.w - knob_d - knob_pad * 2.0f);
        Rect knob{knob_x, track.y + knob_pad, knob_d, knob_d};

        Shadow sh;
        sh.offset_y = 1.0f;
        sh.blur = 2.0f;
        sh.color = Color{0, 0, 0, 0x40};
        shadow_rect(c, knob.translated({0, 1}), CornerRadii(knob_d * 0.5f), sh, 0.1f);

        c.dl().push_rounded(knob, Color{255, 255, 255, 255}, CornerRadii(knob_d * 0.5f), 0.2f);

        bool clicked = false;
        if (over && c.input().buttons[(int)MouseButton::Left].pressed)
        {
            value = !value;
            clicked = true;
        }
        return clicked;
    }

    bool checkbox(Context &c, const Rect &r, const char *label_text, bool &value)
    {
        Theme &th = c.theme();
        Id id = widget_id(c, 0xB0000003u, r);
        WidgetState &st = c.state_touch(id);

        f32 box = fx_min(r.h, 16.0f);
        Rect box_r{r.x, r.y + (r.h - box) * 0.5f, box, box};
        Rect full{r.x, r.y, r.w, r.h};
        bool over = c.hover(full, id);

        st.hover = approach(st.hover, over ? 1.0f : 0.0f, c.anim_dt(), th.anim_fast);

        CornerRadii br(4.0f);
        Color box_fill = value ? th.accent_color : th.control_bg;
        c.dl().push_rounded(box_r, box_fill, br, 0.0f);
        c.dl().push_border(box_r, value ? th.accent_active : th.control_border, th.hairline, br,
                           0.1f);

        if (value)
        {
            const f32 bx = box_r.x, by = box_r.y;
            const f32 cx = bx + box * 0.5f, cy = by + box * 0.5f;
            const f32 t = std::fmax(1.6f, box * 0.13f);

            const Vec2 pts[3] = {
                {cx - box * 0.30f, cy + box * 0.02f},
                {cx - box * 0.08f, cy + box * 0.24f},
                {cx + box * 0.32f, cy - box * 0.26f},
            };
            c.dl().push_path(pts, 3, t, Color{255, 255, 255, 255}, 1, 1, 0, 0.2f);
        }

        if (label_text && r.w > box)
        {
            Rect lr{box_r.right() + th.spacing_sm, r.y, r.w - box - th.spacing_sm, r.h};
            label(c, lr, label_text, over ? th.text : th.text_secondary, Align::Start);
        }

        bool clicked = false;
        if (over && c.input().buttons[(int)MouseButton::Left].pressed)
        {
            value = !value;
            clicked = true;
        }
        return clicked;
    }


    bool slider(Context &c, const Rect &r, float &value, float min, float max, SliderStyle style,
                float step)
    {
        Theme &th = c.theme();
        Id id = widget_id(c, 0xB0000004u, r);
        WidgetState &st = c.state_touch(id);

        bool over = c.hover(r, id);
        bool dragging = st.pressed;

        f32 track_h = 4.0f;
        Rect track{r.x, r.y + (r.h - track_h) * 0.5f, r.w, track_h};
        CornerRadii tr(track_h * 0.5f);
        c.dl().push_rounded(track, th.control_bg, tr, 0.0f);

        if (max <= min) max = min + 1.0f;
        float t = (value - min) / (max - min);
        t = fx_clamp(t, 0.0f, 1.0f);

        Rect fill{track.x, track.y, track.w * t, track_h};
        c.dl().push_rounded(fill, th.accent_color, tr, 0.05f);

        f32 knob_d = over || dragging ? 16.0f : 14.0f;
        f32 kx = track.x + t * (track.w - knob_d);
        Rect knob{kx, r.y + (r.h - knob_d) * 0.5f, knob_d, knob_d};
        Shadow sh;
        sh.offset_y = 1.0f;
        sh.blur = 3.0f;
        sh.color = Color{0, 0, 0, 0x50};
        shadow_rect(c, knob.translated({0, 1}), CornerRadii(knob_d * 0.5f), sh, 0.1f);
        c.dl().push_rounded(knob, Color{255, 255, 255, 255}, CornerRadii(knob_d * 0.5f), 0.2f);
        c.dl().push_border(knob, th.control_border, th.hairline, CornerRadii(knob_d * 0.5f), 0.21f);

        if (over && c.input().buttons[(int)MouseButton::Left].pressed)
        {
            st.pressed = true;
            dragging = true;
        }
        if (st.pressed && !c.input().buttons[(int)MouseButton::Left].down)
        {
            st.pressed = false;
            dragging = false;
        }

        if (dragging && over)
        {
            float nt = fx_clamp((c.input().mouse_pos.x - track.x) / fx_max(track.w, 1.0f), 0.0f,
                                1.0f);
            float nv = min + nt * (max - min);
            if (style == SliderStyle::Stepped && step > 0.0f)
                nv = std::round(nv / step) * step;
            value = fx_clamp(nv, min, max);
        }
        return dragging;
    }


    bool text_field(Context &c, const Rect &r, char *buffer, int capacity,
                    const char *placeholder, Id id_hint)
    {
        Theme &th = c.theme();
        Id id = id_hint ? id_hint : widget_id(c, 0xB0000005u, r);
        WidgetState &st = c.state_touch(id);

        bool over = c.hover(r, id);
        bool focused = c.focus().has_focus(id);
        if (focused) st.focused = true;

        CornerRadii rad(th.corner_field);
        Color fill = focused ? th.control_bg_active : th.control_bg;
        c.dl().push_rounded(r, fill, rad, 0.0f);
        f32 ring_w = focused ? 2.0f : th.hairline;
        Color ring = focused ? th.accent_color : (over ? th.control_border : th.separator);
        c.dl().push_border(r, ring, ring_w, rad, 0.1f);

        FontId f = c.font(th.font_body);
        f32 lh = fonts().line_height(f);
        f32 ascent = fonts().face(f)->ascent;
        Vec2 pen{r.x + th.spacing_sm, r.y + (r.h - lh) * 0.5f + ascent};

        if (buffer && capacity > 0)
        {
            u32 gs = 0;
            TextBuffer &tb = c.text_edit(id);

            c.sync_text_from_host(tb, buffer, strlen(buffer));

            const f32 avail = r.w - th.spacing_sm * 2.0f;
            const std::string &s = tb.text();

            f32 preedit_x_ = 0.0f;
            bool preedit_drawn_ = false;

            bool blink_reset = false;

            if (over && c.input().buttons[(int)MouseButton::Left].pressed)
            {
                c.focus().set_focus(id);
                focused = true;
                const size_t hit = text_hit_index(f, s.c_str(), c.input().mouse_pos.x - pen.x);
                if (c.input().buttons[(int)MouseButton::Left].double_click)
                {
                    auto cls = [](char ch) {
                        return (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r') ? 0
                             : ((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'z') ||
                                (ch >= 'A' && ch <= 'Z'))
                                   ? 1
                                   : 2;
                    };
                    size_t a = hit, b = hit;
                    while (a > 0 && cls(s[a - 1]) == cls(s[hit])) --a;
                    while (b < s.size() && cls(s[b]) == cls(s[hit])) ++b;
                    tb.set_caret(a);
                    tb.set_anchor(b);
                }
                else
                {
                    tb.set_caret(hit);
                }
                blink_reset = true;
            }

            if (over && c.input().buttons[(int)MouseButton::Left].down && focused)
                tb.select_to(text_hit_index(f, s.c_str(), c.input().mouse_pos.x - pen.x));

            if (focused)
            {
                const bool shift = c.input().shift;
                const bool ctrl = c.input().ctrl;
                bool nav = false;

                if (c.input().key_press(Key::Left))
                {
                    if (ctrl)
                        tb.move_word(-1, shift);
                    else
                        tb.move_codepoint(-1, shift);
                    nav = true;
                }
                else if (c.input().key_press(Key::Right))
                {
                    if (ctrl)
                        tb.move_word(1, shift);
                    else
                        tb.move_codepoint(1, shift);
                    nav = true;
                }
                else if (c.input().key_press(Key::Home))
                {
                    tb.move_home(shift);
                    nav = true;
                }
                else if (c.input().key_press(Key::End))
                {
                    tb.move_end(shift);
                    nav = true;
                }
                else if (c.input().key_press(Key::Backspace))
                {
                    tb.backspace();
                    blink_reset = true;
                }
                else if (c.input().key_press(Key::Delete))
                {
                    tb.del();
                    blink_reset = true;
                }

                Platform *plat = platform();

                if (ctrl && c.input().key_press(ascii_key('a')))
                {
                    tb.select_all();
                    nav = true;
                }
                else if (ctrl && c.input().key_press(ascii_key('c')))
                {
                    if (plat && tb.has_selection()) plat->set_clipboard_text(tb.selected());
                }
                else if (ctrl && c.input().key_press(ascii_key('x')))
                {
                    if (plat && tb.has_selection())
                    {
                        plat->set_clipboard_text(tb.selected());
                        tb.delete_selection();
                        blink_reset = true;
                    }
                }
                else if (ctrl && c.input().key_press(ascii_key('v')))
                {
                    if (plat)
                    {
                        const std::string clip = plat->clipboard_text();
                        if (!clip.empty())
                        {
                            const size_t room = (size_t)capacity - 1;
                            std::string_view cv(clip);
                            if (cv.size() > room) cv = cv.substr(0, room);
                            tb.insert(cv);
                            blink_reset = true;
                        }
                    }
                }
                else if (ctrl && c.input().key_press(ascii_key('z')))
                {
                    if (shift)
                        tb.redo();
                    else
                        tb.undo();
                    blink_reset = true;
                }
                else if (ctrl && c.input().key_press(ascii_key('y')))
                {
                    tb.redo();
                    blink_reset = true;
                }

                if (nav) blink_reset = true;

                if (!tb.has_composition())
                {
                    char utf8[5];
                    for (u32 ch : c.input().chars)
                    {
                        if (ch == '\r' || ch == '\n' || ch < 32) continue;
                        int n = encode_utf8(ch, utf8);
                        if (n <= 0) continue;
                        if (s.size() + (size_t)n + 1 > (size_t)capacity) break;
                        tb.insert(std::string_view(utf8, (size_t)n));
                        blink_reset = true;
                    }
                }

                if (blink_reset) caret_restart(c, st);
            }

            if (tb.has_selection())
            {
                Rect spans[256];
                const u32 ns = text_span_rects(f, s.c_str(), tb.sel_min(), tb.sel_max(), pen.x, lh,
                                               spans, 256);
                for (u32 i = 0; i < ns; ++i)
                {
                    spans[i].x -= 0.5f;
                    spans[i].w += 1.0f;
                    c.dl().push(spans[i], th.selection, -0.1f);
                }
            }

            shape_text(c.dl(), f, s.c_str(), pen, th.text, avail, gs);

            if (tb.has_composition())
            {
                const f32 x0 = pen.x + text_offset_x(f, s.c_str(), tb.caret());
                const std::string &pre = tb.preedit();
                Vec2 pre_pen{x0, pen.y};

                shape_text(c.dl(), f, pre.c_str(), pre_pen, th.text, 0.0f, gs);

                const f32 pw = text_width(f, pre.c_str());
                if (pw > 0.0f)
                {
                    const f32 uy = pen.y + 2.0f;
                    c.dl().push(Rect{x0, uy, pw, 1.0f}, th.accent_color, 0.25f);
                }
                preedit_x_ = x0 + text_offset_x(f, pre.c_str(), tb.preedit_caret());
                preedit_drawn_ = true;
            }

            if (focused && caret_visible(c, st))
            {
                const f32 cx = preedit_drawn_ ? preedit_x_
                                              : pen.x + text_offset_x(f, s.c_str(), tb.caret());
                c.dl().push(Rect{cx, r.y + (r.h - lh) * 0.5f, 1.5f, lh}, th.accent_color, 0.2f);
            }

            const size_t room = (size_t)capacity - 1;
            const size_t n = s.size() < room ? s.size() : room;
            memcpy(buffer, s.data(), n);
            buffer[n] = 0;
        }
        else if (placeholder)
        {
            u32 gs = 0;
            shape_text(c.dl(), f, placeholder, pen, th.text_tertiary, r.w - th.spacing_sm * 2.0f, gs);
        }

        return focused;
    }



    struct WrapCache
    {
        bool valid = false;
        Id id = 0;
        FontId font = 0;
        f32 width = 0.0f;
        u64 hash = 0;
        size_t text_size = 0;
        bool wrapped = true;
        std::vector<WrapSpan> rows;
        std::vector<u32> row_line;
        std::vector<u32> line_first_row;
        size_t total_rows = 0;
    };

    static WrapCache &wrap_cache(Id id)
    {
        static std::unordered_map<Id, WrapCache> cache;
        static const size_t kMaxEntries = 16;
        if (cache.size() > kMaxEntries) cache.clear();
        return cache[id];
    }

    bool text_area(Context &c, const Rect &r, char *buffer, int capacity, const char *placeholder,
                   Id id_hint, bool wrap)
    {
        Theme &th = c.theme();
        Id id = id_hint ? id_hint : widget_id(c, 0xB0000006u, r);
        WidgetState &st = c.state_touch(id);

        bool over = c.hover(r, id);
        bool focused = c.focus().has_focus(id);
        if (focused) st.focused = true;

        CornerRadii rad(th.corner_field);
        c.dl().push_rounded(r, focused ? th.control_bg_active : th.control_bg, rad, 0.0f);
        const f32 ring_w = focused ? 2.0f : th.hairline;
        c.dl().push_border(r, focused ? th.accent_color : (over ? th.control_border : th.separator),
                           ring_w, rad, 0.1f);

        if (!buffer || capacity <= 0)
        {
            const FontId f = c.font(th.font_body);
            if (placeholder && fonts().face(f))
            {
                const f32 lh = fonts().line_height(f);
                const f32 ascent = fonts().face(f)->ascent;
                (void)lh;
                u32 gs = 0;
                shape_text(c.dl(), f, placeholder,
                           {r.x + th.spacing_sm, r.y + th.spacing_md + ascent}, th.text_tertiary,
                           r.w - th.spacing_sm * 2.0f, gs);
            }
            return focused;
        }

        TextBuffer &tb = c.text_edit(id);
        c.sync_text_from_host(tb, buffer, (size_t)strlen(buffer));

        const FontId f = c.font(th.font_body);
        const f32 lh = fonts().line_height(f);
        if (!fonts().face(f)) return focused;
        const f32 ascent = fonts().face(f)->ascent;

        const f32 pad_x = th.spacing_sm;
        const f32 pad_y = th.spacing_md;
        const f32 view_w_full = r.w - pad_x * 2.0f;
        const f32 view_h = r.h - pad_y * 2.0f;
        if (view_w_full <= 0.0f || lh <= 0.0f || view_h < lh) return focused;

        const int visible_rows_est = (int)(view_h / lh);
        if (visible_rows_est < 1) return focused;

        const std::string &s = tb.text();
        const size_t nlines = tb.line_count();

        const f32 sb_w = 10.0f;

        auto build_wrap = [&](f32 width) -> WrapCache & {
            WrapCache &wc = wrap_cache(id);
            const u64 h = text_fingerprint(s);
            if (wc.valid && wc.id == id && wc.font == f && wc.width == width &&
                wc.hash == h && wc.text_size == s.size() && wc.wrapped == wrap)
            {
                return wc;
            }
            wc.rows.clear();
            wc.row_line.clear();
            wc.line_first_row.assign(nlines + 1, 0);
            wc.rows.reserve(nlines);
            wc.row_line.reserve(nlines);
            if (wrap)
            {
                size_t longest = 1;
                for (size_t ln = 0; ln < nlines; ++ln)
                    longest = std::max(longest, tb.line_end(ln) - tb.line_start(ln));
                std::vector<WrapSpan> scratch(longest);
                for (size_t ln = 0; ln < nlines; ++ln)
                {
                    wc.line_first_row[ln] = (u32)wc.rows.size();
                    const size_t ls = tb.line_start(ln);
                    const size_t le = tb.line_end(ln);
                    const int nr = wrap_line(f, s, ls, le, width, scratch.data(),
                                             (int)scratch.size());
                    if (nr == 0)
                    {
                        wc.rows.push_back(WrapSpan{ls, le});
                        wc.row_line.push_back((u32)ln);
                    }
                    else
                    {
                        for (int k = 0; k < nr; ++k)
                        {
                            wc.rows.push_back(scratch[(size_t)k]);
                            wc.row_line.push_back((u32)ln);
                        }
                    }
                }
            }
            else
            {
                for (size_t ln = 0; ln < nlines; ++ln)
                {
                    wc.line_first_row[ln] = (u32)wc.rows.size();
                    wc.rows.push_back(WrapSpan{tb.line_start(ln), tb.line_end(ln)});
                    wc.row_line.push_back((u32)ln);
                }
            }
            wc.line_first_row[nlines] = (u32)wc.rows.size();
            wc.total_rows = wc.rows.size();
            wc.valid = true;
            wc.id = id;
            wc.font = f;
            wc.width = width;
            wc.hash = h;
            wc.text_size = s.size();
            wc.wrapped = wrap;
            return wc;
        };

        WrapCache &wc_full = build_wrap(view_w_full);
        const bool v_scroll = wc_full.total_rows > (size_t)visible_rows_est;
        const f32 view_w = view_w_full - (v_scroll ? sb_w : 0.0f);
        if (view_w <= 0.0f) return focused;
        WrapCache &wc = v_scroll ? build_wrap(view_w) : wc_full;
        const std::vector<WrapSpan> &rows = wc.rows;
        const std::vector<u32> &row_line = wc.row_line;
        const size_t total_rows = wc.total_rows;
        if (total_rows == 0) return focused;

        const f32 content_h = (f32)total_rows * lh;

        auto row_of_byte = [&](size_t b) -> size_t {
            size_t lo = 0, hi = rows.size();
            while (lo < hi)
            {
                const size_t mid = (lo + hi) / 2;
                if (rows[mid].begin <= b) lo = mid + 1;
                else hi = mid;
            }
            return lo ? lo - 1 : 0;
        };

        if (st.scroll < 0.0f) st.scroll = 0.0f;
        if (st.scroll > (f32)(nlines - 1)) st.scroll = (f32)(nlines - 1);
        size_t first_visual_row = wc.line_first_row[(size_t)st.scroll];

        bool blink_reset = false;

        auto hit_at = [&](f32 mx, f32 my) -> size_t {
            long ridx = first_visual_row + (long)floorf((my - (r.y + pad_y)) / lh);
            if (ridx < 0) ridx = 0;
            if ((size_t)ridx >= total_rows) ridx = (long)total_rows - 1;
            const WrapSpan &row = rows[(size_t)ridx];
            const size_t col = text_hit_index_n(f, s.data() + row.begin, row.end - row.begin,
                                                mx - (r.x + pad_x) + st.scroll_x);
            return row.begin + col;
        };

        if (over && c.input().buttons[(int)MouseButton::Left].pressed)
        {
            c.focus().set_focus(id);
            focused = true;
            const size_t hit = hit_at(c.input().mouse_pos.x, c.input().mouse_pos.y);
            if (c.input().buttons[(int)MouseButton::Left].double_click)
            {
                const size_t ln = tb.line_of(hit);
                const size_t ls = tb.line_start(ln), le = tb.line_end(ln);
                auto is_word = [](char ch) { return ch != ' ' && ch != '\t' && ch != '\r'; };
                size_t a = hit, b = hit;
                while (a > ls && is_word(s[a - 1])) --a;
                while (b < le && is_word(s[b])) ++b;
                tb.set_caret(a);
                tb.set_anchor(b);
            }
            else
            {
                tb.set_caret(hit);
            }
            blink_reset = true;
        }
        if (over && c.input().buttons[(int)MouseButton::Left].down && focused)
            tb.select_to(hit_at(c.input().mouse_pos.x, c.input().mouse_pos.y));

        if (over && c.input().scroll_y != 0.0f && v_scroll)
        {
            long target = (long)first_visual_row - (long)(c.input().scroll_y * 3.0f);
            if (target < 0) target = 0;
            if ((size_t)target >= total_rows) target = (long)total_rows - 1;
            st.scroll = (f32)row_line[(size_t)target];
        }

        if (focused)
        {
            const bool shift = c.input().shift;
            const bool ctrl = c.input().ctrl;
            Platform *plat = platform();
            bool nav = false;

            if (c.input().key_press(Key::Up) || c.input().key_press(Key::Down) ||
                c.input().key_press(Key::PageUp) || c.input().key_press(Key::PageDown))
            {
                const int dir = (c.input().key_press(Key::Up) || c.input().key_press(Key::PageUp))
                                    ? -1
                                    : 1;
                const int step =
                    (c.input().key_press(Key::PageUp) || c.input().key_press(Key::PageDown))
                        ? visible_rows_est
                        : 1;
                const size_t crow = row_of_byte(tb.caret());
                const WrapSpan &cr = rows[crow];
                const f32 cx = text_width_n(f, s.data() + cr.begin, tb.caret() - cr.begin);
                long target = (long)crow + dir * step;
                if (target < 0) target = 0;
                if ((size_t)target >= total_rows) target = (long)total_rows - 1;
                const WrapSpan &tr = rows[(size_t)target];
                const size_t col = text_hit_index_n(f, s.data() + tr.begin, tr.end - tr.begin, cx);
                const size_t want = tr.begin + col;
                if (shift)
                    tb.select_to(want);
                else
                    tb.set_caret(want);
                nav = true;
            }
            else if (c.input().key_press(Key::Left))
            {
                ctrl ? tb.move_word(-1, shift) : tb.move_codepoint(-1, shift);
                nav = true;
            }
            else if (c.input().key_press(Key::Right))
            {
                ctrl ? tb.move_word(1, shift) : tb.move_codepoint(1, shift);
                nav = true;
            }
            else if (c.input().key_press(Key::Home) || c.input().key_press(Key::End))
            {
                if (ctrl)
                {
                    const size_t was = tb.caret();
                    tb.set_caret(c.input().key_press(Key::Home) ? (size_t)0 : s.size());
                    if (shift) tb.select_to(was);
                }
                else
                {
                    const WrapSpan &cr = rows[row_of_byte(tb.caret())];
                    const size_t want = c.input().key_press(Key::Home) ? cr.begin : cr.end;
                    if (shift)
                        tb.select_to(want);
                    else
                        tb.set_caret(want);
                }
                nav = true;
            }
            else if (c.input().key_press(Key::Backspace))
            {
                tb.backspace();
                blink_reset = true;
            }
            else if (c.input().key_press(Key::Delete))
            {
                tb.del();
                blink_reset = true;
            }
            else if (c.input().key_press(Key::Enter))
            {
                if (s.size() + 2 <= (size_t)capacity - 1)
                {
                    tb.insert("\n");
                    blink_reset = true;
                }
            }

            if (ctrl && c.input().key_press(ascii_key('a')))
            {
                tb.select_all();
                nav = true;
            }
            else if (ctrl && c.input().key_press(ascii_key('c')))
            {
                if (plat && tb.has_selection()) plat->set_clipboard_text(tb.selected());
            }
            else if (ctrl && c.input().key_press(ascii_key('x')))
            {
                if (plat && tb.has_selection())
                {
                    plat->set_clipboard_text(tb.selected());
                    tb.delete_selection();
                    blink_reset = true;
                }
            }
            else if (ctrl && c.input().key_press(ascii_key('v')))
            {
                if (plat)
                {
                    const std::string clip = plat->clipboard_text();
                    if (!clip.empty())
                    {
                        const size_t room = (size_t)capacity - 1;
                        std::string_view cv(clip);
                        if (cv.size() > room) cv = cv.substr(0, room);
                        tb.insert(cv);
                        blink_reset = true;
                    }
                }
            }
            else if (ctrl && c.input().key_press(ascii_key('z')))
            {
                shift ? tb.redo() : tb.undo();
                blink_reset = true;
            }
            else if (ctrl && c.input().key_press(ascii_key('y')))
            {
                tb.redo();
                blink_reset = true;
            }

            if (nav) blink_reset = true;

            if (!tb.has_composition())
            {
                char utf8[5];
                for (u32 ch : c.input().chars)
                {
                    if (ch == '\r' || ch == '\n' || ch < 32) continue;
                    const int nb = encode_utf8(ch, utf8);
                    if (nb <= 0) continue;
                    if (s.size() + (size_t)nb + 1 > (size_t)capacity - 1) break;
                    tb.insert(std::string_view(utf8, (size_t)nb));
                    blink_reset = true;
                }
            }

            if (blink_reset) caret_restart(c, st);
        }

        {
            const size_t crow = row_of_byte(tb.caret());
            const size_t caret_line = row_line[crow];
            if (crow < first_visual_row)
            {
                st.scroll = (f32)caret_line;
            }
            else if (crow >= first_visual_row + (size_t)visible_rows_est)
            {
                size_t ln = (size_t)st.scroll;
                while (ln + 1 < nlines &&
                       wc.line_first_row[ln] + (size_t)visible_rows_est <= crow)
                    ++ln;
                st.scroll = (f32)ln;
            }
            if (st.scroll < 0.0f) st.scroll = 0.0f;
            if (st.scroll > (f32)(nlines - 1)) st.scroll = (f32)(nlines - 1);
            first_visual_row = wc.line_first_row[(size_t)st.scroll];

            const WrapSpan &cr = rows[row_of_byte(tb.caret())];
            const f32 cx = text_width_n(f, s.data() + cr.begin, tb.caret() - cr.begin);
            f32 sx = st.scroll_x;
            if (sx > cx) sx = cx;
            if (sx + view_w < cx) sx = cx - view_w + 1.0f;
            if (sx < 0.0f) sx = 0.0f;
            st.scroll_x = sx;
        }

        auto row_y = [&](size_t ri) -> f32 {
            return r.y + pad_y + (f32)(ri - first_visual_row) * lh;
        };
        const Vec2 origin{r.x + pad_x - st.scroll_x, r.y + pad_y + ascent};

        const size_t last_visible_row = first_visual_row + (size_t)visible_rows_est;

        if (tb.has_selection())
        {
            const size_t lo = tb.sel_min(), hi = tb.sel_max();
            for (size_t ri = first_visual_row; ri < last_visible_row && ri < total_rows; ++ri)
            {
                const WrapSpan &row = rows[ri];
                const size_t ca = lo > row.begin ? lo : row.begin;
                const size_t cb = hi < row.end ? hi : row.end;
                if (cb <= ca) continue;
                const f32 x0 = text_width_n(f, s.data() + row.begin, ca - row.begin);
                const f32 x1 = text_width_n(f, s.data() + row.begin, cb - row.begin);
                if (x1 - x0 <= 0.0f) continue;
                c.dl().push(Rect{origin.x + x0, row_y(ri), x1 - x0, lh},
                            th.selection, -0.1f);
            }
        }

        for (size_t ri = first_visual_row; ri < last_visible_row && ri < total_rows; ++ri)
        {
            const WrapSpan &row = rows[ri];
            if (row.end <= row.begin) continue;
            const Vec2 row_pen{origin.x, row_y(ri) + ascent};
            u32 gs = 0;
            shape_text_n(c.dl(), f, s.data() + row.begin, row.end - row.begin, row_pen, th.text,
                         view_w, gs);
        }

        f32 preedit_x_ = 0.0f;
        bool preedit_drawn_ = false;
        if (tb.has_composition())
        {
            const size_t crow = row_of_byte(tb.caret());
            const WrapSpan &cr = rows[crow];
            const f32 x0 = origin.x + text_width_n(f, s.data() + cr.begin, tb.caret() - cr.begin);
            const std::string &pre = tb.preedit();
            const Vec2 pp{x0, row_y(crow) + ascent};
            u32 gs = 0;
            shape_text(c.dl(), f, pre.c_str(), pp, th.text, view_w, gs);
            const f32 pw = text_width(f, pre.c_str());
            if (pw > 0.0f) c.dl().push(Rect{x0, pp.y - ascent + 2.0f, pw, 1.0f}, th.accent_color, 0.25f);
            preedit_x_ = x0 + text_offset_x(f, pre.c_str(), tb.preedit_caret());
            preedit_drawn_ = true;
        }

        if (focused && caret_visible(c, st))
        {
            const size_t crow = row_of_byte(tb.caret());
            const WrapSpan &cr = rows[crow];
            const f32 cx = preedit_drawn_
                               ? preedit_x_
                               : origin.x + text_width_n(f, s.data() + cr.begin, tb.caret() - cr.begin);
            c.dl().push(Rect{cx, row_y(crow), 1.5f, lh}, th.accent_color, 0.2f);
        }

        if (v_scroll)
        {
            const Rect track{r.right() - sb_w + 3.0f, r.y + pad_y, 3.0f, view_h};
            c.dl().push_rounded(track, th.control_border, CornerRadii(1.5f), 0.3f);
            f32 thumb_h = view_h * ((f32)visible_rows_est / (f32)total_rows);
            if (thumb_h < 14.0f) thumb_h = 14.0f;
            if (thumb_h > view_h) thumb_h = view_h;
            const f32 range = content_h - view_h;
            const f32 t = range > 0.0f ? ((f32)first_visual_row * lh) / range : 0.0f;
            const Rect thumb{track.x, track.y + t * (view_h - thumb_h), track.w, thumb_h};
            c.dl().push_rounded(thumb, th.text_tertiary, CornerRadii(1.5f), 0.3f);
        }

        const size_t room = (size_t)capacity - 1;
        const size_t n = s.size() < room ? s.size() : room;
        memcpy(buffer, s.data(), n);
        buffer[n] = 0;

        return focused;
    }

}