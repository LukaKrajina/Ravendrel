#pragma once
#include "window.h"
namespace qgui
{
    class CircuitGridWindow : public IWindow
    {
    public:
        const char *title() const override { return "Quantum Circuit"; }
        void render(nk_context *ctx, const StateSnapshot &snap, float dt) override;
    };
}