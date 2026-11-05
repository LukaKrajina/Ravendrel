#pragma once
// Fox widget/primitives.hpp - 原子控件图元
//
// 最小的可复用 UI 原子：按钮、标签、图标、分隔线、复选框、单选、开关、
// 滑块、文本框。每个都是自包含的函数，把自己渲染进显示列表并报告交互。
// 更高层的控件（列表、标签页、菜单）由这些组合而成。
//
// 每个控件：
//   * 收一个显式 Rect（绝对定位）或使用布局游标
//   * 从（salt，label）推导稳定 Id，状态跨帧保留
//   * 按主题的动效曲线把悬停/按压动画向目标值插值
//
// 控件画进上下文的显示列表；这里不接触 Vulkan。

#include "core/context.hpp"

namespace fox
{

    // ── 标签 ───────────────────────────────────────────────────────────

    /// 画单行文本标签。返回其像素宽度。
    f32 label(Context &c, const Rect &r, const char *text, Color color,
              Align align = Align::Start);

    /// 用当前布局游标画单行标签。
    f32 label_layout(Context &c, FontId font, const char *text, Color color,
                     f32 height = 0.0f);

    // ── 图片 ───────────────────────────────────────────────────────────
    //
    // Fox 的图片图元。所有 macOS 形态的东西最终都需要它：图标、logo、头像，
    // 还有——因为不能假定有 SF Symbols 字体——表现得和图片一样的着色字形图。

    /// 采样纹理所用的着色。白色则原样显示。
    inline const Color IMAGE_TINT = {255, 255, 255, 255};

    /// 把 `tex` 拉伸到 `r`，乘以 `tint`。
    ///
    /// 拉伸是刻意的，与 UI 素材的制作方式一致：图标图集的格子按槽位大小
    /// 制作，而不是按目标大小。句柄无效时返回 false（且什么都不画），
    /// 过期的 TexId 只是留个空位，而不是去采样碰巧绑定的纹理。
    ///
    /// 采样是 CLAMP_TO_EDGE + LINEAR，任意非 2 的幂素材都安全；UV 超出
    /// 0..1 时截断而不是环绕。
    bool image(Context &c, const Rect &r, TexId tex, Color tint = IMAGE_TINT);

    /// 画图标图集的一格。`src` 是 `tex` 内的归一化区域（{0,0,0.25,1} 是
    /// 左边四分之一）。macOS 规模的 UI 就是这样画图标，不用每个图标一张
    /// 纹理。
    bool image_region(Context &c, const Rect &r, TexId tex, const Rect &src,
                      Color tint = IMAGE_TINT);

    // ── 按钮 ──────────────────────────────────────────────────────────

    enum class ButtonStyle : u8
    {
        Plain,      ///< 仅文字
        Filled,     ///< 强调色填充
        Tinted,     ///< 浅强调色背景
        Bezel,      ///< 经典 macOS 边框/控件
    };

    /// 按压按钮。被点击的那一帧返回 true。
    /// 用 macOS 风格的按压反馈：整个控件轻微着色 + 缩放。
    bool button(Context &c, const Rect &r, const char *text, ButtonStyle style = ButtonStyle::Tinted,
                bool enabled = true);

    /// 在圆角矩形下投一层柔和阴影，自己不画任何填充。
    ///
    /// 表达层次感就用它：随后画的卡片是一条普通的、完全独立的命令。
    ///
    /// ⚠ 不要把 Shadow 挂到普通 Rect 上再指望它的填充。Cmd::color 默认是
    ///   不透明黑色，带阴影但没颜色的 Rect 会画出一块黑板——而阴影实例又
    ///   画在同一个矩形上，黑板看起来就像控件周围一圈硬黑边。这不是什么
    ///   细微的伪影；它是这个 API 最让人困惑的行为，这个辅助函数就是为此
    ///   存在的。
    ///
    /// 顺序有讲究：在画形状的命令**之前**调用。
    void shadow_rect(Context &c, const Rect &r, const CornerRadii &radii, const Shadow &sh,
                     f32 z = 0.0f);

    // ── 开关（复选框 / 切换开关）────────────────────────────────────

    /// macOS 风格切换开关。本帧被切换时返回 true。
    bool toggle_switch(Context &c, const Rect &r, bool &value);
    /// 经典复选框。本帧被切换时返回 true。
    bool checkbox(Context &c, const Rect &r, const char *label, bool &value);

    // ── 滑块 ─────────────────────────────────────────────────────────

    enum class SliderStyle : u8
    {
        Continuous, ///< 在轨道上任意位置拖动
        Stepped,    ///< 离散步进
    };

    /// 水平滑块。就地修改 value，截断到 [min,max]。拖动中返回 true。
    bool slider(Context &c, const Rect &r, float &value, float min, float max,
                SliderStyle style = SliderStyle::Continuous, float step = 0.0f);

    // ── 文本框 ─────────────────────────────────────────────────────────

    /// 单行可编辑文本框。
    /// `buffer` 是 `capacity` 字节的可写 char 数组。
    /// 拥有键盘焦点时返回 true。
    ///
    /// `id_hint` 固定控件标识。留 0 则 Fox 按字段位置推导稳定 id，这样同屏
    /// 的两个字段才是两个字段，而不是共用一个焦点环和一个光标。字段可能在
    /// 帧间移动、又要保留状态时，传显式 id。
    bool text_field(Context &c, const Rect &r, char *buffer, int capacity,
                    const char *placeholder = nullptr, Id id_hint = 0);

    /// 多行文本区。聚焦时返回 true。
    ///
    /// 是独立控件而不是 text_field() 上的 `multiline` 标志。两者确实不同
    /// ——逐行布局、双轴滚动、滚动条、虚拟化绘制、Enter 插入换行、上下键
    /// 按行移动——在一个 150 行的单行函数里穿一个标志，只会让两者都比
    /// "两个函数各做一件事"更难读。共享的部分由构造保证：同一个 TextBuffer、
    /// 同一套 widget_id() 方案、同一个 salt 族。
    ///
    /// 软换行：长行跨视觉行折断（优先按词、CJK 硬断）。编辑模型只认 '\n'
    /// 作分隔——TextBuffer 的 "lines" 一节解释了为什么换行是布局的属性
    /// 而不是缓冲的属性——所以这里一切依赖"行"的查询（命中测试、光标位置、
    /// 上下键、Home/End、选区带、滚动条比例）都按视觉行工作。传 wrap=false
    /// 恢复旧行为（一个逻辑行一行）。
    ///
    /// `buffer` 仍是调用方的固定数组，写回时截断，与 text_field 相同。
    bool text_area(Context &c, const Rect &r, char *buffer, int capacity,
                   const char *placeholder = nullptr, Id id_hint = 0, bool wrap = true);

    // ── 装饰 ────────────────────────────────────────────────────────────

    /// 细线分隔线。
    void separator(Context &c, const Rect &r, bool horizontal = true);
    /// 圆角面板背景（可选玻璃）。
    void panel(Context &c, const Rect &r, Color fill, float radius,
               bool glass = false);
    /// 玻璃工具栏/标题栏材质（模糊其背后的一切）。
    void glass_bar(Context &c, const Rect &r, float radius = 0.0f);

} // namespace fox