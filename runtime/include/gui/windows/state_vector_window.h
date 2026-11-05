#pragma once
#include "../components/window.h"
#include "../i18n.hpp"
#include <complex>
#include <cmath>
#include <cstdio>

namespace qgui
{
    // 完整的基态振幅表及概率直方图。
    // 最多可渲染 8 个量子比特（256 个基态）。
    class StateVectorWindow : public IWindow
    {
    public:
        const char *title() const override { return tr("State Vector"); }

        void render(nk_context *ctx, const StateSnapshot &snap, float dt) override
        {
            (void)dt;
            if (nk_begin(ctx, title(), nk_rect(760, 30, 460, 420),
                         NK_WINDOW_BORDER | NK_WINDOW_TITLE | NK_WINDOW_MOVABLE | NK_WINDOW_SCALABLE | NK_WINDOW_MINIMIZABLE | NK_WINDOW_CLOSABLE))
            {
                size_t n = snap.amplitudes.size();
                if (n == 0)
                {
                    nk_layout_row_dynamic(ctx, 20, 1);
                    nk_label(ctx, tr("Waiting for state data..."), NK_TEXT_LEFT);
                    nk_end(ctx);
                    return;
                }

                nk_layout_row_dynamic(ctx, 20, 1);
                char header[128];
                snprintf(header, sizeof(header), tr("%u qubits, %zu basis states (gen %llu)"),
                         snap.num_qubits, n, (unsigned long long)snap.generation);
                nk_label(ctx, header, NK_TEXT_LEFT);
                size_t plot_count = n > 256 ? 256 : n;
                nk_layout_row_dynamic(ctx, 110, 1);
                if (nk_chart_begin(ctx, NK_CHART_COLUMN, static_cast<int>(plot_count), 0.0f, 1.0f))
                {
                    for (size_t i = 0; i < plot_count; ++i)
                    {
                        nk_chart_push(ctx, static_cast<float>(std::norm(snap.amplitudes[i])));
                    }
                    nk_chart_end(ctx);
                }
                nk_layout_row_dynamic(ctx, 18, 1);
                size_t show = n > 256 ? 256 : n;
                for (size_t i = 0; i < show; ++i)
                {
                    const auto &a = snap.amplitudes[i];
                    char line[128];
                    snprintf(line, sizeof(line), "|%-3zu>  % .4f %+.4fi   (%.3f)",
                             i, a.real(), a.imag(), std::norm(a));
                    nk_label(ctx, line, NK_TEXT_LEFT);
                }
            }
            nk_end(ctx);
        }
    };
}