#include "theme.hpp"

namespace fox
{

    f32 accent_hue(Accent a)
    {
        switch (a)
        {
        case Accent::Blue: return 211.0f;
        case Accent::Purple: return 280.0f;
        case Accent::Pink: return 333.0f;
        case Accent::Red: return 4.0f;
        case Accent::Orange: return 30.0f;
        case Accent::Yellow: return 45.0f;
        case Accent::Green: return 135.0f;
        case Accent::Graphite: return 220.0f;
        default: return 211.0f;
        }
    }

    static Color accent_state(Color base, f32 amount)
    {
        return amount >= 0.0f ? Color::mix(base, Color{255, 255, 255, 255}, amount)
                              : Color::mix(base, Color{0, 0, 0, 255}, -amount);
    }

    void Theme::init_accent(Accent ac)
    {
        accent = ac;
        accent_hue = fox::accent_hue(ac);
        switch (ac)
        {
        case Accent::Blue:
            accent_color = rgb(0x0A84FF);
            break;
        case Accent::Purple:
            accent_color = rgb(0xBF5AF2);
            break;
        case Accent::Pink:
            accent_color = rgb(0xFF375F);
            break;
        case Accent::Red:
            accent_color = rgb(0xFF453A);
            break;
        case Accent::Orange:
            accent_color = rgb(0xFF9F0A);
            break;
        case Accent::Yellow:
            accent_color = rgb(0xFFD60A);
            break;
        case Accent::Green:
            accent_color = rgb(0x32D74B);
            break;
        case Accent::Graphite:
            accent_color = rgb(0x8E8E93);
            break;
        default:
            accent_color = rgb(0x0A84FF);
            break;
        }
        bool is_dark = (appearance == Appearance::Dark);
        accent_hover = is_dark ? accent_state(accent_color, 0.12f) : accent_state(accent_color, -0.06f);
        accent_active = is_dark ? accent_state(accent_color, -0.10f) : accent_state(accent_color, -0.14f);
        text_on_accent = (accent_color.luminance() > 0.6f) ? Color{0, 0, 0, 255}
                                                          : Color{255, 255, 255, 255};
    }

    static f32 hue_from_rgb(Color c)
    {
        const f32 r = (f32)c.r / 255.0f;
        const f32 g = (f32)c.g / 255.0f;
        const f32 b = (f32)c.b / 255.0f;
        const f32 mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
        const f32 mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
        const f32 d = mx - mn;
        if (d <= 1e-6f) return 0.0f;
        f32 h;
        if (mx == r)      h = ((g - b) / d) + (g < b ? 6.0f : 0.0f);
        else if (mx == g) h = ((b - r) / d) + 2.0f;
        else              h = ((r - g) / d) + 4.0f;
        return h * 60.0f;
    }

    void Theme::set_accent_rgb(u32 rgb_value)
    {
        accent_color = Color::rgb(rgb_value);
        const bool is_dark = (appearance == Appearance::Dark);
        accent_hover = is_dark ? accent_state(accent_color, 0.12f) : accent_state(accent_color, -0.06f);
        accent_active = is_dark ? accent_state(accent_color, -0.10f) : accent_state(accent_color, -0.14f);
        text_on_accent = (accent_color.luminance() > 0.6f) ? Color{0, 0, 0, 255}
                                                          : Color{255, 255, 255, 255};
        accent_hue = hue_from_rgb(accent_color);
    }

