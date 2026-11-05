#pragma once
#include "../components/window.h"
#include "../i18n.hpp"
#include <cstdio>

namespace qgui
{
    // 测量结果的时间序列以及近期结果的分布。
    class MeasurementHistoryWindow : public IWindow
    {
    public:
        const char *title() const override { return tr("Measurement History"); }

        void render(nk_context *ctx, const StateSnapshot &snap, float dt) override
        {
            (void)dt;
            if (nk_begin(ctx, title(), nk_rect(760, 470, 460, 360),
                         NK_WINDOW_BORDER | NK_WINDOW_TITLE | NK_WINDOW_MOVABLE | NK_WINDOW_SCALABLE | NK_WINDOW_MINIMIZABLE | NK_WINDOW_CLOSABLE))
            {
                size_t m = snap.measurements.size();
                if (m == 0)
                {
                    nk_layout_row_dynamic(ctx, 20, 1);
                    nk_label(ctx, tr("No measurements yet"), NK_TEXT_LEFT);
                    nk_end(ctx);
                    return;
                }

                int c0 = 0, c1 = 0;
                for (const auto &r : snap.measurements)
                {
                    if (r.result == 0)
                        c0++;
                    else
                        c1++;
                }

                nk_layout_row_dynamic(ctx, 20, 1);
                char stat[128];
                snprintf(stat, sizeof(stat), tr("%zu samples: |0> = %d, |1> = %d"), m, c0, c1);
                nk_label(ctx, stat, NK_TEXT_LEFT);
                size_t cnt = m > 128 ? 128 : m;
                nk_layout_row_dynamic(ctx, 120, 1);
                if (nk_chart_begin(ctx, NK_CHART_LINES, static_cast<int>(cnt), -0.1f, 1.1f))
                {
                    for (size_t i = m - cnt; i < m; ++i)
                    {
                        nk_chart_push(ctx, static_cast<float>(snap.measurements[i].result));
                    }
                    nk_chart_end(ctx);
                }

                int bins[2] = {0, 0};
                size_t hcnt = m > 32 ? 32 : m;
                for (size_t i = m - hcnt; i < m; ++i)
                {
                    int v = snap.measurements[i].result;
                    if (v >= 0 && v < 2)
                        bins[v]++;
                }
                nk_layout_row_dynamic(ctx, 20, 1);
                char hline[128];
                snprintf(hline, sizeof(hline), tr("Last %zu: |0> = %d, |1> = %d"), hcnt, bins[0], bins[1]);
                nk_label(ctx, hline, NK_TEXT_LEFT);
            }
            nk_end(ctx);
        }
    };
}
