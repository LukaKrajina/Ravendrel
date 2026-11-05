#pragma once
// Fox input/input.hpp - 一帧的规范化输入状态
//
// 与后端无关。Vulkan/GLFW 后端每帧把 OS 事件泵进来；控件查询它。保留
// 边沿触发状态（pressed/released），无论帧率多少，控件对一次点击都恰好
// 看到一次。

#include "core/types.hpp"
#include <vector>

namespace fox
{

    // ── 鼠标按键 ───────────────────────────────────────────────────

    enum class MouseButton : u8
    {
        Left = 0,
        Right,
        Middle,
        Count
    };

    /// 每个按键当前帧与上一帧的状态。
    struct ButtonState
    {
        bool down = false;   ///< 当前按住
        bool pressed = false;///< 本帧按下
        bool released = false;///< 本帧松开
        f32 x = 0.0f;       ///< 按下起始位置
        f32 y = 0.0f;
        f32 down_time = 0.0f;
        f32 up_time = 0.0f;
        bool double_click = false;
        int click_count = 0;
    };

    // ── 按键 ────────────────────────────────────────────────────────

    // 可打印 ASCII 用自身编码。命名键用 0x100 以上的值，与 UTF-8 字节
    // 永不冲突。
    enum class Key : u16
    {
        None = 0,
        Backspace = 0x100,
        Tab,
        Enter,
        Escape,
        Space = 0x20, // ' ' 与 ASCII 一致
        Delete,
        Up,
        Down,
        Left,
        Right,
        Home,
        End,
        PageUp,
        PageDown,
        Shift,
        Control,
        Alt,
        Super,
        F1,
        F2,
        F3,
        F4,
        F5,
        F6,
        F7,
        F8,
        F9,
        F10,
        F11,
        F12,
    };

    /// 数组必须提供的按键槽位数。
    ///
    /// 两个不相交的区段共用一个索引空间：可打印 ASCII（含 `' '` 槽位）在
    /// 0..127，命名键在 0x100..0x11C。数组只按 ASCII 定大小正是这个常量
    /// 要防的 bug。取整到超过 F12（0x11C）并留有余量。
    inline constexpr int KEY_SLOTS = 0x120;

    // ── 输入状态 ─────────────────────────────────────────────────────

    /// 一帧的输入。帧末重置（清边沿）。
    struct InputState
    {
        // 指针
        Vec2 mouse_pos;
        Vec2 mouse_delta;
        Vec2 mouse_scroll;   ///< 本帧累计滚轮
        f32 scroll_x = 0.0f;
        f32 scroll_y = 0.0f;
        ButtonState buttons[(int)MouseButton::Count];

        // 键盘
        //
        // ⚠ 这些数组必须是 KEY_SLOTS 长，不是 128。命名键从
        //   Key::Backspace = 0x100（256）开始，128 项的数组在写入
        //   Backspace/Tab/Enter/Escape/方向键的那一刻就被越界写了 128 字节
        //   ——紧随其后的 `chars`、修饰键和指针状态全部被踩坏。症状看着像
        //   "方向键把鼠标搞坏了"，所以一直没被发现。ArraySanitizer 一次就
        //   抓到了它；就是边界写错了。
        bool keys_down[KEY_SLOTS] = {false};
        bool keys_pressed[KEY_SLOTS] = {false};
        bool keys_released[KEY_SLOTS] = {false};
        std::vector<u32> chars; ///< 本帧键入的 Unicode 码点
        bool shift = false;
        bool ctrl = false;
        bool alt = false;
        bool super_key = false;

        // 修饰键的边沿辅助
        bool key_down(Key k) const { int i = k_index(k); return i >= 0 && i < KEY_SLOTS && keys_down[i]; }
        bool key_press(Key k) const { int i = k_index(k); return i >= 0 && i < KEY_SLOTS && keys_pressed[i]; }
        bool key_released_now(Key k) const
        {
            int i = k_index(k);
            return i >= 0 && i < KEY_SLOTS && keys_released[i];
        }

        f32 time = 0.0f; ///< 应用时间（秒），供动画用

        /// 清除每帧的边沿和累加器（控件消费完后调用）。
        void end_frame();

    private:
        static int k_index(Key k)
        {
            // 槽位即键值：可打印 ASCII 占 0..127，命名键占 0x100..0x11C，
            // 二者共用同一索引空间 —— KEY_SLOTS 存在的全部理由（见上方
            // 数组注释）。
            //
            // 曾经这里写 `v < 128 ? v : -1`，于是 key_press(Key::Left) 求值成
            // keys_pressed[-1]：越界读数组**前一个字节**（恰好是 keys_down
            // 的末尾、一个从不被写入的槽位，恒为 false）。症状是方向键 /
            // Home / End / Backspace / Enter / Escape 在文本控件里**全部
            // 静默失效** —— 而守卫 `k_index(k) < KEY_SLOTS` 对 -1 恒真，
            // 拦不住它。文本测试只测 TextBuffer 模型、不经按键路径，所以
            // 这个 bug 活着且无人知晓。
            return (int)k;
        }
    };

} // namespace fox