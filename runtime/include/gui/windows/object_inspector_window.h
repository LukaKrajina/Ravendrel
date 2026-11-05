#pragma once
#include "../components/window.h"
#include "../i18n.hpp"
#include <cstdio>
#include <string>

namespace qgui
{
    // 列出当前存在的量子对象（寄存器/贝尔态/狄拉克态）及其对应的量子比特 ID。
    class ObjectInspectorWindow : public IWindow
    {
    public:
        const char *title() const override { return tr("Quantum Objects"); }
        
        void render(nk_context *ctx, const StateSnapshot &snap, float dt) override
        {
            (void)dt;
            if (nk_begin(ctx, title(), nk_rect(30, 730, 420, 260),
                         NK_WINDOW_BORDER | NK_WINDOW_TITLE | NK_WINDOW_MOVABLE | NK_WINDOW_SCALABLE | NK_WINDOW_MINIMIZABLE | NK_WINDOW_CLOSABLE))
            {
                if (snap.objects.empty())
                {
                    nk_layout_row_dynamic(ctx, 20, 1);
                    nk_label(ctx, tr("No objects"), NK_TEXT_LEFT);
                }

                nk_layout_row_dynamic(ctx, 22, 2);
                nk_label(ctx, tr("Type"), NK_TEXT_LEFT);
                nk_label(ctx, tr("Qubit ids"), NK_TEXT_LEFT);

                for (const auto &o : snap.objects)
                {
                    std::string ids;
                    for (size_t i = 0; i < o.ids.size(); ++i)
                    {
                        char b[16];
                        snprintf(b, sizeof(b), "%d", o.ids[i]);
                        if (i)
                            ids += ", ";
                        ids += b;
                    }
                    // 任意基对象（basis_state）附带的布洛赫方向参数
                    std::string type = o.type;
                    if (o.theta != 0.0 || o.phi != 0.0)
                    {
                        char p[64];
                        snprintf(p, sizeof(p), " (theta=%.2f, phi=%.2f)", o.theta, o.phi);
                        type += p;
                    }
                    nk_layout_row_dynamic(ctx, 22, 2);
                    nk_label(ctx, type.c_str(), NK_TEXT_LEFT);
                    nk_label(ctx, ids.c_str(), NK_TEXT_LEFT);
                }
            }
            nk_end(ctx);
        }
    };
}