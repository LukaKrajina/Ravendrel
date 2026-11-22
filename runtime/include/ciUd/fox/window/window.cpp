#include "window.hpp"
#include "core/anim.hpp"
#include "widget/primitives.hpp"

namespace fox
{


    int traffic_lights(Context &c, const Vec2 &origin)
    {
        Theme &th = c.theme();
        Id gid = c.make_id(0x77000001u, "traffic");

        f32 d = 12.0f;
        f32 gap = 8.0f;
        f32 pad = 4.0f;

        Rect group{origin.x - pad, origin.y - pad, d * 3.0f + gap * 2.0f + pad * 2.0f, d + pad * 2.0f};
        bool group_hover = c.hover(group, gid);

        static const Color kColors[3] = {
            Color{255, 95, 87, 255},
            Color{255, 189, 46, 255},
            Color{39, 201, 63, 255},
        };

        int clicked = -1;
        for (int i = 0; i < 3; ++i)
        {
            Rect lr{origin.x + (f32)i * (d + gap), origin.y, d, d};
            Id lid = c.make_id(0x77000010u, (Id)(i + 1));
            WidgetState &ls = c.state_touch(lid);
            bool over = c.hover(lr, lid);

            f32 target = group_hover ? (over ? 1.0f : 0.55f) : 0.0f;
            ls.hover = approach_hover(ls.hover, target, c.anim_dt(), th.anim_fast);

            if (ls.hover > 0.05f)
            {
                Color fg = Color{0, 0, 0, (u8)(ls.hover * 190.0f)};
                f32 cx = lr.cx(), cy = lr.cy(), s = d * 0.26f;
                if (i == 0)
                {
                    c.dl().push(Rect{cx - s, cy - s * 0.28f, s * 2.0f, 1.2f}, fg, 3.0f);
                    c.dl().push(Rect{cx - s, cy + s * 0.28f - 1.2f, s * 2.0f, 1.2f}, fg, 3.0f);
                    c.dl().push(Rect{cx - s * 0.28f - 1.2f, cy - s, 1.2f, s * 2.0f}, fg, 3.0f);
                    c.dl().push(Rect{cx + s * 0.28f, cy - s, 1.2f, s * 2.0f}, fg, 3.0f);
                }
                else if (i == 1)
                {
                    c.dl().push(Rect{cx - s, cy - 1.2f, s * 2.0f, 1.2f}, fg, 3.0f);
                }
                else
                {
                    c.dl().push(Rect{cx - s, cy - 1.2f, s * 2.0f, 1.2f}, fg, 3.0f);
                    c.dl().push(Rect{cx - 1.2f, cy - s, 1.2f, s * 2.0f}, fg, 3.0f);
                }
            }

            Shadow sh;
            sh.offset_y = 0.5f;
            sh.blur = 2.0f;
            sh.color = Color{0, 0, 0, 0x50};
            shadow_rect(c, lr.translated({0, 0.5f}), CornerRadii(d * 0.5f), sh, 1.0f);

            Color ring = Color{0, 0, 0, (u8)(40 + ls.hover * 60)};
            c.dl().push_rounded(lr, kColors[i], CornerRadii(d * 0.5f), 1.5f);
            c.dl().push_border(lr, ring, 0.5f, CornerRadii(d * 0.5f), 1.6f);

            if (over && c.input().buttons[(int)MouseButton::Left].pressed)
                clicked = i;
        }
        return clicked;
    }


