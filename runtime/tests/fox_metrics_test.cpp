#include "text/font.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>

using namespace fox;

namespace
{
    int g_failures = 0;

    void check(bool ok, const char *what)
    {
        std::printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
        if (!ok) ++g_failures;
    }

    f32 glyph_bottom_rel(const AtlasGlyph &g) { return g.pixel_rect.h - g.bitmap_top; }
}

int main()
{
    FontManager &fm = fonts();

    const f32 size = 16.0f;
    FontId font = fm.load_system_ui(size);
    if (font == FONT_INVALID)
    {
        std::printf("跳过：本机没有找到可用的系统字体（不是失败）\n");
        return 0;
    }

    const FontFace *face = fm.face(font);
    std::printf("字体: %s (ascent=%.2f descent=%.2f line=%.2f pixel=%.2f)\n",
                fm.source_of(font).c_str(), face ? face->ascent : 0.0f,
                face ? face->descent : 0.0f, face ? face->line_height : 0.0f,
                face ? face->pixel_size : 0.0f);

    const char *no_descender = "oenmascxzvw";
    f32 worst = 0.0f;
    for (const char *p = no_descender; *p; ++p)
    {
        const AtlasGlyph &g = fm.glyph_at(font, (u32)(u8)*p);
        if (g.empty) continue;
        const f32 bottom = glyph_bottom_rel(g);
        if (std::fabs(bottom) > std::fabs(worst)) worst = bottom;
    }
    std::printf("  无降部字符的最差底边偏移: %.2f px\n", worst);
    check(std::fabs(worst) <= 1.0f, "无降部字形的底边落在基线上（±1px）");

    bool any_descender = false;
    bool descended = true;
    for (const char *p = "gpy"; *p; ++p)
    {
        const AtlasGlyph &g = fm.glyph_at(font, (u32)(u8)*p);
        if (g.empty) continue;
        any_descender = true;
        if (glyph_bottom_rel(g) < 1.5f) descended = false;
    }
    if (any_descender)
        check(descended, "有降部字形（g/p/y）确实低于基线");

    {
        bool tops_ok = true;
        for (const char *p = "Aol"; *p; ++p)
        {
            const AtlasGlyph &g = fm.glyph_at(font, (u32)(u8)*p);
            if (g.empty) continue;
            if (g.bitmap_top <= 0.5f) tops_ok = false;
        }
        check(tops_ok, "字形顶边在基线上方（bitmap_top > 0）");
    }

    bool sizes_sane = true;
    for (const char *p = "AWMgqj"; *p; ++p)
    {
        const AtlasGlyph &g = fm.glyph_at(font, (u32)(u8)*p);
        if (g.empty) continue;
        if (g.pixel_rect.h <= 0.0f || g.pixel_rect.h > size * 1.6f) sizes_sane = false;
        if (g.m.advance <= 0.0f || g.m.advance > size * 2.0f) sizes_sane = false;
        if (g.bitmap_top > face->ascent + 1.0f) sizes_sane = false;
    }
    check(sizes_sane, "字形高度/advance 与字体像素尺寸相称");

    {
        const AtlasGlyph &a = fm.glyph_at(font, (u32)'A');
        const AtlasGlyph b = a;
        const AtlasGlyph &a2 = fm.glyph_at(font, (u32)'A');
        check(std::memcmp(&b, &a2, sizeof(AtlasGlyph)) == 0, "重复询问同一字形得到相同度量");
    }

    {
        const u32 before_all = fm.atlas_generation();

        fm.glyph_at(font, (u32)'A');
        fm.glyph_at(font, (u32)'A');
        const u32 stable = fm.atlas_generation();
        check(stable == before_all, "重复询问已缓存的字形不会改变代际号");

        GlyphAtlas *atlas = fm.atlas(font);
        const u32 before_new = atlas->generation();
        const AtlasGlyph &g = fm.glyph_at(font, 0x9F99 );
        const u32 after_new = atlas->generation();
        if (!g.empty)
            check(after_new > before_new, "首次栅格化一个新字形会推进代际号");
        else
            std::printf("  跳过：该字体没有 0x9F99 的字形\n");
    }

    std::printf("\n%s（失败 %d 项）\n", g_failures == 0 ? "FOX-METRICS: 全部通过" : "FOX-METRICS: 失败",
                g_failures);
    return g_failures == 0 ? 0 : 1;
}