#include <string.h>
#include <stdio.h>
#include <limits.h>
#include "circuit_grid.h"

namespace qgui
{

    void CircuitGridWindow::render(nk_context *ctx, const StateSnapshot &snap, float dt)
    {
        (void)dt;
        if (nk_begin(ctx, title(), nk_rect(30, 30, 700, 320),
                     NK_WINDOW_BORDER | NK_WINDOW_TITLE | NK_WINDOW_MOVABLE | NK_WINDOW_SCALABLE | NK_WINDOW_MINIMIZABLE | NK_WINDOW_CLOSABLE))
        {
            nk_layout_space_begin(ctx, NK_STATIC, 260, INT_MAX);
            struct nk_command_buffer *canvas = nk_window_get_canvas(ctx);
            struct nk_rect region = nk_layout_space_bounds(ctx);

            int active_qubits = static_cast<int>(snap.num_qubits);
            if (active_qubits <= 0)
            {
                nk_layout_row_dynamic(ctx, 20, 1);
                nk_label(ctx, "No quantum state received yet (daemon offline?)", NK_TEXT_LEFT);
                nk_layout_space_end(ctx);
                nk_end(ctx);
                return;
            }

            const float row_height = 36.0f;
            const float col_width = 58.0f;
            const float start_x = region.x + 60.0f;
            const float start_y = region.y + 30.0f;

            // 带有标签的水平量子比特线
            for (int i = 0; i < active_qubits; ++i)
            {
                float wire_y = start_y + i * row_height;
                char label[16];
                snprintf(label, sizeof(label), "q[%d]", i);
                nk_draw_text(canvas, nk_rect(region.x + 5, wire_y - 10, 50, 20),
                             label, static_cast<int>(strlen(label)), ctx->style.font,
                             nk_rgb(200, 200, 200), nk_rgb(40, 40, 40));
                nk_stroke_line(canvas, start_x, wire_y, start_x + 640.0f, wire_y,
                               1.5f, nk_rgb(90, 90, 90));
            }

            // 来自快照的门序列（显示最近的窗口）
            const int max_steps = 12;
            int total = static_cast<int>(snap.gates.size());
            int start_gate = total > max_steps ? total - max_steps : 0;
            for (int gi = start_gate; gi < total; ++gi)
            {
                const GateRecord &ev = snap.gates[gi];
                int col = gi - start_gate;
                float gate_x = start_x + (col * col_width) + 18.0f;
                float target_y = start_y + ev.target * row_height;

                if (ev.control >= 0 && ev.control < active_qubits)
                {
                    float control_y = start_y + ev.control * row_height;
                    nk_stroke_line(canvas, gate_x, control_y, gate_x, target_y,
                                   1.5f, nk_rgb(150, 200, 255));
                    nk_fill_circle(canvas, nk_rect(gate_x - 4, control_y - 4, 8, 8),
                                   nk_rgb(150, 200, 255));
                    nk_fill_circle(canvas, nk_rect(gate_x - 9, target_y - 9, 18, 18),
                                   nk_rgb(150, 200, 255));
                }
                else
                {
                    struct nk_rect gate_box = nk_rect(gate_x - 13, target_y - 13, 26, 26);
                    nk_fill_rect(canvas, gate_box, 3.0f, nk_rgb(80, 120, 200));
                    nk_draw_text(canvas, nk_rect(gate_box.x + 6, gate_box.y + 4, 20, 20),
                                 ev.name, static_cast<int>(strlen(ev.name)), ctx->style.font,
                                 nk_rgb(255, 255, 255), nk_rgb(80, 120, 200));
                }
            }
            nk_layout_space_end(ctx);
        }
        nk_end(ctx);
    }
}