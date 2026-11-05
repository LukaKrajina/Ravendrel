#pragma once
// Fox theme/theme.hpp - macOS 27（Golden Gate）设计 token
//
// Fox 的视觉定义在这里以纯数据存在：调色板、度量和动效曲线，用 macOS 27
// "Golden Gate" 的设计语言表达——它改进了 Apple 在 Tahoe 26 引入的
// Liquid Glass 材质。
//
// Tahoe -> Golden Gate 的变化（Fox 建模的差异）：
//   · 玻璃不透明度降低 + 扩散更好；边缘是深色边圈加更亮的高光，不再是
//     均匀的奶白观感。
//   · 侧栏回到实心/鲜艳材质，不再是悬浮玻璃泡泡。侧栏图标带强调色。
//   · 圆角变大，且控件与窗口之间一致。
//   · 窗口红绿灯按钮带上淡淡的玻璃质感。
//   · 更有层次 + 微交互（悬停抬升、焦点环、弹性动效）。
//
// 这里全是普通值，主题可以运行时切换，整套配色也能在调试覆盖层里查看。

#include "core/types.hpp"

namespace fox
{

    // ── 强调色 ──────────────────────────────────────────────────────

    /// macOS 系统强调色板。强调色为控件、选区和焦点着色；
    /// 用户期待的就是这些色相。
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

    // ── 外观（浅色 / 深色）─────────────────────────────────────────

    enum class Appearance : u8
    {
        Light,
        Dark,
    };

    // ── 主题 ──────────────────────────────────────────────────────────

    struct Theme
    {
        Appearance appearance = Appearance::Light;
        Accent accent = Accent::Blue;
        f32 accent_hue = 211.0f; ///< 角度，用于着色/渐变

        // ── 基础表面 ────────────────────────────────────────────
        Color window_bg;          ///< 窗口内容背景
        Color sidebar_bg;         ///< macOS 27：不透明的鲜艳侧栏
        Color sidebar_text;
        Color sidebar_text_dim;

        // ── 玻璃（Liquid Glass，Golden Gate 调校）──────────────────
        Color glass_tint;         ///< 铺在模糊背景上的着色
        Color glass_tint_dark;
        f32 glass_blur;           ///< 背景模糊半径，像素
        f32 glass_opacity;        ///< 0..1 着色透出的程度
        f32 glass_saturation;     ///< 背景饱和度提升（鲜艳度）

        // ── 文本 ─────────────────────────────────────────────────
        Color text;               ///< 主标签
        Color text_secondary;
        Color text_tertiary;
        Color text_on_accent;

        // ── 控件 ─────────────────────────────────────────────────
        Color control_bg;         ///< 控件静止态填充
        Color control_bg_hover;
        Color control_bg_active;
        Color control_bg_disabled;
        Color control_border;
        Color separator;          ///< 细线分隔
        Color selection;          ///< 选中行 / 文本选区

        // ── 强调色派生 ───────────────────────────────────────────
        Color accent_color;
        Color accent_hover;
        Color accent_active;

        // ── 度量（Golden Gate：更圆、更宽松）──────────────────
        f32 corner_window;        ///< 窗口圆角半径
        f32 corner_panel;
        f32 corner_control;
        f32 corner_field;

        f32 control_height;       ///< 标准控件高度
        f32 control_height_small;
        f32 control_height_large;

        f32 sidebar_width;
        f32 titlebar_height;
        f32 row_height;           ///< 侧栏 / 列表行高

        f32 spacing_xs, spacing_sm, spacing_md, spacing_lg;
        f32 hairline;             ///< 1x 屏 0.5，2x 屏 1.0

        // ── 字号阶梯（pt；1x 时即 px）──────────────────────
        //
        // ⚠ 这里每个字段都必须由 init_light()/init_dark() 赋值。这里的 0
        //   不是"默认值"——它会流进 Context::font(0)，加载一个退化的字面，
        //   把每个标签都渲染成零宽。症状（所有文字不可见）完全指向不了
        //   主题，所以值得写下来。
        f32 font_caption;         ///< 10 - 脚注
        f32 font_small;           ///< 11 - 说明文字、侧栏
        f32 font_body;            ///< 13 - 默认 UI 文字
        f32 font_callout;         ///< 15 - 控件标签
        f32 font_title3;          ///< 20
        f32 font_title2;          ///< 22
        f32 font_title1;          ///< 28 - 窗口标题

        // ── 动效 ───────────────────────────────────────────────────
        f32 anim_fast;            ///< ~120ms 悬停/按压
        f32 anim_normal;          ///< ~200ms 标准
        f32 anim_slow;            ///< ~360ms 窗口/材质过渡

        /// 按给定外观/强调色应用 Golden Gate 默认值。
        void init(Appearance app, Accent ac);

        /// 设置任意 RGB 强调色（0xRRGGBB），并由它派生
        /// hover / active / text-on-accent 颜色。
        ///
        /// 为什么需要它：Accent 枚举只覆盖 macOS 那套配色，而宿主可能要用
        /// 自己的品牌色。`fox_set_accent` 之前接受一个 RGB 却**把它丢掉**
        ///（还顺手把整个主题重置成蓝色），于是「设一个自定义强调色」这件事
        /// 根本做不到。
        ///
        /// 与 init_accent() 不同：它**不重置主题其余部分**，只换强调色派生
        /// 的那几个颜色。
        void set_accent_rgb(u32 rgb);

        // ⚠ 上面每个长度 token 都以逻辑单位编写，绝不按显示密度缩放。
        //   这里刻意没有 ScaleAllSizes()。
        //
        //   Fox 只有一个转换点——VkBackend::build_instances 在送往 GPU 的
        //   路上把每个矩形、半径、粗细和阴影乘以 DPI。token 再缩放一遍
        //   （ImGui 的做法，因为它没有单独的转换点）会让所有东西翻倍：
        //   scale 2 下 200pt 的侧栏变成 400 逻辑、再变 800 设备像素，
        //   UI 溢出自己的窗口。
        //
        //   主题保持基准值也让它可读：这里的数字就是设计师说的那个数字。
        //
        //   hairline 是唯一看着像例外的值——1x 时 0.5、2x 时 1.0——但它
        //   就是个长度，单一缩放点处理它。它始终保持一个设备像素，这正是
        //   要点。

    private:
        void init_light();
        void init_dark();
        void init_accent(Accent ac);
    };

    /// 进程级主题（只有一套 UI，主题栈就过了）。
    Theme &theme();
    void set_theme(Appearance app, Accent ac);

    /// 给定强调色的色相（角度），用于渐变/着色计算。
    f32 accent_hue(Accent a);

} // namespace fox