    void Theme::init_light()
    {
        window_bg = rgb(0xECECEC);
        sidebar_bg = rgb(0xF6F6F6);
        sidebar_text = rgb(0x1D1D1F);
        sidebar_text_dim = rgb(0x8E8E93);

        glass_tint = rgb(0xFFFFFF, 0x66);
        glass_tint_dark = rgb(0x1C1C1E, 0x99);
        glass_blur = 24.0f;
        glass_opacity = 0.72f;
        glass_saturation = 1.4f;

        text = rgb(0x1D1D1F);
        text_secondary = rgb(0x515154);
        text_tertiary = rgb(0x8E8E93);

        control_bg = Color{255, 255, 255, 0xE6};
        control_bg_hover = Color{255, 255, 255, 0xF2};
        control_bg_active = Color{255, 255, 255, 0xFF};
        control_bg_disabled = Color{255, 255, 255, 0x80};
        control_border = Color{0, 0, 0, 0x1F};
        separator = Color{0, 0, 0, 0x14};
        selection = rgb(0x0A84FF, 0x40);

        corner_window = 12.0f;
        corner_panel = 10.0f;
        corner_control = 7.0f;
        corner_field = 7.0f;

        control_height = 24.0f;
        control_height_small = 18.0f;
        control_height_large = 32.0f;

        sidebar_width = 200.0f;
        titlebar_height = 38.0f;
        row_height = 28.0f;

        spacing_xs = 4.0f;
        spacing_sm = 8.0f;
        spacing_md = 12.0f;
        spacing_lg = 20.0f;
        hairline = 0.5f;

        font_caption = 10.0f;
        font_small = 11.0f;
        font_body = 13.0f;
        font_callout = 15.0f;
        font_title3 = 20.0f;
        font_title2 = 22.0f;
        font_title1 = 28.0f;

        anim_fast = 0.12f;
        anim_normal = 0.20f;
        anim_slow = 0.36f;
    }

    void Theme::init_dark()
    {
        window_bg = rgb(0x1C1C1E);
        sidebar_bg = rgb(0x252528);
        sidebar_text = rgb(0xF5F5F7);
        sidebar_text_dim = rgb(0x98989D);

        glass_tint = rgb(0x2C2C2E, 0x99);
        glass_tint_dark = rgb(0x000000, 0xB3);
        glass_blur = 30.0f;
        glass_opacity = 0.62f;
        glass_saturation = 1.6f;

        text = rgb(0xF5F5F7);
        text_secondary = rgb(0xBFBFC4);
        text_tertiary = rgb(0x8E8E93);

        control_bg = Color{255, 255, 255, 0x14};
        control_bg_hover = Color{255, 255, 255, 0x1F};
        control_bg_active = Color{255, 255, 255, 0x2B};
        control_bg_disabled = Color{255, 255, 255, 0x0A};
        control_border = Color{255, 255, 255, 0x1A};
        separator = Color{255, 255, 255, 0x18};
        selection = rgb(0x0A84FF, 0x59);

        corner_window = 12.0f;
        corner_panel = 10.0f;
        corner_control = 7.0f;
        corner_field = 7.0f;

        control_height = 24.0f;
        control_height_small = 18.0f;
        control_height_large = 32.0f;

        sidebar_width = 200.0f;
        titlebar_height = 38.0f;
        row_height = 28.0f;

        spacing_xs = 4.0f;
        spacing_sm = 8.0f;
        spacing_md = 12.0f;
        spacing_lg = 20.0f;
        hairline = 0.5f;

        font_caption = 10.0f;
        font_small = 11.0f;
        font_body = 13.0f;
        font_callout = 15.0f;
        font_title3 = 20.0f;
        font_title2 = 22.0f;
        font_title1 = 28.0f;

        anim_fast = 0.12f;
        anim_normal = 0.20f;
        anim_slow = 0.36f;
    }

    void Theme::init(Appearance app, Accent ac)
    {
        appearance = app;
        if (app == Appearance::Light)
            init_light();
        else
            init_dark();
        init_accent(ac);
    }

    Theme &theme()
    {
        static Theme t;
        static bool inited = false;
        if (!inited)
        {
            t.init(Appearance::Light, Accent::Blue);
            inited = true;
        }
        return t;
    }

    void set_theme(Appearance app, Accent ac)
    {
        theme().init(app, ac);
    }

}