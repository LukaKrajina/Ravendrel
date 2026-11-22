#include "font_loader.h"
#include "font_search_paths.hpp"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

namespace qgui
{

namespace
{
    bool file_exists(const std::string &p)
    {
        if (p.empty()) return false;
        std::error_code ec;
        return std::filesystem::is_regular_file(p, ec);
    }

    const char *kCjkCandidates[] = {
        fontpath::NOTO_CJK_SC_OTF,
        fontpath::NOTO_CJK_TTC,
        fontpath::NOTO_CJK_PKG_TTC,
        fontpath::WQY_MICROHEI,
        fontpath::WQY_ZENHEI,
        fontpath::DROID_FALLBACK_FULL,
        fontpath::WIN_MSYH,
        fontpath::WIN_SIMHEI,
        fontpath::WIN_SIMSUN,
        fontpath::MAC_PINGFANG,
        fontpath::MAC_STHEITI,
        fontpath::MAC_ARIAL_UNICODE,
    };

    const char *kCyrillicCandidates[] = {
        fontpath::DEJAUVU_SANS,
        fontpath::DEJAUVU_SANS_FLAT,
        fontpath::LIBERATION_SANS,
        fontpath::WIN_ARIAL,
        fontpath::MAC_HELVETICA,
        fontpath::MAC_SFNS,
    };

    const nk_rune kLatinCjkRanges[] = {
        0x0020, 0x00FF,
        0x2010, 0x205F,
        0x2190, 0x21FF,
        0x2500, 0x257F,
        0x25A0, 0x25FF,
        0x2600, 0x26FF,
        0x3000, 0x303F,
        0x4E00, 0x9FFF,
        0xFF00, 0xFFEF,
        0
    };

    const nk_rune kLatinOnlyRanges[] = {
        0x0020, 0x00FF,
        0x2010, 0x205F,
        0x2190, 0x21FF,
        0
    };

    const nk_rune kLatinCyrillicRanges[] = {
        0x0020, 0x00FF,
        0x0400, 0x045F,
        0x2010, 0x205F,
        0x2190, 0x21FF,
        0
    };

    struct FontSpec
    {
        float px;
        bool cjk;
    };

    const FontSpec kFontSpecs[FontSize::FONT_COUNT] = {
        {12.f, true},
        {16.f, true},
        {24.f, true},
        {34.f, false},
    };

    struct FontSlot
    {
        const nk_user_font *handle = nullptr;
        float px = 0.f;
    };
    FontSlot g_fonts[FontSize::FONT_COUNT];
    int g_atlas_glyphs = 0;

    void reset_slots()
    {
        for (int i = 0; i < FontSize::FONT_COUNT; ++i)
        {
            g_fonts[i].handle = nullptr;
            g_fonts[i].px = 0.f;
        }
        g_atlas_glyphs = 0;
    }
}

void set_atlas_glyphs(int n) { g_atlas_glyphs = n; }


bool find_i18n_font(std::string &out_cjk, std::string &out_cyrillic)
{
    out_cjk.clear();
    out_cyrillic.clear();
    for (const char *c : kCjkCandidates)
    {
        if (file_exists(c))
        {
            out_cjk = c;
            break;
        }
    }
    for (const char *c : kCyrillicCandidates)
    {
        if (file_exists(c))
        {
            out_cyrillic = c;
            break;
        }
    }
    return !out_cjk.empty() || !out_cyrillic.empty();
}


void load_i18n_fonts(struct nk_font_atlas *atlas)
{
    reset_slots();
    if (!atlas) return;

    std::string cjk, cyr;
    find_i18n_font(cjk, cyr);

    if (cjk.empty() && cyr.empty())
    {
        for (int i = 0; i < FontSize::FONT_COUNT; ++i)
        {
            struct nk_font *f = nk_font_atlas_add_default(atlas, kFontSpecs[i].px, nullptr);
            if (f)
            {
                g_fonts[i].handle = &f->handle;
                g_fonts[i].px = kFontSpecs[i].px;
            }
        }
        return;
    }

    for (int i = 0; i < FontSize::FONT_COUNT; ++i)
    {
        const FontSpec &spec = kFontSpecs[i];

        const char *path = !cjk.empty() ? cjk.c_str() : cyr.c_str();
        struct nk_font_config cfg = nk_font_config(spec.px);
        cfg.range = spec.cjk ? kLatinCjkRanges
                             : (!cjk.empty() ? kLatinOnlyRanges : kLatinCyrillicRanges);

        struct nk_font *f = nk_font_atlas_add_from_file(atlas, path, spec.px, &cfg);
        if (f)
        {
            g_fonts[i].handle = &f->handle;
            g_fonts[i].px = spec.px;
        }

        if (!cyr.empty())
        {
            struct nk_font_config cfg2 = nk_font_config(spec.px);
            cfg2.merge_mode = nk_true;
            cfg2.range = kLatinCyrillicRanges;
            nk_font_atlas_add_from_file(atlas, cyr.c_str(), spec.px, &cfg2);
        }
    }

    if (font_count() == 0)
    {
        struct nk_font *f = nk_font_atlas_add_default(atlas, 16.f, nullptr);
        if (f)
        {
            g_fonts[FontSize::FONT_BODY].handle = &f->handle;
            g_fonts[FontSize::FONT_BODY].px = 16.f;
        }
    }
}


const struct nk_user_font *font_at(int index)
{
    if (index < 0 || index >= FontSize::FONT_COUNT) return nullptr;
    return g_fonts[index].handle;
}

int font_count()
{
    int n = 0;
    for (int i = 0; i < FontSize::FONT_COUNT; ++i)
        if (g_fonts[i].handle) ++n;
    return n;
}

float font_px(int index)
{
    if (index < 0 || index >= FontSize::FONT_COUNT) return 0.f;
    return g_fonts[index].px;
}

int atlas_info()
{
    return g_atlas_glyphs;
}

}