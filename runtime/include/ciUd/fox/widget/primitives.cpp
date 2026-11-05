// Fox widget/primitives.cpp - 原子控件实现

#include "primitives.hpp"
#include "core/anim.hpp"
#include "text/soft_wrap.hpp"
#include <algorithm>
#include <cstring>
#include <cstdio>
#include <cmath>
#include <unordered_map>
#include <vector>

namespace fox
{

    // 每（控件种类，屏上位置）一个保留状态 id。
    //
    // 这里每条规则都是踩过坑才写下来的：
    //
    //  · id 不能依赖控件的**值**。id 随值变就会选中另一个 WidgetState，
    //    而新槽位从 anim = 0 开始——toggle_switch() 每次点击都换一个新
    //    槽位，滑动永远完不成。看着像动画卡死，其实是 id 从动画底下挪走
    //    了。
    //
    //  · id 不能是裸常量。那意味着"全应用这类控件共用一个 WidgetState"。
    //    slider() 用过 make_id(salt, "slider")，同屏两个滑块共用一个
    //    悬停/拖动/动画槽位——拖一个另一个也动。
    //
    //  · 按 label 推导的 id 在两个控件同名时相撞，这很常见：设置页两个
    //    "Reset" 按钮，或一条工具栏。更糟的是，带状态的标题（"3 项"）每次
    //    数字变化都会丢掉按压动画。
    //
    //  · 按位置推导的 id 唯一、跨帧稳定、零成本。移动过的控件会拿到新 id
    //    ——这是代价，也正是 Context::prune_state_cache() 要回收的东西。
    //    需要跨移动保持身份的调用方传显式 hint，就像 text_field 那样。
    static inline Id widget_id(const Context &c, Id salt, const Rect &r)
    {
        return c.make_id(salt, (Id)id_hash_rect(r));
    }

    // ── 文本框辅助 ─────────────────────────────────────────────────────

