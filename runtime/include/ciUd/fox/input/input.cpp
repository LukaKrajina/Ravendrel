#include "input.hpp"

namespace fox
{

    void InputState::end_frame()
    {
        mouse_scroll = Vec2{0.0f, 0.0f};
        scroll_x = 0.0f;
        scroll_y = 0.0f;

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

}