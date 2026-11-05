#pragma once
#include "../components/window.h"
#include "../i18n.hpp"

namespace qgui
{

// 设置窗口：提供界面语言切换（English / 中文 / Русский / Français / Deutsch）。
// 选择后立即生效并持久化。
class SettingsWindow : public IWindow
{
    public:
        const char *title() const override { return tr("Settings"); }
        
        void render(nk_context *ctx, const StateSnapshot &snap, float dt) override
        {
            (void)snap;
            (void)dt;
            if (nk_begin(ctx, title(), nk_rect(440, 690, 270, 300),
                        NK_WINDOW_BORDER | NK_WINDOW_TITLE | NK_WINDOW_MOVABLE | NK_WINDOW_SCALABLE | NK_WINDOW_MINIMIZABLE | NK_WINDOW_CLOSABLE))
            {
                nk_layout_row_dynamic(ctx, 20, 1);
                nk_label(ctx, tr("Language"), NK_TEXT_LEFT);

                const Lang current = current_lang();
                const LangEntry *langs = available_langs();
                const int count = available_lang_count();
                for (int i = 0; i < count; ++i)
                {
                    nk_layout_row_dynamic(ctx, 22, 1);
                    if (nk_option_label(ctx, langs[i].label, current == langs[i].lang))
                    {
                        set_lang(langs[i].lang);
                    }
                }
            }
            nk_end(ctx);
        }
    };
}