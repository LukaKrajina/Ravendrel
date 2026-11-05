// ═══════════════════════════════════════════════════════════════════════
// Bro 桌面端 UI 原语（rvdl_ui_*）
// ═══════════════════════════════════════════════════════════════════════
//
// 为什么必须放在 rvdl_rt 里面，而不能由外部宿主自己编一份 Nuklear：
//
//   ClassicGui.hpp 里的 cgui_state() 是个 inline 函数里的 static，
//   C++ 只保证「同一个程序」内唯一。DLL 是独立模块，外部宿主拿到的是
//   **另一份** cgui_state()，impl 恒为 nullptr —— 早期版本就是这么栽的：
//   能编译、能链接、跑起来静默无窗口。
//
//   所以但凡是碰得到 cgui_state().impl / RenderContext::ctx() 的代码，
//   都得和 ClassicGfx.cpp 在同一个模块里。
//
// ── 补的是哪些缺口 ─────────────────────────────────────────────────────
//
// 现有 cgui_*/cgfx_* 能画形状，但缺三样：
//   1. 绝对定位 + 带颜色的文本。cgui_text 走 nk_label，是流式布局，
//      位置由 Nuklear 内部游标决定，没法做三栏布局。
//   2. 键盘。整个 cgui_* 里一个键盘函数都没有。
//   3. 滚轮 / 帧时序 / 文本量宽。
//
// ── 已知的固有限制（Nuklear 1.32 行为，非本文件缺陷）──────────────────
//
//   nk_draw_text 内部写死 cmd->height = font->height，
//   而字体在 atlas 烘焙时就是 16px。所以**所有文字等高**，
//   视觉层级只能靠颜色、字重、背景条和位置来表达。
//
// 编译目标：随 rvdl_rt 一起构建（见 runtime/CMakeLists.txt）

#pragma once
#include "RuntimeApi.h"
#include <cstdint>

#ifndef RVDL_HOST_EXPORT
#define RVDL_HOST_EXPORT extern "C" RVDL_RT_API
#endif

// 文本槽（宿主 → rvdl）由 bro_shell 注册，不属于 rvdl_rt，本文件不声明。
//   gh_text(slot) / gh_slot_len(slot) / gh_slot_int(slot)
// 可变字符串缓冲见 BroStr.hpp。