    int sidebar(Context &c, const Rect &r, const SidebarItem *items, int count, int selected,
                int &scroll_out)
    {
        Theme &th = c.theme();
        Id sid = c.make_id(0x77000002u, "sidebar");

        c.dl().push(r, th.sidebar_bg, -3.0f);
        c.dl().push(Rect{r.right() - th.hairline, r.y, th.hairline, r.h}, th.separator, -2.9f);

        f32 scroll = 0.0f;
        Rect viewport = c.layout().scroll_area(r.inset(0, 8), sid, c.input(), (f32)count * th.row_height, scroll);
        scroll_out = (int)scroll;

        int clicked = -1;
        FontId font = c.font(th.font_body);
        FontId small = c.font(th.font_small);

        for (int i = 0; i < count; ++i)
        {
            float y = viewport.y - scroll + (f32)i * th.row_height;
            Rect row{r.x + 8.0f, y, r.w - 16.0f, th.row_height - 2.0f};

            Id rid = c.make_id(0x77000020u, (Id)(i + 1));
            WidgetState &st = c.state_touch(rid);
            bool over = row.contains(c.input().mouse_pos);

            bool sel = (i == selected);
            st.hover = approach_hover(st.hover, over ? 1.0f : 0.0f, c.anim_dt(), th.anim_fast);
            if (over || st.hover > 0.01f) c.request_redraw();

            if (sel)
                c.dl().push_rounded(row, Color::mix(th.accent_color, th.sidebar_bg, 0.82f),
                                    CornerRadii(th.corner_control), 1.0f);
            else if (st.hover > 0.01f)
                c.dl().push_rounded(row,
                                    Color{255, 255, 255, (u8)(st.hover * 28)},
                                    CornerRadii(th.corner_control), 1.0f);

            f32 tx = row.x + th.spacing_md;
            if (items[i].icon_tex != TEX_NONE)
            {
                Color ic = sel ? th.accent_color : th.sidebar_text_dim;
                const f32 isz = 18.0f;
                const f32 iy = row.y + (row.h - isz) * 0.5f;
                image_region(c, Rect{tx, iy, isz, isz}, items[i].icon_tex, items[i].icon_src, ic);
                tx += isz + th.spacing_sm;
            }
            else if (items[i].icon)
            {
                Color ic = sel ? th.accent_color : th.sidebar_text_dim;
                label(c, Rect{tx, row.y, 20.0f, row.h}, items[i].icon, ic, Align::Center);
                tx += 20.0f + th.spacing_sm;
            }
            Color fg = sel ? th.sidebar_text : th.sidebar_text;
            if (!sel && st.hover > 0.5f) fg = th.sidebar_text;
            label(c, Rect{tx, row.y, row.w - (tx - row.x) - th.spacing_sm, row.h}, items[i].label,
                  fg, Align::Start);
            (void)font;
            (void)small;

            if (over && c.input().buttons[(int)MouseButton::Left].pressed)
                clicked = i;
        }
        return clicked;
    }


    bool Window::begin(Context &c, const Rect &bounds, const char *title, bool show_sidebar)
    {
        Theme &th = c.theme();
        close_requested_ = false;
        show_sidebar_ = show_sidebar;

        c.dl().push_rounded(bounds, th.window_bg, CornerRadii(th.corner_window), -5.0f);

        titlebar_ = Rect{bounds.x, bounds.y, bounds.w, th.titlebar_height};

        CornerRadii top(th.corner_window);
        c.dl().push_glass(titlebar_, top, th.glass_tint, th.glass_blur, th.glass_opacity,
                          th.glass_saturation, -2.0f);
        c.dl().push(Rect{bounds.x, titlebar_.bottom() - th.hairline, bounds.w, th.hairline},
                    th.separator, -1.9f);

        Vec2 light_origin{bounds.x + 14.0f, titlebar_.y + (th.titlebar_height - 12.0f) * 0.5f};
        int light = traffic_lights(c, light_origin);
        if (light == (int)LightButton::Close) close_requested_ = true;

        if (title)
        {
            FontId tf = c.font(th.font_callout);
            f32 tw = text_width(tf, title);
            f32 tx = bounds.cx() - tw * 0.5f;
            u32 gs = 0;
            f32 lh = fonts().line_height(tf);
            f32 ty = titlebar_.y + (th.titlebar_height - lh) * 0.5f + fonts().face(tf)->ascent;
            shape_text(c.dl(), tf, title, {tx, ty}, th.text, 0.0f, gs);
        }

        f32 content_y = titlebar_.bottom();
        f32 content_h = bounds.h - th.titlebar_height;
        if (show_sidebar)
        {
            sidebar_ = Rect{bounds.x, content_y, th.sidebar_width, content_h};
            content_ = Rect{sidebar_.right(), content_y, bounds.w - th.sidebar_width, content_h};
        }
        else
        {
            sidebar_ = Rect{};
            content_ = Rect{bounds.x, content_y, bounds.w, content_h};
        }
        return true;
    }

}