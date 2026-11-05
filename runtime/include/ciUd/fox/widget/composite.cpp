#include "composite.hpp"
#include "primitives.hpp"
#include "core/anim.hpp"

#include <cstdio>
#include <cstring>

namespace fox
{
    static inline Id composite_id(const Context &c, Id salt, const Rect &r)
    {
        return c.make_id(salt, (Id)id_hash_rect(r));
    }

    bool menu_title(Context &c, const Rect &r, const char *label_text)
    {
        Theme &th = c.theme();
        Id id = composite_id(c, 0xB0000020u, r);
        WidgetState &st = c.state_touch(id);
        bool open = c.popup_is_open(id);

        bool over = c.hover(r, id);

        if (!open && over && c.open_popup() != 0 &&
            c.input().buttons[(int)MouseButton::Left].down == false)
        {
            c.set_open_popup(id);
            open = true;
        }

        st.anim = approach(st.anim, open ? 1.0f : 0.0f, c.anim_dt(), th.anim_fast);
        if (fx_abs(st.anim - (open ? 1.0f : 0.0f)) > 0.001f) c.request_redraw();

        Color fill = Color::mix(th.control_bg, th.accent_color, st.anim * 0.22f);
        if (over && !open) fill = th.control_bg_hover;
        if (st.anim > 0.01f || over)
        {
            c.dl().push_rounded(r, fill, CornerRadii(th.corner_control), 1.0f);
        }

        Color fg = open ? th.accent_color : th.text;
        label(c, r, label_text ? label_text : "", fg, Align::Center);

        bool clicked = false;
        if (over && c.input().buttons[(int)MouseButton::Left].pressed)
        {
            if (open)
                c.set_open_popup(0);
            else
                c.set_open_popup(id);
            clicked = true;
        }
        return clicked;
    }

    Rect menu_popup(Context &c, const Rect &anchor, int item_count,
                    f32 item_height, f32 min_width)
    {
        Id id = composite_id(c, 0xB0000020u, anchor);
        if (!c.popup_is_open(id)) return Rect{0.0f, 0.0f, 0.0f, 0.0f};

        const f32 pad = 4.0f;
        f32 w = fx_max(min_width, anchor.w);
        f32 h = item_height * (f32)fx_max(item_count, 0) + pad * 2.0f;
        Rect pop{anchor.x, anchor.y + anchor.h + 2.0f, w, h};
        if (pop.y + pop.h > (f32)c.height())
            pop.y = fx_max(0.0f, anchor.y - pop.h - 2.0f);

        if (c.input().buttons[(int)MouseButton::Left].pressed)
        {
            Vec2 m = c.input().mouse_pos;
            if (!anchor.contains(m) && !pop.contains(m))
            {
                c.set_open_popup(0);
                return Rect{0.0f, 0.0f, 0.0f, 0.0f};
            }
        }
        if (c.input().key_press(Key::Escape))
        {
            c.set_open_popup(0);
            return Rect{0.0f, 0.0f, 0.0f, 0.0f};
        }

        Theme &th = c.theme();
        CornerRadii rad(th.corner_panel);
        Shadow sh;
        sh.offset_y = 4.0f;
        sh.blur = 14.0f;
        sh.color = Color{0, 0, 0, 0x45};
        shadow_rect(c, pop.translated({0.0f, 2.0f}), rad, sh, 9.0f);
        c.dl().push_rounded(pop, th.window_bg, rad, 10.0f);
        c.dl().push_border(pop, th.control_border, th.hairline, rad, 10.1f);

        return Rect{pop.x + pad, pop.y + pad, pop.w - pad * 2.0f, pop.h - pad * 2.0f};
    }

