#include "core/context.hpp"
#include "widget/primitives.hpp"
#include "window/window.hpp"
#include "backend/cpu_backend.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace fox;

namespace
{
    constexpr int kCell = 32;
    constexpr int kIconCount = 4;

    Rect icon_cell(int i)
    {
        const float w = 1.0f / (float)kIconCount;
        return Rect{(float)i * w, 0.0f, w, 1.0f};
    }

    std::vector<u8> build_icon_sheet(int w, int h)
    {
        std::vector<u8> px((size_t)w * h * 4, 0);
        for (int cell = 0; cell < kIconCount; ++cell)
        {
            const int ox = cell * kCell;
            for (int y = 0; y < kCell; ++y)
                for (int x = 0; x < kCell; ++x)
                {
                    const float fx = (float)x + 0.5f, fy = (float)y + 0.5f;
                    float cov = 0.0f;
                    switch (cell)
                    {
                    case 0: cov = std::sqrt((fx - 16) * (fx - 16) + (fy - 16) * (fy - 16)) <= 12.0f; break;
                    case 1: { const float d = std::sqrt((fx - 16) * (fx - 16) + (fy - 16) * (fy - 16)); cov = d <= 12.0f && d >= 7.0f; break; }
                    case 2: { const float ty = fy / 26.0f; cov = ty <= 1.0f && std::fabs(fx - 16.0f) <= ty * 12.0f; break; }
                    default: { const float dx = std::fabs(fx - 16.0f), dy = std::fabs(fy - 16.0f); cov = dx <= 13.0f && dy <= 13.0f && !(dx <= 9.0f && dy <= 9.0f); break; }
                    }
                    if (!cov) continue;
                    const size_t o = ((size_t)y * w + (size_t)(ox + x)) * 4;
                    px[o + 0] = 255; px[o + 1] = 255; px[o + 2] = 255; px[o + 3] = 255;
                }
        }
        return px;
    }
}

