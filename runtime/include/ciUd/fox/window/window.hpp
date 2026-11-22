#pragma once

#include "core/context.hpp"

namespace fox
{

    enum class LightButton : u8
    {
        Close,
        Minimize,
        Maximize,
        Count
    };

    class Window
    {
    public:
        bool begin(Context &c, const Rect &bounds, const char *title, bool show_sidebar = true);

        Rect content() const { return content_; }
        Rect sidebar() const { return sidebar_; }
        Rect titlebar() const { return titlebar_; }

        bool close_requested() const { return close_requested_; }

    private:
        Rect titlebar_;
        Rect sidebar_;
        Rect content_;

        bool show_sidebar_ = true;
        bool close_requested_ = false;
    };

    struct SidebarItem
    {
        const char *label;
        TexId icon_tex = TEX_NONE;
        Rect icon_src{0.0f, 0.0f, 1.0f, 1.0f};
        const char *icon = nullptr;
    };

    int sidebar(Context &c, const Rect &r, const SidebarItem *items, int count, int selected,
                int &scroll_out);

    int traffic_lights(Context &c, const Vec2 &origin);

}