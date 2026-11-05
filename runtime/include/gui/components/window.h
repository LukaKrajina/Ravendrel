#pragma once
#include "../src/nuklear_config.h"
#include "../protocol.hpp"

namespace qgui
{
    class IWindow
    {
    public:
        virtual ~IWindow() = default;
        virtual const char *title() const = 0;
        virtual void render(nk_context *ctx, const StateSnapshot &snap, float dt) = 0;
    };
}
