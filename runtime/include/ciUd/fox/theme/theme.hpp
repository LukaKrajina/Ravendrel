#pragma once

#include "core/types.hpp"

namespace fox
{


    enum class Accent : u8
    {
        Multicolor = 0,
        Blue,
        Purple,
        Pink,
        Red,
        Orange,
        Yellow,
        Green,
        Graphite,
        Count
    };


    enum class Appearance : u8
    {
        Light,
        Dark,
    };


    struct Theme
    {
        Appearance appearance = Appearance::Light;
        Accent accent = Accent::Blue;
        f32 accent_hue = 211.0f;

        Color window_bg;
        Color sidebar_bg;
        Color sidebar_text;
        Color sidebar_text_dim;

        Color glass_tint;
        Color glass_tint_dark;
        f32 glass_blur;
        f32 glass_opacity;
        f32 glass_saturation;

        Color text;
        Color text_secondary;
        Color text_tertiary;
        Color text_on_accent;

        Color control_bg;
        Color control_bg_hover;
        Color control_bg_active;
        Color control_bg_disabled;
        Color control_border;
        Color separator;
        Color selection;

        Color accent_color;
        Color accent_hover;
        Color accent_active;

        f32 corner_window;
        f32 corner_panel;
        f32 corner_control;
        f32 corner_field;

        f32 control_height;
        f32 control_height_small;
        f32 control_height_large;

        f32 sidebar_width;
        f32 titlebar_height;
        f32 row_height;

        f32 spacing_xs, spacing_sm, spacing_md, spacing_lg;
        f32 hairline;

        f32 font_caption;
        f32 font_small;
        f32 font_body;
        f32 font_callout;
        f32 font_title3;
        f32 font_title2;
        f32 font_title1;

        f32 anim_fast;
        f32 anim_normal;
        f32 anim_slow;

        void init(Appearance app, Accent ac);

        void set_accent_rgb(u32 rgb);


    private:
        void init_light();
        void init_dark();
        void init_accent(Accent ac);
    };

    Theme &theme();
    void set_theme(Appearance app, Accent ac);

    f32 accent_hue(Accent a);

}