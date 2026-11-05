#pragma once
#include "core/context.hpp"

namespace fox
{
    bool menu_title(Context &c, const Rect &r, const char *label);
    Rect menu_popup(Context &c, const Rect &anchor, int item_count,
                    f32 item_height = 22.0f, f32 min_width = 120.0f);
    bool menu_item(Context &c, const Rect &r, const char *label, bool enabled = true);

    void menu_close_all(Context &c);

    bool combo(Context &c, const Rect &r, const char *const *items, int count,
               int &selected);

    bool selectable(Context &c, const Rect &r, const char *label, bool &selected);

    Rect group_begin(Context &c, const Rect &r, const char *title, Id id,
                     f32 content_height);
                     
    void group_end(Context &c);

    bool property_int(Context &c, const Rect &r, const char *label, int &value,
                      int min, int max, int step = 1);
    bool property_float(Context &c, const Rect &r, const char *label, float &value,
                        float min, float max, float step = 0.1f);
}