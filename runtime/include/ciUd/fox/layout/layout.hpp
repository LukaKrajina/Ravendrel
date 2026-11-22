#pragma once

#include "core/types.hpp"
#include "input/input.hpp"
#include <vector>
#include <unordered_map>

namespace fox
{

    enum class Direction : u8
    {
        Row,
        Column,
    };

    enum class Align : u8
    {
        Start,
        Center,
        End,
        Stretch,
    };

    struct Layout
    {
        Rect bounds;
        Vec2 cursor;
        f32 cursor_x = 0.0f;
        f32 cursor_y = 0.0f;

        f32 line_height = 0.0f;
        f32 x = 0.0f;
        f32 y = 0.0f;
        f32 row_start_y = 0.0f;

        f32 spacing = 0.0f;
        Direction dir = Direction::Column;
        f32 total_spacing = 0.0f;

        f32 max_used_x = 0.0f;
        f32 max_used_y = 0.0f;
    };

    class LayoutSystem
    {
    public:
        void begin(const Rect &bounds, Direction dir, f32 spacing);
        void end();

        Rect allocate(const Vec2 &size, Align align = Align::Start);

        Rect row(int count, f32 height);
        Rect column_rect(int i);

        Rect scroll_area(const Rect &area, Id id, InputState &input, f32 content_height,
                         f32 &out_scroll);

        f32 available_width() const;
        f32 available_height() const;

        Rect cursor_rect() const { return Rect{l.x, l.y, available_width(), available_height()}; }

        const Layout &current() const { return l; }

    private:
        Layout l;
        struct RowInfo
        {
            Rect rect;
            int count = 0;
            f32 gap = 0.0f;
        };
        RowInfo last_row_;
        struct StackEntry
        {
            Layout saved;
        };
        std::vector<StackEntry> stack_;
        struct ScrollEntry
        {
            f32 offset = 0.0f;
        };
        std::unordered_map<Id, ScrollEntry> scrolls_;
    };

}