    bool menu_item(Context &c, const Rect &r, const char *label_text, bool enabled)
    {
        Theme &th = c.theme();
        Id id = composite_id(c, 0xB0000021u, r);
        WidgetState &st = c.state_touch(id);

        bool over = enabled && c.hover(r, id);
        st.hover = approach(st.hover, over ? 1.0f : 0.0f, c.anim_dt(), th.anim_fast);

        if (over)
        {
            c.dl().push_rounded(r, Color::mix(th.control_bg, th.accent_color, 0.35f),
                                CornerRadii(4.0f), 10.2f);
        }

        Color fg = enabled ? th.text : th.text_tertiary;
        Rect tr{r.x + 8.0f, r.y, r.w - 8.0f, r.h};
        label(c, tr, label_text ? label_text : "", fg, Align::Start);

        bool clicked = false;
        if (over && c.input().buttons[(int)MouseButton::Left].pressed)
        {
            clicked = true;
            c.set_open_popup(0);
        }
        return clicked;
    }

    void menu_close_all(Context &c)
    {
        c.set_open_popup(0);
    }

    bool combo(Context &c, const Rect &r, const char *const *items, int count,
               int &selected)
    {
        Theme &th = c.theme();
        if (count <= 0 || !items) return false;
        if (selected < 0 || selected >= count) selected = 0;

        Id id = composite_id(c, 0xB0000022u, r);
        WidgetState &st = c.state_touch(id);
        bool open = c.popup_is_open(id);

        bool over = c.hover(r, id);
        st.hover = approach(st.hover, over ? 1.0f : 0.0f, c.anim_dt(), th.anim_fast);

        Color fill = Color::mix(th.control_bg, th.control_bg_hover, st.hover);
        if (open) fill = th.control_bg_active;
        CornerRadii rad(th.corner_control);
        c.dl().push_rounded(r, fill, rad, 0.0f);
        c.dl().push_border(r, open ? th.accent_color : th.control_border,
                           open ? 2.0f : th.hairline, rad, 0.1f);

        const f32 chevron_w = 14.0f;
        Rect text_r{r.x + th.spacing_sm, r.y, r.w - chevron_w - th.spacing_sm * 2.0f, r.h};
        label(c, text_r, items[selected], th.text, Align::Start);

        const f32 cx = r.x + r.w - chevron_w * 0.5f - 2.0f;
        const f32 cy = r.y + r.h * 0.5f;
        const f32 s = 3.5f;
        if (open)
        {
            c.dl().push_triangle({cx - s, cy + s * 0.6f}, {cx + s, cy + s * 0.6f}, {cx, cy - s * 0.6f},
                                 th.text_secondary, 0.2f);
        }
        else
        {
            c.dl().push_triangle({cx - s, cy - s * 0.6f}, {cx + s, cy - s * 0.6f}, {cx, cy + s * 0.6f},
                                 th.text_secondary, 0.2f);
        }

        if (over && c.input().buttons[(int)MouseButton::Left].pressed)
        {
            if (open)
                c.set_open_popup(0);
            else
                c.set_open_popup(id);
        }

        if (!open) return false;

        const f32 item_h = 22.0f;
        const f32 pad = 4.0f;
        f32 w = fx_max(r.w, 120.0f);
        f32 h = item_h * (f32)count + pad * 2.0f;
        Rect pop{r.x, r.y + r.h + 2.0f, w, h};
        if (pop.y + pop.h > (f32)c.height())
            pop.y = fx_max(0.0f, r.y - pop.h - 2.0f);

        if (c.input().buttons[(int)MouseButton::Left].pressed)
        {
            Vec2 m = c.input().mouse_pos;
            if (!r.contains(m) && !pop.contains(m))
            {
                c.set_open_popup(0);
                return false;
            }
        }
        if (c.input().key_press(Key::Escape))
        {
            c.set_open_popup(0);
            return false;
        }

        CornerRadii prad(th.corner_panel);
        Shadow sh;
        sh.offset_y = 4.0f;
        sh.blur = 14.0f;
        sh.color = Color{0, 0, 0, 0x45};
        shadow_rect(c, pop.translated({0.0f, 2.0f}), prad, sh, 9.0f);
        c.dl().push_rounded(pop, th.window_bg, prad, 10.0f);
        c.dl().push_border(pop, th.control_border, th.hairline, prad, 10.1f);

        bool changed = false;
        for (int i = 0; i < count; i++)
        {
            Rect row{pop.x + pad, pop.y + pad + item_h * (f32)i, pop.w - pad * 2.0f, item_h};
            Id row_id = composite_id(c, 0xB0000023u, row);
            WidgetState &rst = c.state_touch(row_id);
            bool rover = c.hover(row, row_id);
            rst.hover = approach(rst.hover, rover ? 1.0f : 0.0f, c.anim_dt(), th.anim_fast);

            if (i == selected)
            {
                c.dl().push_rounded(row, Color::mix(th.control_bg, th.accent_color, 0.30f),
                                    CornerRadii(4.0f), 10.2f);
            }
            else if (rover)
            {
                c.dl().push_rounded(row, Color::mix(th.control_bg, th.accent_color, 0.35f),
                                    CornerRadii(4.0f), 10.2f);
            }
            label(c, Rect{row.x + 8.0f, row.y, row.w - 8.0f, row.h}, items[i],
                  i == selected ? th.accent_color : th.text, Align::Start);

            if (rover && c.input().buttons[(int)MouseButton::Left].pressed)
            {
                if (selected != i)
                {
                    selected = i;
                    changed = true;
                }
                c.set_open_popup(0);
                break;
            }
        }
        return changed;
    }