extern "C"
{
    // ── 帧 ────────────────────────────────────────────────────────────
    /// 帧开始：推进帧计数。rvdl 在 begin_frame 之前调一次。
    RVDL_HOST_EXPORT int32_t rvdl_ui_begin(void);
    RVDL_HOST_EXPORT int32_t rvdl_ui_frame(void);
    RVDL_HOST_EXPORT double rvdl_ui_dt_ms(void);
    RVDL_HOST_EXPORT double rvdl_ui_time_ms(void);

    // ── 窗口 ──────────────────────────────────────────────────────────
    /// 窗口是否真的创建成功
    RVDL_HOST_EXPORT int32_t rvdl_ui_open(void);
    RVDL_HOST_EXPORT int32_t rvdl_ui_win_w(void);
    RVDL_HOST_EXPORT int32_t rvdl_ui_win_h(void);

    // ── 文本（绝对定位）──────────────────────────────────────────────
    //
    // size 是字号档位（qgui::FontSize），**不是**像素。
    // 字号在 atlas 烘焙时就定死了，运行时改不了 —— 给一个连续 px 参数
    // 只会让人以为能任意缩放，然后拿到没烘焙过的尺寸（豆腐块）。
    //
    //   rvdl_ui_text  (x, y, size, color, text)
    //   rvdl_ui_textw (x, y, w, line_height, size, color, text)
    RVDL_HOST_EXPORT int32_t rvdl_ui_text(int32_t x, int32_t y, int32_t size, int32_t color,
                                         const char *text);
    /// 折行绘制，返回行数。line_height <= 0 时用字号高度。
    RVDL_HOST_EXPORT int32_t rvdl_ui_textw(int32_t x, int32_t y, int32_t w, int32_t line_height,
                                          int32_t size, int32_t color, const char *text);
    /// 只算折行后的行数（预排，不产生绘制命令）
    RVDL_HOST_EXPORT int32_t rvdl_ui_text_lines(int32_t w, int32_t size, const char *text);
    /// 文本像素宽（右对齐布局需要先量）
    RVDL_HOST_EXPORT int32_t rvdl_ui_text_width(int32_t size, const char *text);
    /// 该字号档位的高度（px）
    RVDL_HOST_EXPORT int32_t rvdl_ui_text_height(int32_t size);
    /// 实际可用的字号档位数
    RVDL_HOST_EXPORT int32_t rvdl_ui_font_count(void);
    /// 第 size 档的烘焙像素高度（0 = 该档不可用）
    RVDL_HOST_EXPORT int32_t rvdl_ui_font_px(int32_t size);
    /// 图集里烘进去的**字形数**（0 = 还没烘焙），用于核对显存开销。
    /// ⚠ 不是图集宽高 —— 那个拿不到（font_stash_end 之后 tex_width 不可读）。
    RVDL_HOST_EXPORT int32_t rvdl_ui_atlas_info(void);

    // ── 图标（SDF 程序化生成）─────────────────────────────────────────
    //
    // 图标不是图片文件，而是**有符号距离场**（见 qicon.hpp）。
    // 好处：任意缩放都清晰、深色主题下能整体调色、不需要纹理句柄
    // （Nuklear 的 nk_draw_image 需要 Vulkan 纹理，而创建纹理的代码是
    // 头文件里的 NK_INTERN，外部模块调不到）。
    //
    /// 生成第 id 个图标的场。越界返回 0。
    RVDL_HOST_EXPORT int32_t rvdl_icon_build(int32_t id);
    /// 把场画到 (x,y,w,h)。返回发出的矩形数（0 = 空图标）。
    RVDL_HOST_EXPORT int32_t rvdl_icon_draw(int32_t x, int32_t y, int32_t w, int32_t h,
                                           int32_t color, float alpha_scale);
    /// 只算会画多少个矩形（布局预排用）
    RVDL_HOST_EXPORT int32_t rvdl_icon_draw_count(void);

    /// 主场的**填充面积**：场里 d < 0 的格子数。
    ///
    /// 靠矩形数量不出「并集更大 / 交集更小」—— 矩形数只反映水平段
    /// 个数，跟面积没有单调关系（一个大方块和一堆碎块可以发出同样
    /// 多的矩形）。要验证组合代数，得用真实面积。
    RVDL_HOST_EXPORT int32_t rvdl_icon_area(void);
    /// 图标默认尺寸
    RVDL_HOST_EXPORT int32_t rvdl_icon_size(int32_t id);

    // ── 组合代数（rvdl 侧自己拼图标）────────────────────────────────────
    //
    // 这一组必须暴露给 rvdl —— 否则「图标库」只是 15 个写死的形状，
    // 而不是库。用法是「先画形状进草稿场，再 mix 回主场」：
    //
    //   rvdl_icon_reset();
    //   rvdl_icon_shape_ring(16,16,10,3);    // 草稿场 = 一个圆环
    //   rvdl_icon_mix(QICON_OP_UNION);        // 合进主场
    //   rvdl_icon_shape_circle(16,16,4);     // 草稿场 = 中心圆
    //   rvdl_icon_mix(QICON_OP_SUBTRACT);     // 挖孔
    //   rvdl_icon_draw(...)
    //
    // 草稿场与主场分开，是因为布尔运算都是二元的，得有两个操作数。
    // 挤在一块就没法表达 A - B。

    /// 清空主场
    RVDL_HOST_EXPORT void rvdl_icon_reset(void);
    /// 用常量填充主场（调试用）
    RVDL_HOST_EXPORT void rvdl_icon_fill(float v);

    /// 形状原语 —— 画进**草稿场**（不动主场），返回 1 表示成功
    RVDL_HOST_EXPORT int32_t rvdl_icon_shape_rect(float x, float y, float w, float h, float r);
    RVDL_HOST_EXPORT int32_t rvdl_icon_shape_circle(float cx, float cy, float r);
    RVDL_HOST_EXPORT int32_t rvdl_icon_shape_ring(float cx, float cy, float r, float thickness);
    RVDL_HOST_EXPORT int32_t rvdl_icon_shape_segment(float x0, float y0, float x1, float y1,
                                                     float thickness);
    RVDL_HOST_EXPORT int32_t rvdl_icon_shape_triangle(float x0, float y0, float x1, float y1,
                                                     float x2, float y2);
    RVDL_HOST_EXPORT int32_t rvdl_icon_shape_star(float cx, float cy, float r_outer, float r_inner,
                                                 int32_t points, float rot);

    /// 把草稿场混进主场（op 见 qicon.hpp 的 enum qicon_op）。
    /// 返回混完后主场的**面积**（见 rvdl_icon_area）。
    RVDL_HOST_EXPORT int32_t rvdl_icon_mix(int32_t op);

    /// 对主场做仿射（2×3 矩阵）变换
    RVDL_HOST_EXPORT int32_t rvdl_icon_rotate(float cx, float cy, float angle);
    RVDL_HOST_EXPORT int32_t rvdl_icon_scale(float cx, float cy, float sx, float sy);

    // ── 调色（3×3 矩阵 + 偏移）────────────────────────────────────────
    /// 调色板变换：色旋转（度）+ 亮度 + 对比度。返回 0xRRGGBB。
    RVDL_HOST_EXPORT int32_t rvdl_icon_tint(int32_t rgb, float hue_deg, float lightness,
                                           float contrast);

    // ── 形状（补 cgfx_* 的 alpha / 裁剪）─────────────────────────────
    RVDL_HOST_EXPORT int32_t rvdl_ui_rect_a(int32_t x, int32_t y, int32_t w, int32_t h,
                                           int32_t color, int32_t alpha);
    /// 裁剪矩形；之后的绘制被限制在此区域内
    RVDL_HOST_EXPORT int32_t rvdl_ui_clip(int32_t x, int32_t y, int32_t w, int32_t h);
    RVDL_HOST_EXPORT int32_t rvdl_ui_clip_end(void);

    // ── 输入 ──────────────────────────────────────────────────────────
    /// 键盘电平。可打印字符用 ASCII；特殊键用 NK_KEY_* 枚举（见下）。
    RVDL_HOST_EXPORT int32_t rvdl_ui_key_down(int32_t key);
    /// 键盘边沿（内部保存上一帧快照）
    RVDL_HOST_EXPORT int32_t rvdl_ui_key_press(int32_t key);
    /// 帧末刷新全部按键快照
    RVDL_HOST_EXPORT int32_t rvdl_ui_key_sync(void);
    /// 取本帧的一个 Unicode 码点，无输入返回 -1
    RVDL_HOST_EXPORT int32_t rvdl_ui_char(void);
    RVDL_HOST_EXPORT int32_t rvdl_ui_char_pending(void);
    /// 滚轮位移（向上为正），取一次即清零
    RVDL_HOST_EXPORT int32_t rvdl_ui_scroll(void);
    RVDL_HOST_EXPORT int32_t rvdl_ui_mouse_x(void);
    RVDL_HOST_EXPORT int32_t rvdl_ui_mouse_y(void);
    RVDL_HOST_EXPORT int32_t rvdl_ui_mouse_down(void);
    RVDL_HOST_EXPORT int32_t rvdl_ui_mouse_press(void);

    // ── 诊断 ──────────────────────────────────────────────────────────
    /// 位 0 = 画布就绪，位 1 = 字体就绪，位 8+ = 字体高度。
    /// 用来区分「窗口没起来」和「字体没绑上」两种完全不同的故障。
    RVDL_HOST_EXPORT int32_t rvdl_ui_probe(void);
}

// ── 键码 ────────────────────────────────────────────────────────────────
// 与 Nuklear 的 enum nk_keys 一致；rvdl 侧用 morph 常量镜像这份表。
// 可打印字符直接用 ASCII（'A'==65），特殊键用下面这些。
#define GH_NK_KEY_NONE   0
#define GH_NK_KEY_SHIFT  1
#define GH_NK_KEY_CTRL   2
#define GH_NK_KEY_DEL    3
#define GH_NK_KEY_ENTER  4
#define GH_NK_KEY_TAB    5
#define GH_NK_KEY_BACKSP 6
#define GH_NK_KEY_COPY   7
#define GH_NK_KEY_CUT    8
#define GH_NK_KEY_PASTE  9
#define GH_NK_KEY_UP     10
#define GH_NK_KEY_DOWN   11
#define GH_NK_KEY_LEFT   12
#define GH_NK_KEY_RIGHT  13