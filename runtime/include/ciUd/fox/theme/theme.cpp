// Fox theme/theme.cpp - Golden Gate token 数值
//
// 纯数据 + 简单 setter。不放头文件里，免得 token 表撑大每个编译单元。

#include "theme.hpp"

namespace fox
{

    // 强调色色相（近似 macOS 系统强调色值）。
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

    // 把基础强调色 + 强度转成带色调的颜色，对应 macOS 从单一强调色派生
    // hover/active 状态的做法。
    static Color accent_state(Color base, f32 amount)
    {
        // 浅色模式：hover 变亮，active 略变暗。这里用向白（hover）或向黑
        // （active）混合来近似。
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
        // 深色模式下强调色 hover 变亮；浅色模式下变暗。
        accent_hover = is_dark ? accent_state(accent_color, 0.12f) : accent_state(accent_color, -0.06f);
        accent_active = is_dark ? accent_state(accent_color, -0.10f) : accent_state(accent_color, -0.14f);
        text_on_accent = (accent_color.luminance() > 0.6f) ? Color{0, 0, 0, 255}
                                                          : Color{255, 255, 255, 255};
    }

    /// 颜色的色相（度），供渐变 / 色调运算用。
    static f32 hue_from_rgb(Color c)
    {
        const f32 r = (f32)c.r / 255.0f;
        const f32 g = (f32)c.g / 255.0f;
        const f32 b = (f32)c.b / 255.0f;
        const f32 mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
        const f32 mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
        const f32 d = mx - mn;
        if (d <= 1e-6f) return 0.0f;   // 无彩色：色相无定义，取 0
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
        // 与 init_accent() 同一套派生：暗色下 hover 变亮，亮色下变暗。
        accent_hover = is_dark ? accent_state(accent_color, 0.12f) : accent_state(accent_color, -0.06f);
        accent_active = is_dark ? accent_state(accent_color, -0.10f) : accent_state(accent_color, -0.14f);
        // 亮色强调上的文字必须是深色，否则读不出来。
        //
        // ⚠ 之前这两个分支**完全相同**（都是白色）—— 一个假分支：任何强调色
        // 都会得到白字，而亮色强调（如黄色）上白字是看不见的。
        text_on_accent = (accent_color.luminance() > 0.6f) ? Color{0, 0, 0, 255}
                                                          : Color{255, 255, 255, 255};
        accent_hue = hue_from_rgb(accent_color);
    }

    void Theme::init_light()
    {
        window_bg = rgb(0xECECEC);
        sidebar_bg = rgb(0xF6F6F6); // macOS 27：接近不透明，不是玻璃
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

        // Golden Gate 几何：宽松且一致的圆角。
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
        sidebar_bg = rgb(0x252528); // macOS 27 深色侧栏：实心、抬升
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
        // 这里不做 DPI 处理，也不需要：主题 token 以逻辑单位编写，只在
        // 后端缩放一次。早先的版本在这里也缩放，2x 下所有尺寸翻倍。
        theme().init(app, ac);
    }

} // namespace fox