    bool selectable(Context &c, const Rect &r, const char *label_text, bool &selected)
    {
        Theme &th = c.theme();
        Id id = composite_id(c, 0xB0000024u, r);
        WidgetState &st = c.state_touch(id);

        bool over = c.hover(r, id);
        st.hover = approach(st.hover, over ? 1.0f : 0.0f, c.anim_dt(), th.anim_fast);

        if (selected)
        {
            c.dl().push_rounded(r, Color::mix(th.control_bg, th.accent_color, 0.45f),
                                CornerRadii(th.corner_control * 0.6f), 0.0f);
        }
        else if (over)
        {
            c.dl().push_rounded(r, Color::mix(th.control_bg, th.accent_color, 0.18f),
                                CornerRadii(th.corner_control * 0.6f), 0.0f);
        }

        Color fg = selected ? th.accent_color : th.text;
        label(c, Rect{r.x + 10.0f, r.y, r.w - 10.0f, r.h}, label_text ? label_text : "", fg,
              Align::Start);

        bool clicked = false;
        if (over && c.input().buttons[(int)MouseButton::Left].pressed)
        {
            selected = true;
            clicked = true;
        }
        return clicked;
    }

    Rect group_begin(Context &c, const Rect &r, const char *title, Id id,
                     f32 content_height)
    {
        Theme &th = c.theme();
        c.dl().push_rounded(r, th.control_bg, CornerRadii(th.corner_panel), 0.0f);
        c.dl().push_border(r, th.control_border, th.hairline,
                           CornerRadii(th.corner_panel), 0.1f);

        const f32 title_h = 22.0f;
        Rect title_bar{r.x, r.y, r.w, title_h};
        c.dl().push_rounded(title_bar, Color::mix(th.control_bg, th.window_bg, 0.5f),
                            CornerRadii(th.corner_panel), 0.05f);
        label(c, Rect{title_bar.x + 10.0f, title_bar.y, title_bar.w - 10.0f, title_bar.h},
              title ? title : "", th.text_secondary, Align::Start);
        separator(c, Rect{r.x, r.y + title_h, r.w, 1.0f}, true);

        Rect content{r.x + 1.0f, r.y + title_h + 1.0f, r.w - 2.0f,
                     r.h - title_h - 2.0f};
        c.dl().push_clip(content);

        WidgetState &st = c.state_touch(id);
        const f32 max_scroll = fx_max(0.0f, content_height - content.h);
        if (c.hover(content, id))
        {
            st.scroll -= c.input().scroll_y * 40.0f;
        }
        st.scroll = fx_clamp(st.scroll, 0.0f, max_scroll);
        if (fx_abs(st.scroll) > 0.001f) c.request_redraw();

        if (max_scroll > 0.5f)
        {
            const f32 bar_w = 6.0f;
            const f32 track_h = content.h;
            const f32 thumb_h = fx_max(24.0f, track_h * (content.h / fx_max(content_height, 1.0f)));
            const f32 thumb_y = content.y + (track_h - thumb_h) * (st.scroll / max_scroll);
            Rect track{content.x + content.w - bar_w - 2.0f, content.y, bar_w, track_h};
            c.dl().push_rounded(track, Color{0, 0, 0, 0x18}, CornerRadii(bar_w * 0.5f), 0.3f);
            Rect thumb{track.x, thumb_y, bar_w, thumb_h};
            c.dl().push_rounded(thumb, Color{0, 0, 0, 0x35}, CornerRadii(bar_w * 0.5f), 0.31f);
        }

        return Rect{content.x, content.y - st.scroll, content.w, content.h};
    }

