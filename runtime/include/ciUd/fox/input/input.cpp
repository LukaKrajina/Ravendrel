// Fox input/input.cpp - 每帧边沿管理

#include "input.hpp"

namespace fox
{

    void InputState::end_frame()
    {
        mouse_scroll = Vec2{0.0f, 0.0f};
        scroll_x = 0.0f;
        scroll_y = 0.0f;

        // 用 KEY_SLOTS，不是 128。只清 ASCII 那半边的话，第一次按下
        // Tab / Enter / 方向键 / F 键之后，keys_pressed[0x100..0x11C] 会
        // 永远锁在 true——再按第二个键时就像同时按住了好几个键。
        for (int i = 0; i < KEY_SLOTS; ++i)
        {
            keys_pressed[i] = false;
            keys_released[i] = false;
        }

        for (int i = 0; i < (int)MouseButton::Count; ++i)
        {
            buttons[i].pressed = false;
            buttons[i].released = false;
            buttons[i].double_click = false;
        }

        chars.clear();
    }

} // namespace fox