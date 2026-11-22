#pragma once

#include "core/types.hpp"
#include <vector>

namespace fox
{


    enum class MouseButton : u8
    {
        Left = 0,
        Right,
        Middle,
        Count
    };

    struct ButtonState
    {
        bool down = false;
        bool pressed = false;
        bool released = false;
        f32 x = 0.0f;
        f32 y = 0.0f;
        f32 down_time = 0.0f;
        f32 up_time = 0.0f;
        bool double_click = false;
        int click_count = 0;
    };


    enum class Key : u16
    {
        None = 0,
        Backspace = 0x100,
        Tab,
        Enter,
        Escape,
        Space = 0x20,
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

    inline constexpr int KEY_SLOTS = 0x120;


    struct InputState
    {
        Vec2 mouse_pos;
        Vec2 mouse_delta;
        Vec2 mouse_scroll;
        f32 scroll_x = 0.0f;
        f32 scroll_y = 0.0f;
        ButtonState buttons[(int)MouseButton::Count];

        bool keys_down[KEY_SLOTS] = {false};
        bool keys_pressed[KEY_SLOTS] = {false};
        bool keys_released[KEY_SLOTS] = {false};
        std::vector<u32> chars;
        bool shift = false;
        bool ctrl = false;
        bool alt = false;
        bool super_key = false;

        bool key_down(Key k) const { int i = k_index(k); return i >= 0 && i < KEY_SLOTS && keys_down[i]; }
        bool key_press(Key k) const { int i = k_index(k); return i >= 0 && i < KEY_SLOTS && keys_pressed[i]; }
        bool key_released_now(Key k) const
        {
            int i = k_index(k);
            return i >= 0 && i < KEY_SLOTS && keys_released[i];
        }

        f32 time = 0.0f;

        void end_frame();

    private:
        static int k_index(Key k)
        {
            return (int)k;
        }
    };

}