int main(int argc, char **argv)
{
    const char *out = argc > 1 ? argv[1] : "fox_headless.ppm";
    const bool want_selection = argc > 2 && argv[2][0] == 's';
    const bool want_ime = argc > 2 && argv[2][0] == 'i';
    const bool latin_primary = argc > 3 && argv[3][0] == 'l';
    const int W = 1000, H = 680;

    Context &c = ctx();
    c.init("Fox headless", W, H);

    if (latin_primary)
    {
        c.set_font_file("C:/Windows/Fonts/segoeui.ttf");
        std::fprintf(stderr, "[headless] primary forced to a Latin-only face\n");
    }

    c.set_fixed_dt(1.0f / 60.0f);

    std::vector<u8> sheet = build_icon_sheet(kCell * kIconCount, kCell);
    TexId icons = c.create_texture((u32)(kCell * kIconCount), (u32)kCell, sheet.data());

    CpuBackend cpu;
    if (!cpu.init(W, H))
    {
        std::fprintf(stderr, "[headless] buffer allocation failed\n");
        return 1;
    }

    bool g_switch = true, g_check = true;
    float g_slider = 0.62f, g_slider2 = 0.30f;
    char g_buf[128] = "Fox 图形框架";

    for (int f = 0; f < 60; ++f)
    {
        if (f == 0)
        {
            const FontId primary = c.font(c.theme().font_body);
            std::fprintf(stderr, "[headless] primary=%s  A=%d  CJK(4F60)=%d  chain=%zu\n",
                         fonts().face_source(primary), fonts().has_glyph(primary, 'A') ? 1 : 0,
                         fonts().has_glyph(primary, 0x4F60) ? 1 : 0, fonts().fallback_chain().size());
            for (FontId fb : fonts().fallback_chain())
                std::fprintf(stderr, "[headless]   fallback=%s  CJK(4F60)=%d\n",
                             fonts().face_source(fb), fonts().has_glyph(fb, 0x4F60) ? 1 : 0);
        }

        c.frame_begin();

        Theme &th = c.theme();
        Rect win_rect{0.0f, 0.0f, (float)W, (float)H};
        Window w;
        w.begin(c, win_rect, "Fox", true);

        const char *labels[] = {"Overview", "Circuit", "Operators", "Backends", "Settings"};
        int selected = 1;
        SidebarItem items[5];
        for (int i = 0; i < 5; ++i)
        {
            items[i].label = labels[i];
            items[i].icon_tex = icons;
            items[i].icon_src = icon_cell(i % kIconCount);
            items[i].icon = nullptr;
        }
        int scroll = 0;
        sidebar(c, w.sidebar(), items, 5, selected, scroll);

        Rect cr = w.content();
        f32 pad = th.spacing_lg;
        Rect inner{cr.x + pad, cr.y + pad, cr.w - pad * 2.0f, cr.h - pad * 2.0f};

        label(c, Rect{inner.x, inner.y, inner.w, 28}, "Golden Gate Overview", th.text,
              Align::Start);
        c.dl().push(Rect{inner.x, inner.y + 32.0f, inner.w, th.hairline}, th.separator, 0.2f);

        f32 y = inner.y + 48.0f;

        {
            const f32 s = 28.0f;
            f32 ix = inner.x;
            for (int i = 0; i < kIconCount; ++i)
            {
                image_region(c, Rect{ix, y, s, s}, icons, icon_cell(i), th.accent_color);
                ix += s + 8.0f;
            }
            image_region(c, Rect{ix, y + 4.0f, 20.0f, 20.0f}, icons, icon_cell(1),
                         Color{th.accent_color.r, th.accent_color.g, th.accent_color.b, 102});
            image_region(c, Rect{ix + 28.0f, y, 64.0f, 40.0f}, icons, icon_cell(3), th.text);
            label(c, Rect{ix + 100.0f, y, 200.0f, s}, "icons", th.text_tertiary, Align::Start);
            y += s + th.spacing_md;
        }

        {
            const Rect r{inner.x, y, inner.w, 92.0f};
            const CornerRadii rad(10.0f);
            Shadow sh;
            sh.offset_y = 2.0f;
            sh.blur = 10.0f;
            sh.color = Color{0, 0, 0, 0x24};
            shadow_rect(c, r, rad, sh, 0.5f);
            c.dl().push_rounded(r, th.control_bg, rad, 1.0f);
            c.dl().push_border(r, th.separator, th.hairline, rad, 1.1f);
            f32 cx2 = r.x + th.spacing_lg;
            label(c, Rect{cx2, r.y + th.spacing_md, r.w - th.spacing_lg * 2, 20}, "Liquid Glass",
                  th.text, Align::Start);
            label(c, Rect{cx2, r.y + th.spacing_md + 24.0f, r.w - th.spacing_lg * 2, 40},
                  "Frosted material blurs its backdrop and adds a specular rim.",
                  th.text_secondary, Align::Start);
            y += r.h + th.spacing_md;
        }

        button(c, Rect{inner.x, y, 130, 32}, "Primary", ButtonStyle::Filled);
        button(c, Rect{inner.x + 140, y, 130, 32}, "Secondary");
        button(c, Rect{inner.x + 280, y, 130, 32}, "Disabled", ButtonStyle::Tinted, false);
        y += 44;

        toggle_switch(c, Rect{inner.x, y + 4, 42, 24}, g_switch);
        checkbox(c, Rect{inner.x + 80, y, 200, 24}, "Enable Physics", g_check);
        y += 36;
        slider(c, Rect{inner.x, y, inner.w - 200, 24}, g_slider, 0.f, 1.f);
        y += 36;
        slider(c, Rect{inner.x, y, inner.w - 200, 24}, g_slider2, 0.f, 1.f);
        y += 40;
        const Rect field_r{inner.x, y, 320, 28};
        text_field(c, field_r, g_buf, (int)sizeof(g_buf), "hint", 0);

        y += 44;
        char area_buf[512] =
            "Fox \xE5\xA4\x9A\xE8\xA1\x8C\xE6\x96\x87\xE6\x9C\xAC\xE5\x9F\x9F\n"
            "second line\n"
            "third line\n"
            "fourth line\n"
            "fifth line\n"
            "sixth line\n"
            "seventh line\n"
            "eighth line\n"
            "ninth line\n"
            "tenth line - the caret starts BELOW the fold";
        const Rect area_r{inner.x, y, 320, 120};
        text_area(c, area_r, area_buf, (int)sizeof(area_buf), "notes", 0);

        {
            const Id aid = c.make_id(0xB0000006u, (Id)id_hash_rect(area_r));
            c.focus().set_focus(aid);
            TextBuffer &atb = c.text_edit(aid);
            c.sync_text_from_host(atb, area_buf, strlen(area_buf));
            atb.set_caret(atb.size());
            if (f == 0)
            {
                const WidgetState &ws = c.state(aid);
                std::fprintf(stderr,
                             "[headless] text_area lines=%zu caret_line=%zu box(y=%.1f h=%.1f)\n",
                             atb.line_count(), atb.line_of(atb.caret()), area_r.y, area_r.h);
                std::fprintf(stderr, "[headless]   lh=%.2f pad_y=%.2f scroll=%.2f scroll_x=%.2f\n",
                             fonts().line_height(c.font(th.font_body)), th.spacing_md, ws.scroll,
                             ws.scroll_x);
            }
        }

        if (want_selection || want_ime)
        {
            const Id fid = c.make_id(0xB0000005u, (Id)id_hash_rect(field_r));
            c.focus().set_focus(fid);
            TextBuffer &tb = c.text_edit(fid);
            if (want_selection)
            {
                tb.set_caret(0);
                tb.set_anchor(4);
            }
            if (want_ime)
            {
                c.sync_text_from_host(tb, g_buf, strlen(g_buf));
                c.ime_begin("nihao", 3);
                if (f == 0)
                    std::fprintf(stderr, "[headless] ime composing=%d preedit=\"%s\"\n",
                                 c.ime_composing() ? 1 : 0, tb.preedit().c_str());
            }
        }

        c.frame_end();
        cpu.render(c.dl(), c);
    }

    const size_t painted = cpu.non_background_pixels();
    std::fprintf(stderr, "[headless] %dx%d instances=%u painted=%zu (%.1f%%)\n", W, H,
                 cpu.last_instance_count(), painted,
                 100.0 * (double)painted / (double)((size_t)W * H));

    if (!cpu.write_ppm(out))
    {
        std::fprintf(stderr, "[headless] write_ppm failed\n");
        return 1;
    }
    std::fprintf(stderr, "[headless] wrote %s\n", out);

    if (painted < 1000)
    {
        std::fprintf(stderr, "[headless] FAIL: essentially nothing drawn\n");
        return 1;
    }
    std::fprintf(stderr, "[headless] OK\n");
    return 0;
}