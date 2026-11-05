#pragma once
#include "../components/window.h"
#include "../i18n.hpp"
#include <cstdio>
#include <string>
#include <deque>

namespace qgui
{
    // 帧时序/帧率（FPS）、守护进程连接状态、后端及呈现信息。
    class MetricsWindow : public IWindow
    {
    public:
        const char *title() const override { return tr("Metrics"); }

        void render(nk_context *ctx, const StateSnapshot &snap, float dt) override
        {
            if (dt > 0.0f && dt < 1.0f)
            {
                frame_times_.push_back(dt);
                if (frame_times_.size() > 120)
                    frame_times_.pop_front();
            }

            if (nk_begin(ctx, title(), nk_rect(720, 810, 460, 260),
                         NK_WINDOW_BORDER | NK_WINDOW_TITLE | NK_WINDOW_MOVABLE | NK_WINDOW_SCALABLE | NK_WINDOW_MINIMIZABLE | NK_WINDOW_CLOSABLE))
            {
                double avg = 0.0;
                for (double t : frame_times_)
                    avg += t;
                avg = frame_times_.empty() ? 0.0 : avg / static_cast<double>(frame_times_.size());
                float fps = avg > 0.0 ? static_cast<float>(1.0 / avg) : 0.0f;

                nk_layout_row_dynamic(ctx, 20, 1);
                char l1[128];
                snprintf(l1, sizeof(l1), tr("FPS: %.1f   frame: %.2f ms"), fps, avg * 1000.0);
                nk_label(ctx, l1, NK_TEXT_LEFT);

                char l2[128];
                snprintf(l2, sizeof(l2), tr("Daemon: %s   generation: %llu"),
                         connected_ ? tr("connected") : tr("OFFLINE"),
                         (unsigned long long)snap.generation);
                nk_label(ctx, l2, NK_TEXT_LEFT);

                char l3[128];
                snprintf(l3, sizeof(l3), tr("Backend: %s"), snap.backend_name.c_str());
                nk_label(ctx, l3, NK_TEXT_LEFT);

                char l4[128];
                snprintf(l4, sizeof(l4), tr("Qubits: %u   amplitudes: %zu"),
                         snap.num_qubits, snap.amplitudes.size());
                nk_label(ctx, l4, NK_TEXT_LEFT);

                char l5[128];
                snprintf(l5, sizeof(l5), tr("Present mode: %s"), present_mode_.c_str());
                nk_label(ctx, l5, NK_TEXT_LEFT);
            }
            nk_end(ctx);
        }

        void set_connected(bool c) { connected_ = c; }
        void set_present_mode(std::string name) { present_mode_ = std::move(name); }

    private:
        bool connected_ = false;
        std::string present_mode_ = "MAILBOX";
        std::deque<float> frame_times_;
    };
}