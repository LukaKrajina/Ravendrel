// Fox layout/layout.cpp - 立即模式布局游标

#include "layout.hpp"
#include <algorithm>

namespace fox
{

    void LayoutSystem::begin(const Rect &bounds, Direction dir, f32 spacing)
    {
        // 保存外层布局，嵌套容器可以恢复。
        stack_.push_back(StackEntry{l});

        l = Layout{};
        l.bounds = bounds;
        l.dir = dir;
        l.spacing = spacing;
        l.cursor_x = bounds.x;
        l.cursor_y = bounds.y;
        l.row_start_y = bounds.y;
        l.x = bounds.x;
        l.y = bounds.y;
        l.max_used_x = bounds.x;
        l.max_used_y = bounds.y;
    }

    void LayoutSystem::end()
    {
        if (!stack_.empty())
        {
            l = stack_.back().saved;
            stack_.pop_back();
        }
    }

    f32 LayoutSystem::available_width() const
    {
        return l.dir == Direction::Row ? fx_max(0.0f, l.bounds.right() - l.cursor_x)
                                       : l.bounds.w;
    }

    f32 LayoutSystem::available_height() const
    {
        return l.dir == Direction::Column ? fx_max(0.0f, l.bounds.bottom() - l.cursor_y)
                                         : l.bounds.h;
    }

    Rect LayoutSystem::allocate(const Vec2 &size, Align align)
    {
        f32 w = size.x;
        f32 h = size.y;

        if (l.dir == Direction::Row)
        {
            f32 avail = available_width();
            if (w <= 0.0f || (align == Align::Stretch)) w = avail;
            w = fx_min(w, avail);

            f32 y = l.cursor_y;
            if (h > 0.0f)
            {
                switch (align)
                {
                case Align::Start: y = l.cursor_y; break;
                case Align::Center: y = l.cursor_y + (l.line_height - h) * 0.5f; break;
                case Align::End: y = l.cursor_y + (l.line_height - h); break;
                default: break;
                }
            }

            Rect r{l.cursor_x, y, w, h};
            l.cursor_x += w + l.spacing;
            l.max_used_x = fx_max(l.max_used_x, r.right());
            l.max_used_y = fx_max(l.max_used_y, r.bottom());
            if (r.bottom() > l.cursor_y + l.line_height) l.line_height = r.bottom() - l.cursor_y;
            return r;
        }
        else // Column
        {
            f32 avail_h = available_height();
            if (h <= 0.0f || (align == Align::Stretch)) h = fx_min(l.line_height > 0 ? l.line_height : 0, avail_h);
            if (h <= 0.0f) h = size.y;
            h = fx_min(h, avail_h);

            f32 x = l.cursor_x;
            if (align == Align::Center) x = l.bounds.x + (l.bounds.w - w) * 0.5f;
            else if (align == Align::End) x = l.bounds.x + (l.bounds.w - w);

            Rect r{x, l.cursor_y, w, h};
            l.cursor_y += h + l.spacing;
            l.max_used_x = fx_max(l.max_used_x, r.right());
            l.max_used_y = fx_max(l.max_used_y, r.bottom());
            return r;
        }
    }

    Rect LayoutSystem::row(int count, f32 height)
    {
        if (count <= 0) count = 1;
        f32 avail = l.bounds.right() - l.cursor_x;
        f32 gap = l.spacing * (f32)(count - 1);
        f32 cell = (avail - gap) / (f32)count;
        if (cell < 0.0f) cell = 0.0f;

        last_row_.rect = Rect{l.cursor_x, l.cursor_y, avail, height};
        last_row_.count = count;
        last_row_.gap = l.spacing;

        // 按列块推进游标（各行垂直堆叠）。
        l.cursor_y += height + l.spacing;
        l.max_used_y = fx_max(l.max_used_y, l.cursor_y);
        return last_row_.rect;
    }

    Rect LayoutSystem::column_rect(int i)
    {
        int n = last_row_.count;
        if (n <= 0 || i < 0 || i >= n) return Rect{};
        f32 cell = (last_row_.rect.w - last_row_.gap * (f32)(n - 1)) / (f32)n;
        f32 x = last_row_.rect.x + (cell + last_row_.gap) * (f32)i;
        return Rect{x, last_row_.rect.y, cell, last_row_.rect.h};
    }

    Rect LayoutSystem::scroll_area(const Rect &area, Id id, InputState &input, f32 content_height,
                                   f32 &out_scroll)
    {
        ScrollEntry &se = scrolls_[id];
        const f32 max_scroll = fx_max(0.0f, content_height - area.h);

        // 滚轮滚动指针所在的区域。每个区域有自己的偏移，侧栏列表和主列表
        // 可以独立滚动。
        if (area.contains(input.mouse_pos) && input.scroll_y != 0.0f)
        {
            se.offset -= input.scroll_y * 40.0f;
        }
        se.offset = fx_clamp(se.offset, 0.0f, max_scroll);
        out_scroll = se.offset;

        return area;
    }

} // namespace fox