    void group_end(Context &c)
    {
        c.dl().push_clip_pop();
    }

    namespace
    {
        bool property_core(Context &c, const Rect &r, const char *label_text,
                           f32 &value, f32 min, f32 max, f32 step, bool is_int,
                           const char *fmt)
        {
            Theme &th = c.theme();
            Id id = composite_id(c, 0xB0000025u, r);
            WidgetState &st = c.state_touch(id);
            const f32 label_w = r.w * 0.45f;
            const f32 box_w = r.w - label_w - th.spacing_sm;
            Rect label_r{r.x, r.y, label_w, r.h};
            Rect box_r{r.x + label_w + th.spacing_sm, r.y + 1.0f, box_w, r.h - 2.0f};

            label(c, label_r, label_text ? label_text : "", th.text, Align::Start);

            bool over_box = c.hover(box_r, id);
            bool focused = c.focus().has_focus(id);
            if (focused) st.focused = true;

            Color fill = focused ? th.control_bg_active : th.control_bg;
            CornerRadii rad(th.corner_field);
            c.dl().push_rounded(box_r, fill, rad, 0.0f);
            c.dl().push_border(box_r, focused ? th.accent_color
                                              : (over_box ? th.control_border : th.separator),
                               focused ? 2.0f : th.hairline, rad, 0.1f);

            char buf[64];
            if (is_int)
                std::snprintf(buf, sizeof buf, fmt, (long long)value);
            else
                std::snprintf(buf, sizeof buf, fmt, (double)value);
            label(c, box_r, buf, th.text, Align::Center);

            if (over_box && c.input().buttons[(int)MouseButton::Left].pressed)
            {
                c.focus().set_focus(id);
                st.pressed = true;
                st.drag_start_x = c.input().mouse_pos.x;
                st.drag_start_value = value;
            }
            if (st.pressed && !c.input().buttons[(int)MouseButton::Left].down)
                st.pressed = false;

            bool changed = false;

            if (st.pressed && c.input().buttons[(int)MouseButton::Left].down)
            {
                const f32 range = fx_max(max - min, (f32)step);
                const f32 per_px = range / fx_max(box_r.w, 1.0f) * 0.5f;
                f32 nv = st.drag_start_value +
                         (c.input().mouse_pos.x - st.drag_start_x) * per_px;
                if (is_int)
                    nv = (f32)(long long)(nv + (nv >= 0 ? 0.5f : -0.5f));
                else if (step > 0.0f)
                    nv = std::round(nv / step) * step;
                nv = fx_clamp(nv, min, max);
                if (nv != value)
                {
                    value = nv;
                    changed = true;
                }
            }

            if (focused)
            {
                f32 nv = value;
                if (c.input().key_press(Key::Up) || c.input().key_press(Key::Right))
                    nv = value + step;
                else if (c.input().key_press(Key::Down) || c.input().key_press(Key::Left))
                    nv = value - step;
                nv = fx_clamp(nv, min, max);
                if (nv != value)
                {
                    value = nv;
                    changed = true;
                }
            }
            return changed;
        }
    }

    bool property_int(Context &c, const Rect &r, const char *label, int &value,
                      int min, int max, int step)
    {
        if (max < min) max = min;
        f32 v = (f32)value;
        const bool changed = property_core(c, r, label, v, (f32)min, (f32)max,
                                           (f32)(step > 0 ? step : 1), true, "%lld");
        value = (int)v;
        return changed;
    }

    bool property_float(Context &c, const Rect &r, const char *label, float &value,
                        float min, float max, float step)
    {
        if (max < min) max = min;
        return property_core(c, r, label, value, min, max, step, false, "%.3f");
    }

}