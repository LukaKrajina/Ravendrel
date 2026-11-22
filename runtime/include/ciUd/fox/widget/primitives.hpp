#pragma once

#include "core/context.hpp"

namespace fox
{


    f32 label(Context &c, const Rect &r, const char *text, Color color,
              Align align = Align::Start);

    f32 label_layout(Context &c, FontId font, const char *text, Color color,
                     f32 height = 0.0f);


    inline const Color IMAGE_TINT = {255, 255, 255, 255};

    bool image(Context &c, const Rect &r, TexId tex, Color tint = IMAGE_TINT);

    bool image_region(Context &c, const Rect &r, TexId tex, const Rect &src,
                      Color tint = IMAGE_TINT);


    enum class ButtonStyle : u8
    {
        Plain,
        Filled,
        Tinted,
        Bezel,
    };

    bool button(Context &c, const Rect &r, const char *text, ButtonStyle style = ButtonStyle::Tinted,
                bool enabled = true);

    void shadow_rect(Context &c, const Rect &r, const CornerRadii &radii, const Shadow &sh,
                     f32 z = 0.0f);


    bool toggle_switch(Context &c, const Rect &r, bool &value);
    bool checkbox(Context &c, const Rect &r, const char *label, bool &value);


    enum class SliderStyle : u8
    {
        Continuous,
        Stepped,
    };

    bool slider(Context &c, const Rect &r, float &value, float min, float max,
                SliderStyle style = SliderStyle::Continuous, float step = 0.0f);


    bool text_field(Context &c, const Rect &r, char *buffer, int capacity,
                    const char *placeholder = nullptr, Id id_hint = 0);

    bool text_area(Context &c, const Rect &r, char *buffer, int capacity,
                   const char *placeholder = nullptr, Id id_hint = 0, bool wrap = true);


    void separator(Context &c, const Rect &r, bool horizontal = true);
    void panel(Context &c, const Rect &r, Color fill, float radius,
               bool glass = false);
    void glass_bar(Context &c, const Rect &r, float radius = 0.0f);

}