#pragma once
// Fox layout/layout.hpp - 立即模式布局游标
//
// 布局系统是矩形上的一个游标：控件放在游标处并推进它。这和 Nuklear 的模型
// 相同（现有 rvdl 代码能平滑移植），但图元更好：带间距的行/列、对齐，
// 以及用于嵌套区域的栈式分配。
//
// 所有矩形都以绝对窗口像素给出。"区域"为游标划出作用域，让子容器能独立
// 布局。

#include "core/types.hpp"
#include "input/input.hpp"
#include <vector>
#include <unordered_map>

namespace fox
{

    enum class Direction : u8
    {
        Row,    ///< 从左到右
        Column, ///< 从上到下
    };

    enum class Align : u8
    {
        Start,
        Center,
        End,
        Stretch,
    };

    /// 限定在矩形内的布局游标。
    struct Layout
    {
        Rect bounds;      ///< 本布局填充的区域
        Vec2 cursor;      ///< 当前笔位
        f32 cursor_x = 0.0f;
        f32 cursor_y = 0.0f;

        f32 line_height = 0.0f; ///< 当前行高度（最高项）
        f32 x = 0.0f;
        f32 y = 0.0f;
        f32 row_start_y = 0.0f;

        f32 spacing = 0.0f;    ///< 当前行内各项的间距
        Direction dir = Direction::Column;
        f32 total_spacing = 0.0f;

        f32 max_used_x = 0.0f;
        f32 max_used_y = 0.0f;
    };

    /// 运行时布局状态（一个游标 + 区域栈）。
    class LayoutSystem
    {
    public:
        void begin(const Rect &bounds, Direction dir, f32 spacing);
        void end();

        /// 在游标处按给定大小预留控件；返回其矩形。推进游标。
        Rect allocate(const Vec2 &size, Align align = Align::Start);

        /// 分配一行 `count` 列，填满可用宽度。返回整行高度。
        /// 用 column_rect() 取每个单元格。
        Rect row(int count, f32 height);
        /// 最近一次 row() 的第 i 格矩形。
        Rect column_rect(int i);

        /// 可滚动区域：返回裁剪后的内容矩形和要应用的滚动偏移。
        /// 自行管理按 id 记录的滚动状态。
        Rect scroll_area(const Rect &area, Id id, InputState &input, f32 content_height,
                         f32 &out_scroll);

        /// 当前行内的可用空间。
        f32 available_width() const;
        f32 available_height() const;

        /// 当前游标矩形（调试覆盖层用）。
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

} // namespace fox