    /// 把一个码点编码为 UTF-8。返回字节数，失败返回 0。
    ///
    /// 按键数组送来的是码点（glfwSetCharCallback）而不是字节，总得有人编码。
    /// 拒绝代理项和超过 U+10FFFF 的值是必须的：这里写入的是调用方的固定
    /// 大小 buffer——对一个实际要 6 字节的值按 4 字节编码，就是对别人数组
    /// 的一字节越界写。
    static inline int encode_utf8(u32 cp, char *out)
    {
        if (cp > 0x10FFFFu) return 0;
        if (cp >= 0xD800u && cp <= 0xDFFFu) return 0; // 孤立代理项

        int n = 0;
        if (cp < 0x80)
        {
            out[n++] = (char)cp;
        }
        else if (cp < 0x800)
        {
            out[n++] = (char)(0xC0 | (cp >> 6));
            out[n++] = (char)(0x80 | (cp & 0x3F));
        }
        else if (cp < 0x10000)
        {
            out[n++] = (char)(0xE0 | (cp >> 12));
            out[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            out[n++] = (char)(0x80 | (cp & 0x3F));
        }
        else
        {
            out[n++] = (char)(0xF0 | (cp >> 18));
            out[n++] = (char)(0x80 | ((cp >> 12) & 0x3F));
            out[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            out[n++] = (char)(0x80 | (cp & 0x3F));
        }
        return n;
    }

    /// 光标闪烁，任何编辑都会重新开始。
    ///
    /// `st.caret` 是距上次编辑的整秒相位。复用这个字段是刻意的：它按字段
    /// 存在、已按控件 id 索引，而打字时还在闪的光标是用户马上会注意到的
    /// 东西。再单开一个字段就多一处可能忘掉重置的地方。
    static inline void caret_restart(Context &c, WidgetState &st)
    {
        // 记录「上次编辑的时刻」（秒）。
        //
        // ⚠ 之前写的是 0，而 st.caret 全库只被赋过这一次 —— 于是相位恒为 0，
        // caret_visible 恒真 → **光标永不闪烁**（文档承诺 0.55s 亮 / 0.45s 灭）。
        st.caret = (i32)c.time();
        c.request_redraw();
    }

    static inline bool caret_visible(const Context &c, const WidgetState &st)
    {
        // 距上次编辑的秒数，取小数部分作为闪烁相位：0.55s 亮 / 0.45s 灭。
        //
        // ⚠ 旧公式 `(f32)st.caret - fmod(c.time(), 1.0f) < 0.55f` 不是相位差：
        //   st.caret 恒为 0 时它恒真（永亮）；即使 st.caret 是「上次编辑时刻」，
        //   「时刻 − 小数秒」也不是「距上次编辑多久」。
        const f32 since = c.time() - (f32)st.caret;
        if (since < 0.0f) return true;
        return fmod(since, 1.0f) < 0.55f;
    }

    /// 可打印 ASCII 转 Key。
    ///
    /// Key 只给没有字符的键命名（'a' 不是枚举项）；ASCII 占同一索引空间的
    /// 0..127 槽位，命名键在 0x100+。见 input.hpp 的 KEY_SLOTS——两个区段
    /// 共用一个索引空间，那个常量存在的全部理由就在于此。
    static inline Key ascii_key(char ch) { return (Key)(u16)(unsigned char)ch; }

    // â”€â”€ 标签 â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

    f32 label(Context &c, const Rect &r, const char *text, Color color, Align align)
    {
        if (!text) return 0.0f;
        Theme &th = c.theme();
        FontId f = c.font(th.font_body);
        f32 w = text_width(f, text);

        f32 tx = r.x;
        if (align == Align::Center) tx = r.x + (r.w - w) * 0.5f;
        else if (align == Align::End) tx = r.x + r.w - w;

        f32 lh = fonts().line_height(f);
        f32 ty = r.y + (r.h - lh) * 0.5f;

        u32 gs = 0;
        shape_text(c.dl(), f, text, {tx, ty + fonts().face(f)->ascent}, color, 0.0f, gs);
        return w;
    }

    f32 label_layout(Context &c, FontId font, const char *text, Color color, f32 height)
    {
        if (height <= 0.0f) height = fonts().line_height(font);
        Rect r = c.layout().allocate(Vec2{0.0f, height}, Align::Stretch);
        return label(c, r, text, color, Align::Start);
    }

    // ── 图片 ───────────────────────────────────────────────────────────

    bool image(Context &c, const Rect &r, TexId tex, Color tint)
    {
        // 记录前先校验。指向已销毁纹理的命令若传到后端，后端唯一安全的
        // 反应就是跳过绘制——故障会表现为控件神秘消失，而不是这里报参数错误。
        const Context::Texture *t = c.texture(tex);
        if (!t || r.w <= 0.0f || r.h <= 0.0f) return false;
        c.dl().push_image(r, tex, tint);
        return true;
    }

    bool image_region(Context &c, const Rect &r, TexId tex, const Rect &src, Color tint)
    {
        const Context::Texture *t = c.texture(tex);
        if (!t || r.w <= 0.0f || r.h <= 0.0f) return false;
        // 超出 0..1 的子矩形截断而不拒绝：CLAMP_TO_EDGE 的双线性采样本来
        // 就会截断，拒绝只会把布局计算的舍入误差变成图标消失。
        Rect s{src.x, src.y, src.w, src.h};
        s.x = s.x < 0.0f ? 0.0f : (s.x > 1.0f ? 1.0f : s.x);
        s.y = s.y < 0.0f ? 0.0f : (s.y > 1.0f ? 1.0f : s.y);
        if (s.w < 0.0f) s.w = 0.0f;
        if (s.h < 0.0f) s.h = 0.0f;
        if (s.x + s.w > 1.0f) s.w = 1.0f - s.x;
        if (s.y + s.h > 1.0f) s.h = 1.0f - s.y;
        if (s.w <= 0.0f || s.h <= 0.0f) return false;
        c.dl().push_image(r, tex, tint, s);
        return true;
    }

    // â”€â”€ 面板 / 玻璃 â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

    void panel(Context &c, const Rect &r, Color fill, float radius, bool glass)
    {
        CornerRadii rad(radius);
        if (glass)
            c.dl().push_glass(r, rad, theme().glass_tint, theme().glass_blur, theme().glass_opacity,
                              theme().glass_saturation, -1.0f);
        else
            c.dl().push_rounded(r, fill, rad, -1.0f);
    }

    void glass_bar(Context &c, const Rect &r, float radius)
    {
        Theme &th = theme();
        CornerRadii rad(radius);
        c.dl().push_glass(r, rad, th.glass_tint, th.glass_blur, th.glass_opacity,
                          th.glass_saturation, -2.0f);
    }

    void separator(Context &c, const Rect &r, bool horizontal)
    {
        Rect rr = horizontal ? Rect{r.x, r.y + r.h * 0.5f, r.w, theme().hairline}
                             : Rect{r.x + r.w * 0.5f, r.y, theme().hairline, r.h};
        c.dl().push(rr, theme().separator, -0.5f);
    }

    // â”€â”€ 按钮 â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

    void shadow_rect(Context &c, const Rect &r, const CornerRadii &radii, const Shadow &sh, f32 z)
    {
        if (sh.blur <= 0.0f || sh.color.a == 0) return;
        Cmd cmd;
        cmd.type = CmdType::Rect;
        cmd.rect = r;
        cmd.radii = radii;
        cmd.shadow = sh;
        // 完全透明的填充：这条命令只为它投下的阴影而存在。为什么这里漏掉
        // 颜色比别处漏掉糟糕得多，见声明处的说明。
        cmd.color = Color{0, 0, 0, 0};
        cmd.z = z;
        c.dl().push(cmd);
    }

    bool button(Context &c, const Rect &r, const char *text, ButtonStyle style, bool enabled)
    {
        Theme &th = c.theme();
        Id id = widget_id(c, 0xB0000001u, r);
        WidgetState &st = c.state_touch(id);

        bool over = enabled && c.hover(r, id);
        if (over) c.request_redraw();

        f32 target_hover = over ? 1.0f : 0.0f;
        f32 target_active = (over && c.input().buttons[(int)MouseButton::Left].down) ? 1.0f : 0.0f;
        st.hover = approach(st.hover, target_hover, c.anim_dt(), th.anim_fast);
        st.active = approach(st.active, target_active, c.anim_dt(), th.anim_fast * 0.6f);

        // 各样式的颜色
        Color base, hover_c, press_c, fg;
        switch (style)
        {
        case ButtonStyle::Filled:
            base = th.accent_color;
            hover_c = th.accent_hover;
            press_c = th.accent_active;
            fg = th.text_on_accent;
            break;
        case ButtonStyle::Tinted:
            base = Color::mix(th.accent_color, th.window_bg, 0.85f);
            hover_c = Color::mix(th.accent_color, th.window_bg, 0.7f);
            press_c = Color::mix(th.accent_color, th.window_bg, 0.55f);
            fg = th.accent_color;
            break;
        case ButtonStyle::Bezel:
            base = th.control_bg;
            hover_c = th.control_bg_hover;
            press_c = th.control_bg_active;
            fg = th.text;
            break;
        default: // Plain
            base = Color{0, 0, 0, 0};
            hover_c = th.control_bg_hover;
            press_c = th.control_bg_active;
            fg = th.accent_color;
            break;
        }
        if (!enabled)
        {
            base = th.control_bg_disabled;
            fg = th.text_tertiary;
            hover_c = base;
            press_c = base;
        }

        Color fill = st.active > 0.01f ? Color::mix(hover_c, press_c, st.active)
                                       : Color::mix(base, hover_c, st.hover);
        CornerRadii rad(th.corner_control);

        // 轻微投影带来 Golden Gate 的"抬升"观感。
        if (style == ButtonStyle::Filled || style == ButtonStyle::Bezel)
        {
            Shadow sh;
            sh.offset_y = 1.0f;
            sh.blur = 3.0f;
            sh.color = Color{0, 0, 0, (u8)(0.30f * (1.0f - st.active * 0.5f))};
            Cmd shadow_cmd;
            shadow_cmd.type = CmdType::Rect;
            shadow_cmd.rect = r.translated({0.0f, sh.offset_y});
            shadow_cmd.radii = rad;
            shadow_cmd.shadow = sh;
            shadow_cmd.color = sh.color;
            shadow_cmd.z = -1.1f;
            c.dl().push(shadow_cmd);
        }

        c.dl().push_rounded(r, fill, rad, -1.0f);

        if (style != ButtonStyle::Plain)
            c.dl().push_border(r, th.control_border, th.hairline, rad, -0.9f);

        label(c, r, text ? text : "", fg, Align::Center);

        // 点击
        bool clicked = false;
        if (enabled && over)
        {
            if (c.input().buttons[(int)MouseButton::Left].pressed)
            {
                st.pressed = true;
                clicked = false; // 等松开再算
            }
            if (st.pressed && c.input().buttons[(int)MouseButton::Left].released)
            {
                st.pressed = false;
                clicked = true;
            }
        }
        else
        {
            st.pressed = false;
        }
        return clicked;
    }

    // â”€â”€ 开关 â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

    bool toggle_switch(Context &c, const Rect &r, bool &value)
    {
        Theme &th = c.theme();
        Id id = widget_id(c, 0xB0000002u, r);
        WidgetState &st = c.state_touch(id);
        bool over = c.hover(r, id);

        f32 target = value ? 1.0f : 0.0f;
        st.anim = approach(st.anim, target, c.anim_dt(), th.anim_fast * 1.2f);
        if (fx_abs(st.anim - target) > 0.001f) c.request_redraw();

        // 轨道
        f32 track_w = r.w;
        f32 track_h = fx_min(r.h, track_w * 0.5f);
        Rect track{r.x, r.y + (r.h - track_h) * 0.5f, track_w, track_h};
        CornerRadii kr(track_h * 0.5f);

        Color off = th.control_bg;
        Color on = th.accent_color;
        Color fill = Color::mix(off, on, st.anim);
        c.dl().push_rounded(track, fill, kr, 0.0f);

        // 滑钮
        f32 knob_pad = 2.0f;
        f32 knob_d = track_h - knob_pad * 2.0f;
        f32 knob_x = track.x + knob_pad + st.anim * (track.w - knob_d - knob_pad * 2.0f);
        Rect knob{knob_x, track.y + knob_pad, knob_d, knob_d};

        Shadow sh;
        sh.offset_y = 1.0f;
        sh.blur = 2.0f;
        sh.color = Color{0, 0, 0, 0x40};
        shadow_rect(c, knob.translated({0, 1}), CornerRadii(knob_d * 0.5f), sh, 0.1f);

        c.dl().push_rounded(knob, Color{255, 255, 255, 255}, CornerRadii(knob_d * 0.5f), 0.2f);

        // 点击切换
        bool clicked = false;
        if (over && c.input().buttons[(int)MouseButton::Left].pressed)
        {
            value = !value;
            clicked = true;
        }
        return clicked;
    }

    bool checkbox(Context &c, const Rect &r, const char *label_text, bool &value)
    {
        Theme &th = c.theme();
        Id id = widget_id(c, 0xB0000003u, r);
        WidgetState &st = c.state_touch(id);

        f32 box = fx_min(r.h, 16.0f);
        Rect box_r{r.x, r.y + (r.h - box) * 0.5f, box, box};
        Rect full{r.x, r.y, r.w, r.h};
        bool over = c.hover(full, id);

        st.hover = approach(st.hover, over ? 1.0f : 0.0f, c.anim_dt(), th.anim_fast);

        // 方框
        CornerRadii br(4.0f);
        Color box_fill = value ? th.accent_color : th.control_bg;
        c.dl().push_rounded(box_r, box_fill, br, 0.0f);
        c.dl().push_border(box_r, value ? th.accent_active : th.control_border, th.hairline, br,
                           0.1f);

        // 勾号
        if (value)
        {
            // 勾号是一条折线：一撇一捺，圆头圆接——一条描边路径，
            // 不是一堆图元。
            //
            // 历史：以前是两个轴对齐矩形（4x2 和 2x4，在框中间糊成一团，
            // 看着像一个点或一次花屏），后来是沿两条臂摆约 16 个圆点。
            // 两者都是在绕同一个缺失的东西——描边折线。圆点看着对，但要
            // 16 个实例、接头也接不顺；`push_path` 两段就画完。
            const f32 bx = box_r.x, by = box_r.y;
            const f32 cx = bx + box * 0.5f, cy = by + box * 0.5f;
            const f32 t = std::fmax(1.6f, box * 0.13f);

            const Vec2 pts[3] = {
                {cx - box * 0.30f, cy + box * 0.02f},
                {cx - box * 0.08f, cy + box * 0.24f},
                {cx + box * 0.32f, cy - box * 0.26f},
            };
            c.dl().push_path(pts, 3, t, Color{255, 255, 255, 255}, 1, 1, 0, 0.2f);
        }

        // 标签
        if (label_text && r.w > box)
        {
            Rect lr{box_r.right() + th.spacing_sm, r.y, r.w - box - th.spacing_sm, r.h};
            label(c, lr, label_text, over ? th.text : th.text_secondary, Align::Start);
        }

        bool clicked = false;
        if (over && c.input().buttons[(int)MouseButton::Left].pressed)
        {
            value = !value;
            clicked = true;
        }
        return clicked;
    }

    // â”€â”€ 滑块 â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

    bool slider(Context &c, const Rect &r, float &value, float min, float max, SliderStyle style,
                float step)
    {
        Theme &th = c.theme();
        Id id = widget_id(c, 0xB0000004u, r);
        WidgetState &st = c.state_touch(id);

        bool over = c.hover(r, id);
        bool dragging = st.pressed;

        // 轨道
        f32 track_h = 4.0f;
        Rect track{r.x, r.y + (r.h - track_h) * 0.5f, r.w, track_h};
        CornerRadii tr(track_h * 0.5f);
        c.dl().push_rounded(track, th.control_bg, tr, 0.0f);

        if (max <= min) max = min + 1.0f;
        float t = (value - min) / (max - min);
        t = fx_clamp(t, 0.0f, 1.0f);

        // 已填充部分
        Rect fill{track.x, track.y, track.w * t, track_h};
        c.dl().push_rounded(fill, th.accent_color, tr, 0.05f);

        // 滑钮
        f32 knob_d = over || dragging ? 16.0f : 14.0f;
        f32 kx = track.x + t * (track.w - knob_d);
        Rect knob{kx, r.y + (r.h - knob_d) * 0.5f, knob_d, knob_d};
        Shadow sh;
        sh.offset_y = 1.0f;
        sh.blur = 3.0f;
        sh.color = Color{0, 0, 0, 0x50};
        shadow_rect(c, knob.translated({0, 1}), CornerRadii(knob_d * 0.5f), sh, 0.1f);
        c.dl().push_rounded(knob, Color{255, 255, 255, 255}, CornerRadii(knob_d * 0.5f), 0.2f);
        c.dl().push_border(knob, th.control_border, th.hairline, CornerRadii(knob_d * 0.5f), 0.21f);

        // 交互
        if (over && c.input().buttons[(int)MouseButton::Left].pressed)
        {
            st.pressed = true;
            dragging = true;
        }
        if (st.pressed && !c.input().buttons[(int)MouseButton::Left].down)
        {
            st.pressed = false;
            dragging = false;
        }

        if (dragging && over)
        {
            float nt = fx_clamp((c.input().mouse_pos.x - track.x) / fx_max(track.w, 1.0f), 0.0f,
                                1.0f);
            float nv = min + nt * (max - min);
            if (style == SliderStyle::Stepped && step > 0.0f)
                nv = std::round(nv / step) * step;
            value = fx_clamp(nv, min, max);
        }
        return dragging;
    }

    // â”€â”€ 文本框 â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

    bool text_field(Context &c, const Rect &r, char *buffer, int capacity,
                    const char *placeholder, Id id_hint)
    {
        Theme &th = c.theme();
        // ⚠ id 必须按字段区分，不能全应用一个常量。
        //
        //   以前是硬编码的 make_id(0xB0000005, "field")，于是应用里每个
        //   文本框共用一个 WidgetState：聚焦第二个框，焦点环画在第一个上，
        //   两个光标位置还是同一个数。没有哪里画错，只是表现得像永远只有
        //   一个可编辑字段。
        //
        //   没有 label 可作 key，就用矩形区分——对齐到整像素，亚像素布局
        //   抖动就不会让字段"变成另一个控件"而丢状态。需要跨移动保持身份
        //   的调用方改传显式 id。
        Id id = id_hint ? id_hint : widget_id(c, 0xB0000005u, r);
        WidgetState &st = c.state_touch(id);

        bool over = c.hover(r, id);
        bool focused = c.focus().has_focus(id);
        if (focused) st.focused = true;

        CornerRadii rad(th.corner_field);
        Color fill = focused ? th.control_bg_active : th.control_bg;
        c.dl().push_rounded(r, fill, rad, 0.0f);
        // 焦点环：强调色轮廓，聚焦时变粗。
        f32 ring_w = focused ? 2.0f : th.hairline;
        Color ring = focused ? th.accent_color : (over ? th.control_border : th.separator);
        c.dl().push_border(r, ring, ring_w, rad, 0.1f);

        FontId f = c.font(th.font_body);
        f32 lh = fonts().line_height(f);
        f32 ascent = fonts().face(f)->ascent;
        Vec2 pen{r.x + th.spacing_sm, r.y + (r.h - lh) * 0.5f + ascent};

        if (buffer && capacity > 0)
        {
            u32 gs = 0;
            // ── 编辑模型 ──────────────────────────────────────
            //
            // 以前用 memmove 直接改 `buffer`、手写 UTF-8 编码器，光标放在
            // WidgetState 里。在你想要选区或撤销栈之前这都没问题——而到那时
            // 你离需要这个对象只差三个功能——所以直接用这个对象。
            TextBuffer &tb = c.text_edit(id);

            // 宿主优先。直接写 buffer 的宿主（载入存档值、重置表单）不该被
            // 过期的光标状态顶回来；set_text 同时丢弃历史，因为新值不是用户
            // 敲出来的。
            c.sync_text_from_host(tb, buffer, strlen(buffer));

            const f32 avail = r.w - th.spacing_sm * 2.0f;
            const std::string &s = tb.text();

            // IME 占用字段时光标该去的位置。由下面的预编辑绘制设置，每帧
            // 重置——合成打开那一帧留下的过期值，会在合成结束后把光标停到
            // 莫名其妙的地方。
            f32 preedit_x_ = 0.0f;
            bool preedit_drawn_ = false;

            // ── 交互 ─────────────────────────────────────────
            // 在绘制之前处理，本帧的点击才能反映在本帧画面上。
            bool blink_reset = false;

            if (over && c.input().buttons[(int)MouseButton::Left].pressed)
            {
                c.focus().set_focus(id);
                focused = true;
                const size_t hit = text_hit_index(f, s.c_str(), c.input().mouse_pos.x - pen.x);
                if (c.input().buttons[(int)MouseButton::Left].double_click)
                {
                    // 双击选中光标下的词。
                    auto cls = [](char ch) {
                        return (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r') ? 0
                             : ((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'z') ||
                                (ch >= 'A' && ch <= 'Z'))
                                   ? 1
                                   : 2;
                    };
                    size_t a = hit, b = hit;
                    while (a > 0 && cls(s[a - 1]) == cls(s[hit])) --a;
                    while (b < s.size() && cls(s[b]) == cls(s[hit])) ++b;
                    tb.set_caret(a);
                    tb.set_anchor(b);
                }
                else
                {
                    tb.set_caret(hit);
                }
                blink_reset = true;
            }

            // 拖动从按下处开始扩展选区。
            if (over && c.input().buttons[(int)MouseButton::Left].down && focused)
                tb.select_to(text_hit_index(f, s.c_str(), c.input().mouse_pos.x - pen.x));

            if (focused)
            {
                const bool shift = c.input().shift;
                const bool ctrl = c.input().ctrl;
                bool nav = false;

                if (c.input().key_press(Key::Left))
                {
                    if (ctrl)
                        tb.move_word(-1, shift);
                    else
                        tb.move_codepoint(-1, shift);
                    nav = true;
                }
                else if (c.input().key_press(Key::Right))
                {
                    if (ctrl)
                        tb.move_word(1, shift);
                    else
                        tb.move_codepoint(1, shift);
                    nav = true;
                }
                else if (c.input().key_press(Key::Home))
                {
                    tb.move_home(shift);
                    nav = true;
                }
                else if (c.input().key_press(Key::End))
                {
                    tb.move_end(shift);
                    nav = true;
                }
                else if (c.input().key_press(Key::Backspace))
                {
                    tb.backspace();
                    blink_reset = true;
                }
                else if (c.input().key_press(Key::Delete))
                {
                    tb.del();
                    blink_reset = true;
                }

                // Ctrl+A / Ctrl+Z / Ctrl+Y（及 Ctrl+Shift+Z），加上剪贴板
                // 三件套。
                //
                // 即使平台没有剪贴板，复制和剪切也照常执行：平台为空只意味着
                // 选区没处可去，这是正确的降级行为。空平台上粘贴什么都不做，
                // 也正确。这些都不许成为文本框在 QuarkOS 上行为不对的理由。
                Platform *plat = platform();

                if (ctrl && c.input().key_press(ascii_key('a')))
                {
                    tb.select_all();
                    nav = true;
                }
                else if (ctrl && c.input().key_press(ascii_key('c')))
                {
                    if (plat && tb.has_selection()) plat->set_clipboard_text(tb.selected());
                }
                else if (ctrl && c.input().key_press(ascii_key('x')))
                {
                    if (plat && tb.has_selection())
                    {
                        plat->set_clipboard_text(tb.selected());
                        // 剪贴板写入成功之后再删。反过来的顺序会在剪贴板
                        // 不可用时丢文本，而那恰是用户最想不到会丢的时候。
                        tb.delete_selection();
                        blink_reset = true;
                    }
                }
                else if (ctrl && c.input().key_press(ascii_key('v')))
                {
                    if (plat)
                    {
                        const std::string clip = plat->clipboard_text();
                        if (!clip.empty())
                        {
                            // 插入前先尊重调用方的固定容量，粘贴内容截断到
                            // 装得下，而不是写回时溢出宿主的数组。
                            const size_t room = (size_t)capacity - 1;
                            std::string_view cv(clip);
                            if (cv.size() > room) cv = cv.substr(0, room);
                            tb.insert(cv);
                            blink_reset = true;
                        }
                    }
                }
                else if (ctrl && c.input().key_press(ascii_key('z')))
                {
                    if (shift)
                        tb.redo();
                    else
                        tb.undo();
                    blink_reset = true;
                }
                else if (ctrl && c.input().key_press(ascii_key('y')))
                {
                    tb.redo();
                    blink_reset = true;
                }

                if (nav) blink_reset = true;

                // 键入的字符。char_callback 已经逐码点送来解码后的 UTF-8，
                // 这里逐个编码、各插入一步——正是这让缓冲能把连续打字合并成
                // 一条撤销记录。
                //
                // IME 合成期间完全跳过。那些按键属于 IME，这里再送一遍就是
                // 拼音合成会把多余的 'n' 烙进文档的原因：用户敲了 "nihao"，
                // 提交了 "你好"，后面还插进来一个 "nihao"。
                if (!tb.has_composition())
                {
                    char utf8[5];
                    for (u32 ch : c.input().chars)
                    {
                        if (ch == '\r' || ch == '\n' || ch < 32) continue;
                        int n = encode_utf8(ch, utf8);
                        if (n <= 0) continue;
                        if (s.size() + (size_t)n + 1 > (size_t)capacity) break;
                        tb.insert(std::string_view(utf8, (size_t)n));
                        blink_reset = true;
                    }
                }

                if (blink_reset) caret_restart(c, st);
            }

            // ── 绘制 ────────────────────────────────────────────────
            //
            // 选区画在字形**下面**。必须在 shape_text() 之前压入，因为
            // shape_text 要发完字形才发绘制命令——等到该画选区时，
            // 还没有东西可读回来。
            if (tb.has_selection())
            {
                Rect spans[256];
                const u32 ns = text_span_rects(f, s.c_str(), tb.sel_min(), tb.sel_max(), pen.x, lh,
                                               spans, 256);
                for (u32 i = 0; i < ns; ++i)
                {
                    // 每个 span 向外撑一点，让相邻的接上。不这样的话，舍入
                    // 会留下亚像素缝隙，在高亮里看着像一道道竖缝。
                    spans[i].x -= 0.5f;
                    spans[i].w += 1.0f;
                    c.dl().push(spans[i], th.selection, -0.1f);
                }
            }

            shape_text(c.dl(), f, s.c_str(), pen, th.text, avail, gs);

            // ── IME 预编辑 ──────────────────────────────────────────
            //
            // 作为独立的一段绘制，紧跟在光标前的文本后面，而不是拼接进
            // 重建的字符串。
            //
            // 拼接看着简单，其实是个坑：它把光标之后所有字节偏移都挪了位，
            // 选区带、光标位置、命中测试都得按预编辑长度重新基址。三处可能
            // 写错，症状全是"选区差了一点"。分三次绘制根本没有偏移要重算
            // ——只是顺次推进笔位。
            if (tb.has_composition())
            {
                const f32 x0 = pen.x + text_offset_x(f, s.c_str(), tb.caret());
                const std::string &pre = tb.preedit();
                Vec2 pre_pen{x0, pen.y};

                shape_text(c.dl(), f, pre.c_str(), pre_pen, th.text, 0.0f, gs);

                const f32 pw = text_width(f, pre.c_str());
                // 下划线。这是区分合成中文本和已提交文本的唯一标记，不是
                // 装饰——没有它，用户看不出 IME 还在斟酌什么。
                if (pw > 0.0f)
                {
                    const f32 uy = pen.y + 2.0f;
                    c.dl().push(Rect{x0, uy, pw, 1.0f}, th.accent_color, 0.25f);
                }
                // IME 持有输入时，光标应位于预编辑文本内部，否则用户打字时
                // 光标看起来不动。
                preedit_x_ = x0 + text_offset_x(f, pre.c_str(), tb.preedit_caret());
                preedit_drawn_ = true;
            }

            // 光标：按字节偏移定位，落在字符**之间**，而不是随着前面所有
            // 内容的宽度漂移。合成中则移到预编辑自己的光标处。
            if (focused && caret_visible(c, st))
            {
                const f32 cx = preedit_drawn_ ? preedit_x_
                                              : pen.x + text_offset_x(f, s.c_str(), tb.caret());
                c.dl().push(Rect{cx, r.y + (r.h - lh) * 0.5f, 1.5f, lh}, th.accent_color, 0.2f);
            }

            // ── 写回 ─────────────────────────────────────────
            //
            // 截断而不是溢出：调用方的 buffer 是固定 C 数组，它不知道自己
            // 有多长。越界写过去永远是错的。
            const size_t room = (size_t)capacity - 1;
            const size_t n = s.size() < room ? s.size() : room;
            memcpy(buffer, s.data(), n);
            buffer[n] = 0;
        }
        else if (placeholder)
        {
            u32 gs = 0;
            shape_text(c.dl(), f, placeholder, pen, th.text_tertiary, r.w - th.spacing_sm * 2.0f, gs);
        }

        return focused;
    }

    // -- 多行文本区 -------------------------------------------

    // ── 软换行缓存（视觉行）────────────────────────────────────────────
    //
    // 换行算法本身在 text/soft_wrap.hpp（那里有按真实字体度量做的单元测试）。
    // 这里放的是缓存：行列表是 O(文本量) 的字形度量，2000 行的文档不缓存就
    // 得每帧重新度量每个字形。缓存键是（控件 id、文本指纹、宽度、字体、
    // wrap 标志）——指纹是对文本的 FNV-1a，约 1 纳秒/字节。

    struct WrapCache
    {
        bool valid = false;
        Id id = 0;
        FontId font = 0;
        f32 width = 0.0f;
        u64 hash = 0;
        size_t text_size = 0;
        bool wrapped = true;             ///< 这份映射是开着换行构建的吗？
        std::vector<WrapSpan> rows;      ///< 全部视觉行，按顺序
        std::vector<u32> row_line;       ///< 视觉行 -> 逻辑行
        std::vector<u32> line_first_row; ///< 逻辑行 -> 首个视觉行下标
        size_t total_rows = 0;
    };

    /// 每个控件 id 一条，有上限。上限保证映射不会为每个创建过的字段都长
    /// 一条；消失的文本区会在别的 id 把映射顶过上限时丢掉自己的条目。
    static WrapCache &wrap_cache(Id id)
    {
        static std::unordered_map<Id, WrapCache> cache;
        static const size_t kMaxEntries = 16;
        if (cache.size() > kMaxEntries) cache.clear();
        return cache[id];
    }

    bool text_area(Context &c, const Rect &r, char *buffer, int capacity, const char *placeholder,
                   Id id_hint, bool wrap)
    {
        Theme &th = c.theme();
        // 与 text_field 同一套 id 方案和 salt 族，两个控件相关但绝不相撞。
        Id id = id_hint ? id_hint : widget_id(c, 0xB0000006u, r);
        WidgetState &st = c.state_touch(id);

        bool over = c.hover(r, id);
        bool focused = c.focus().has_focus(id);
        if (focused) st.focused = true;

        CornerRadii rad(th.corner_field);
        c.dl().push_rounded(r, focused ? th.control_bg_active : th.control_bg, rad, 0.0f);
        const f32 ring_w = focused ? 2.0f : th.hairline;
        c.dl().push_border(r, focused ? th.accent_color : (over ? th.control_border : th.separator),
                           ring_w, rad, 0.1f);

        if (!buffer || capacity <= 0)
        {
            // 与 text_field 同一规则：没有可编辑 buffer 就没什么可画，只有
            // 提示文字，调用方看到的就是提示。（以前这里完全忽略该参数，
            // 带 placeholder 的文本区于是毫无反应。）
            const FontId f = c.font(th.font_body);
            if (placeholder && fonts().face(f))
            {
                const f32 lh = fonts().line_height(f);
                const f32 ascent = fonts().face(f)->ascent;
                (void)lh;
                u32 gs = 0;
                shape_text(c.dl(), f, placeholder,
                           {r.x + th.spacing_sm, r.y + th.spacing_md + ascent}, th.text_tertiary,
                           r.w - th.spacing_sm * 2.0f, gs);
            }
            return focused;
        }

        TextBuffer &tb = c.text_edit(id);
        c.sync_text_from_host(tb, buffer, (size_t)strlen(buffer));

        const FontId f = c.font(th.font_body);
        const f32 lh = fonts().line_height(f);
        if (!fonts().face(f)) return focused;
        const f32 ascent = fonts().face(f)->ascent;

        const f32 pad_x = th.spacing_sm;
        const f32 pad_y = th.spacing_md;
        const f32 view_w_full = r.w - pad_x * 2.0f;
        const f32 view_h = r.h - pad_y * 2.0f;
        // 小到一行都放不下。直接返回，免得后面拿零或负的行高做除法。
        if (view_w_full <= 0.0f || lh <= 0.0f || view_h < lh) return focused;

        const int visible_rows_est = (int)(view_h / lh);
        if (visible_rows_est < 1) return focused;

        const std::string &s = tb.text();
        const size_t nlines = tb.line_count();

        // 只有文档溢出时才预留滚动条宽度。它随内容出现/消失，这确实是一次
        // 布局位移——但总是预留的话，每个短框都白白少 10 像素，而位移只
        // 发生一次，在文本首次溢出的那一帧。
        const f32 sb_w = 10.0f;

        // ── wrap map：逻辑行 → 视觉行 ────────────────────────────────────
        //
        // 两遍：先用全宽（不含滚动条）换行得到总行数，决定滚动条是否出现；
        // 滚动条抽走 10px 后按更窄的宽度重 wrap。缓存键含宽度，两个变体
        // 共存，重 wrap 只发生在滚动条出现/消失的那一帧。
        //
        // `wrap=false` 时每个逻辑行恰是一个视觉行 —— 与软换行前的行为
        // 逐字节一致（同一些 span、同一批 glyph 调用）。
        auto build_wrap = [&](f32 width) -> WrapCache & {
            WrapCache &wc = wrap_cache(id);
            const u64 h = text_fingerprint(s);
            if (wc.valid && wc.id == id && wc.font == f && wc.width == width &&
                wc.hash == h && wc.text_size == s.size() && wc.wrapped == wrap)
            {
                return wc;
            }
            wc.rows.clear();
            wc.row_line.clear();
            wc.line_first_row.assign(nlines + 1, 0);
            wc.rows.reserve(nlines);
            wc.row_line.reserve(nlines);
            if (wrap)
            {
                size_t longest = 1;
                for (size_t ln = 0; ln < nlines; ++ln)
                    longest = std::max(longest, tb.line_end(ln) - tb.line_start(ln));
                std::vector<WrapSpan> scratch(longest);
                for (size_t ln = 0; ln < nlines; ++ln)
                {
                    wc.line_first_row[ln] = (u32)wc.rows.size();
                    const size_t ls = tb.line_start(ln);
                    const size_t le = tb.line_end(ln);
                    const int nr = wrap_line(f, s, ls, le, width, scratch.data(),
                                             (int)scratch.size());
                    if (nr == 0)
                    {
                        // 空的逻辑行仍占一行。
                        //
                        // 光标必须能停在空行上；而且 fox_text_lines 的行数算法是
                        // 「空行算 1 行」—— 两边不一致，宿主的行号算术就会和 Fox
                        // 的对不上（fox.h 承诺「与 text_area 用同一套算法」）。
                        // 之前这里 nr==0 时**一行都不 push**，于是
                        //   text_area(wrap=true)  → "a\n\nb" = 2 行
                        //   text_area(wrap=false) → 3 行
                        //   fox_text_lines         → 3 行
                        wc.rows.push_back(WrapSpan{ls, le});
                        wc.row_line.push_back((u32)ln);
                    }
                    else
                    {
                        for (int k = 0; k < nr; ++k)
                        {
                            wc.rows.push_back(scratch[(size_t)k]);
                            wc.row_line.push_back((u32)ln);
                        }
                    }
                }
            }
            else
            {
                for (size_t ln = 0; ln < nlines; ++ln)
                {
                    wc.line_first_row[ln] = (u32)wc.rows.size();
                    wc.rows.push_back(WrapSpan{tb.line_start(ln), tb.line_end(ln)});
                    wc.row_line.push_back((u32)ln);
                }
            }
            wc.line_first_row[nlines] = (u32)wc.rows.size();
            wc.total_rows = wc.rows.size();
            wc.valid = true;
            wc.id = id;
            wc.font = f;
            wc.width = width;
            wc.hash = h;
            wc.text_size = s.size();
            wc.wrapped = wrap;
            return wc;
        };

        WrapCache &wc_full = build_wrap(view_w_full);
        const bool v_scroll = wc_full.total_rows > (size_t)visible_rows_est;
        const f32 view_w = view_w_full - (v_scroll ? sb_w : 0.0f);
        if (view_w <= 0.0f) return focused;
        WrapCache &wc = v_scroll ? build_wrap(view_w) : wc_full;
        const std::vector<WrapSpan> &rows = wc.rows;
        const std::vector<u32> &row_line = wc.row_line;
        const size_t total_rows = wc.total_rows;
        if (total_rows == 0) return focused;

        const f32 content_h = (f32)total_rows * lh;

        // byte → 视觉行（rows 按 begin 有序，二分）。
        auto row_of_byte = [&](size_t b) -> size_t {
            size_t lo = 0, hi = rows.size();
            while (lo < hi)
            {
                const size_t mid = (lo + hi) / 2;
                if (rows[mid].begin <= b) lo = mid + 1;
                else hi = mid;
            }
            return lo ? lo - 1 : 0;
        };

        // ---- scroll state（st.scroll 为**逻辑行**号）────────────────────
        // 首个视觉行 = 所滚动逻辑行的第一个视觉行。软换行后一个逻辑行可占
        // 多个视觉行，故「一屏能画多少行」由视觉行数决定，而不是固定的
        // visible_lines 条逻辑行。交互（点击命中）用此刻的值；滚轮与
        // 「滚动到光标」会改 st.scroll，渲染前重算。
        if (st.scroll < 0.0f) st.scroll = 0.0f;
        if (st.scroll > (f32)(nlines - 1)) st.scroll = (f32)(nlines - 1);
        size_t first_visual_row = wc.line_first_row[(size_t)st.scroll];

        // ---- 交互 ----
        bool blink_reset = false;

        // 点击 -> 字节偏移。两个方向都截断：点在最后一行之下落在该行末尾
        // 而不是虚无处，点在第一行之上落在第一行。软换行下这里的"行"是
        // 视觉行，点长行的第三个折行段就落在看起来该落的位置。
        auto hit_at = [&](f32 mx, f32 my) -> size_t {
            long ridx = first_visual_row + (long)floorf((my - (r.y + pad_y)) / lh);
            if (ridx < 0) ridx = 0;
            if ((size_t)ridx >= total_rows) ridx = (long)total_rows - 1;
            const WrapSpan &row = rows[(size_t)ridx];
            const size_t col = text_hit_index_n(f, s.data() + row.begin, row.end - row.begin,
                                                mx - (r.x + pad_x) + st.scroll_x);
            return row.begin + col;
        };

        if (over && c.input().buttons[(int)MouseButton::Left].pressed)
        {
            c.focus().set_focus(id);
            focused = true;
            const size_t hit = hit_at(c.input().mouse_pos.x, c.input().mouse_pos.y);
            if (c.input().buttons[(int)MouseButton::Left].double_click)
            {
                const size_t ln = tb.line_of(hit);
                const size_t ls = tb.line_start(ln), le = tb.line_end(ln);
                auto is_word = [](char ch) { return ch != ' ' && ch != '\t' && ch != '\r'; };
                size_t a = hit, b = hit;
                while (a > ls && is_word(s[a - 1])) --a;
                while (b < le && is_word(s[b])) ++b;
                tb.set_caret(a);
                tb.set_anchor(b);
            }
            else
            {
                tb.set_caret(hit);
            }
            blink_reset = true;
        }
        if (over && c.input().buttons[(int)MouseButton::Left].down && focused)
            tb.select_to(hit_at(c.input().mouse_pos.x, c.input().mouse_pos.y));

        // 滚轮：一格滚三个视觉行，感觉是"挪一点"而不是"翻一屏"。按视觉行
        // 而不是逻辑行走——软换行后一个逻辑行可能占五六个视觉行，按逻辑行
        // 走 3 行会一整屏一整屏地跳。
        if (over && c.input().scroll_y != 0.0f && v_scroll)
        {
            long target = (long)first_visual_row - (long)(c.input().scroll_y * 3.0f);
            if (target < 0) target = 0;
            if ((size_t)target >= total_rows) target = (long)total_rows - 1;
            st.scroll = (f32)row_line[(size_t)target];
        }

        if (focused)
        {
            const bool shift = c.input().shift;
            const bool ctrl = c.input().ctrl;
            Platform *plat = platform();
            bool nav = false;

            if (c.input().key_press(Key::Up) || c.input().key_press(Key::Down) ||
                c.input().key_press(Key::PageUp) || c.input().key_press(Key::PageDown))
            {
                // 垂直移动按**视觉行**：软换行后「下一行」是下一个视觉行，
                // 可能仍在同一个逻辑行内。粘性列用像素宽（而不是 TextBuffer
                // 的字节列）—— 字节列在 CJK 上不是列。
                const int dir = (c.input().key_press(Key::Up) || c.input().key_press(Key::PageUp))
                                    ? -1
                                    : 1;
                const int step =
                    (c.input().key_press(Key::PageUp) || c.input().key_press(Key::PageDown))
                        ? visible_rows_est
                        : 1;
                const size_t crow = row_of_byte(tb.caret());
                const WrapSpan &cr = rows[crow];
                const f32 cx = text_width_n(f, s.data() + cr.begin, tb.caret() - cr.begin);
                long target = (long)crow + dir * step;
                if (target < 0) target = 0;
                if ((size_t)target >= total_rows) target = (long)total_rows - 1;
                const WrapSpan &tr = rows[(size_t)target];
                const size_t col = text_hit_index_n(f, s.data() + tr.begin, tr.end - tr.begin, cx);
                const size_t want = tr.begin + col;
                if (shift)
                    tb.select_to(want);
                else
                    tb.set_caret(want);
                nav = true;
            }
            else if (c.input().key_press(Key::Left))
            {
                ctrl ? tb.move_word(-1, shift) : tb.move_codepoint(-1, shift);
                nav = true;
            }
            else if (c.input().key_press(Key::Right))
            {
                ctrl ? tb.move_word(1, shift) : tb.move_codepoint(1, shift);
                nav = true;
            }
            else if (c.input().key_press(Key::Home) || c.input().key_press(Key::End))
            {
                // Ctrl+Home/End 到文档开头/结尾。没有它，不用鼠标就离不开
                // 长文档。
                //
                // 裸 Home/End 去**视觉行**的首/尾（软换行后的「一行」就是
                // 视觉行 —— 光标在第三个换行段里时，End 应到该段尾，而不是
                // 整个逻辑行的尾，后者在屏幕外）。
                if (ctrl)
                {
                    const size_t was = tb.caret();
                    tb.set_caret(c.input().key_press(Key::Home) ? (size_t)0 : s.size());
                    if (shift) tb.select_to(was);
                }
                else
                {
                    const WrapSpan &cr = rows[row_of_byte(tb.caret())];
                    const size_t want = c.input().key_press(Key::Home) ? cr.begin : cr.end;
                    if (shift)
                        tb.select_to(want);
                    else
                        tb.set_caret(want);
                }
                nav = true;
            }
            else if (c.input().key_press(Key::Backspace))
            {
                tb.backspace();
                blink_reset = true;
            }
            else if (c.input().key_press(Key::Delete))
            {
                tb.del();
                blink_reset = true;
            }
            else if (c.input().key_press(Key::Enter))
            {
                // 这里 Enter **插入换行**。text_field 丢弃它；在文本区丢弃
                // 它，控件就没法用了。
                //
                // 插入前先检查容量，因为写回时会截断——buffer 在文档中途
                // 写满的话，尾部会直接丢掉，没有任何提示。
                if (s.size() + 2 <= (size_t)capacity - 1)
                {
                    tb.insert("\n");
                    blink_reset = true;
                }
            }

            if (ctrl && c.input().key_press(ascii_key('a')))
            {
                tb.select_all();
                nav = true;
            }
            else if (ctrl && c.input().key_press(ascii_key('c')))
            {
                if (plat && tb.has_selection()) plat->set_clipboard_text(tb.selected());
            }
            else if (ctrl && c.input().key_press(ascii_key('x')))
            {
                if (plat && tb.has_selection())
                {
                    // 先写剪贴板再删除：反过来的顺序恰在剪贴板不可用时丢
                    // 文本。
                    plat->set_clipboard_text(tb.selected());
                    tb.delete_selection();
                    blink_reset = true;
                }
            }
            else if (ctrl && c.input().key_press(ascii_key('v')))
            {
                if (plat)
                {
                    const std::string clip = plat->clipboard_text();
                    if (!clip.empty())
                    {
                        const size_t room = (size_t)capacity - 1;
                        std::string_view cv(clip);
                        if (cv.size() > room) cv = cv.substr(0, room);
                        tb.insert(cv);
                        blink_reset = true;
                    }
                }
            }
            else if (ctrl && c.input().key_press(ascii_key('z')))
            {
                shift ? tb.redo() : tb.undo();
                blink_reset = true;
            }
            else if (ctrl && c.input().key_press(ascii_key('y')))
            {
                tb.redo();
                blink_reset = true;
            }

            if (nav) blink_reset = true;

            // 键入的字符，IME 合成期间抑制：那些按键属于 IME，两边都处理
            // 就是拼音合成会把多余的 'n' 烙进文档的原因。
            if (!tb.has_composition())
            {
                char utf8[5];
                for (u32 ch : c.input().chars)
                {
                    // '\n' 永远不会到这里：Enter 是键不是字符，在上面处理，
                    // 这样才能做容量检查。
                    if (ch == '\r' || ch == '\n' || ch < 32) continue;
                    const int nb = encode_utf8(ch, utf8);
                    if (nb <= 0) continue;
                    if (s.size() + (size_t)nb + 1 > (size_t)capacity - 1) break;
                    tb.insert(std::string_view(utf8, (size_t)nb));
                    blink_reset = true;
                }
            }

            if (blink_reset) caret_restart(c, st);
        }

        // ---- 把光标滚进视区 ----
        //
        // 在输入之后、同一帧内。延后一帧会让每次按键时光标明显在屏幕外
        // 停一帧，那正是用户说的"卡"。
        //
        // 按视觉行判定：光标所在的视觉行必须落在 [first_visual_row,
        // first_visual_row + 一屏行数) 内。st.scroll 是逻辑行号，故向上滚
        // 直接取光标所在逻辑行，向下滚则逐逻辑行前进直到光标的视觉行进入
        // 视区（一个逻辑行可能占多个视觉行，一步跨不死）。
        {
            const size_t crow = row_of_byte(tb.caret());
            const size_t caret_line = row_line[crow];
            if (crow < first_visual_row)
            {
                st.scroll = (f32)caret_line;
            }
            else if (crow >= first_visual_row + (size_t)visible_rows_est)
            {
                // 向下：前进到「光标视觉行落入前 visible_rows_est 个视觉行」
                // 的最小逻辑行。line_first_row 单调不减，线性前进即可
                // （通常几步就走完）。
                size_t ln = (size_t)st.scroll;
                while (ln + 1 < nlines &&
                       wc.line_first_row[ln] + (size_t)visible_rows_est <= crow)
                    ++ln;
                st.scroll = (f32)ln;
            }
            if (st.scroll < 0.0f) st.scroll = 0.0f;
            if (st.scroll > (f32)(nlines - 1)) st.scroll = (f32)(nlines - 1);
            first_visual_row = wc.line_first_row[(size_t)st.scroll];

            // 水平滚动只在「单个不可断片段比盒子还宽」时才需要（长 URL、
            // 长 token）。其余情况软换行已经不需要横向滚。
            const WrapSpan &cr = rows[row_of_byte(tb.caret())];
            const f32 cx = text_width_n(f, s.data() + cr.begin, tb.caret() - cr.begin);
            f32 sx = st.scroll_x;
            if (sx > cx) sx = cx;
            if (sx + view_w < cx) sx = cx - view_w + 1.0f;
            if (sx < 0.0f) sx = 0.0f;
            st.scroll_x = sx;
        }

        // 视觉行的 Y，相对第一个可见行。
        //
        // ⚠ 不是绝对行号。用 `ri` 而不是 `ri - first_visual_row` 相乘会把
        //   每行都下推 first_visual_row * lh，文本从框的底部滚出去，而滚动
        //   偏移本身看着完全正确。只有文档高到能滚动时才出现——所以没滚动
        //   的文本区看着没问题，把它藏住了。
        auto row_y = [&](size_t ri) -> f32 {
            return r.y + pad_y + (f32)(ri - first_visual_row) * lh;
        };
        const Vec2 origin{r.x + pad_x - st.scroll_x, r.y + pad_y + ascent};

        // 可见视觉行范围：[first_visual_row, last_visible_row]
        const size_t last_visible_row = first_visual_row + (size_t)visible_rows_est;

        // ---- 选区带，逐视觉行 ----
        //
        // 画在字形下面。只考虑可见行，万行文档才不会发出一万个矩形。
        if (tb.has_selection())
        {
            const size_t lo = tb.sel_min(), hi = tb.sel_max();
            for (size_t ri = first_visual_row; ri < last_visible_row && ri < total_rows; ++ri)
            {
                const WrapSpan &row = rows[ri];
                const size_t ca = lo > row.begin ? lo : row.begin;
                const size_t cb = hi < row.end ? hi : row.end;
                if (cb <= ca) continue;
                const f32 x0 = text_width_n(f, s.data() + row.begin, ca - row.begin);
                const f32 x1 = text_width_n(f, s.data() + row.begin, cb - row.begin);
                if (x1 - x0 <= 0.0f) continue;
                c.dl().push(Rect{origin.x + x0, row_y(ri), x1 - x0, lh},
                            th.selection, -0.1f);
            }
        }

        // ---- 字形 ----
        //
        // 只对可见行做整形。文本区能装大文档全靠这个：整串整形会不管屏幕
        // 上有什么、每字符都发一个字形 quad。软换行下每行最宽 view_w，
        // max_width 裁剪几乎不会触发。
        for (size_t ri = first_visual_row; ri < last_visible_row && ri < total_rows; ++ri)
        {
            const WrapSpan &row = rows[ri];
            if (row.end <= row.begin) continue;
            const Vec2 row_pen{origin.x, row_y(ri) + ascent};
            u32 gs = 0;
            shape_text_n(c.dl(), f, s.data() + row.begin, row.end - row.begin, row_pen, th.text,
                         view_w, gs);
        }

        // ---- 预编辑，画在光标所在视觉行 ----
        f32 preedit_x_ = 0.0f;
        bool preedit_drawn_ = false;
        if (tb.has_composition())
        {
            const size_t crow = row_of_byte(tb.caret());
            const WrapSpan &cr = rows[crow];
            const f32 x0 = origin.x + text_width_n(f, s.data() + cr.begin, tb.caret() - cr.begin);
            const std::string &pre = tb.preedit();
            const Vec2 pp{x0, row_y(crow) + ascent};
            u32 gs = 0;
            shape_text(c.dl(), f, pre.c_str(), pp, th.text, view_w, gs);
            const f32 pw = text_width(f, pre.c_str());
            // 下划线是区分合成中文本与已提交文本的唯一标记，不是装饰。
            if (pw > 0.0f) c.dl().push(Rect{x0, pp.y - ascent + 2.0f, pw, 1.0f}, th.accent_color, 0.25f);
            preedit_x_ = x0 + text_offset_x(f, pre.c_str(), tb.preedit_caret());
            preedit_drawn_ = true;
        }

        // ---- 光标 ----
        if (focused && caret_visible(c, st))
        {
            const size_t crow = row_of_byte(tb.caret());
            const WrapSpan &cr = rows[crow];
            const f32 cx = preedit_drawn_
                               ? preedit_x_
                               : origin.x + text_width_n(f, s.data() + cr.begin, tb.caret() - cr.begin);
            c.dl().push(Rect{cx, row_y(crow), 1.5f, lh}, th.accent_color, 0.2f);
        }

        // ---- 滚动条 ----
        if (v_scroll)
        {
            const Rect track{r.right() - sb_w + 3.0f, r.y + pad_y, 3.0f, view_h};
            c.dl().push_rounded(track, th.control_border, CornerRadii(1.5f), 0.3f);
            // 比例按视觉行：总行数才是内容高度，逻辑行数在软换行下会低估。
            f32 thumb_h = view_h * ((f32)visible_rows_est / (f32)total_rows);
            // 抓不住的滑块不叫滑块。按比例缩小前先保证最小尺寸。
            if (thumb_h < 14.0f) thumb_h = 14.0f;
            if (thumb_h > view_h) thumb_h = view_h;
            const f32 range = content_h - view_h;
            // 位置按首个视觉行在总行数中的比例 —— st.scroll 是逻辑行号，
            // 直接乘 lh 在软换行下不等比于像素偏移。
            const f32 t = range > 0.0f ? ((f32)first_visual_row * lh) / range : 0.0f;
            const Rect thumb{track.x, track.y + t * (view_h - thumb_h), track.w, thumb_h};
            c.dl().push_rounded(thumb, th.text_tertiary, CornerRadii(1.5f), 0.3f);
        }

        // ---- 写回，截断 ----
        const size_t room = (size_t)capacity - 1;
        const size_t n = s.size() < room ? s.size() : room;
        memcpy(buffer, s.data(), n);
        buffer[n] = 0;

        return focused;
    }

} // namespace fox
