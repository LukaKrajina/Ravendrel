#pragma once
// Fox window/window.hpp - macOS 27 窗口边框
//
// 组装标准 macOS 窗口：带红绿灯按钮的玻璃标题栏、可选的鲜艳材质侧栏、
// 工具栏和内容区。控件就是放进这个"框架"里的。
//
// 这里建模的 Golden Gate 细节：
//   · 标题栏是 Liquid Glass——模糊其下滚动的内容。
//   · 红绿灯按钮带淡淡的玻璃质感（不是纯色圆点），悬停时整组一起亮。
//   · 侧栏是实心鲜艳材质，不是玻璃——Tahoe 的悬浮泡泡侧栏是被批评最多
//     的地方，27 里改回来了。
//   · 侧栏行可以带强调色图标。

#include "core/context.hpp"

namespace fox
{

    /// 红绿灯按钮状态。
    enum class LightButton : u8
    {
        Close,
        Minimize,
        Maximize,
        Count
    };

    /// 整个窗口：标题栏 + 可选侧栏 + 内容区。
    class Window
    {
    public:
        /// 开始本帧的窗口。窗口折叠/最小化时返回 false（调用方应跳过主体）。
        bool begin(Context &c, const Rect &bounds, const char *title, bool show_sidebar = true);

        /// 内容区矩形（标题栏以下、除侧栏外的一切）。
        Rect content() const { return content_; }
        /// 侧栏矩形（隐藏时为空）。
        Rect sidebar() const { return sidebar_; }
        Rect titlebar() const { return titlebar_; }

        /// 本帧点击了关闭按钮则为 true。
        bool close_requested() const { return close_requested_; }

    private:
        Rect titlebar_;
        Rect sidebar_;
        Rect content_;

        bool show_sidebar_ = true;
        bool close_requested_ = false;
    };

    /// 画 macOS 风格侧栏。返回被点击项的下标，没有则 -1。
    ///
    /// 条目图标要么是采样图片（正路），要么是单个字形（回退）。字形形式
    /// 存在是因为文本图标没法脱离字体单独着色或缩放，只能照搬标签字体的
    /// 光栅化结果——所以它是第二选择而不是首选。
    struct SidebarItem
    {
        const char *label;
        TexId icon_tex = TEX_NONE; ///< 图标图集，TEX_NONE 表示无图片图标
        Rect icon_src{0.0f, 0.0f, 1.0f, 1.0f}; ///< `icon_tex` 内的归一化格子
        const char *icon = nullptr; ///< 字形回退，或 null
    };

    int sidebar(Context &c, const Rect &r, const SidebarItem *items, int count, int selected,
                int &scroll_out);

    /// 画红绿灯按钮组。返回被点击的按钮（没有则 -1）。
    int traffic_lights(Context &c, const Vec2 &origin);

} // namespace fox