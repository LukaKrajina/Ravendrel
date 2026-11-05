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

    // 首选 CJK + Latin（覆盖 + CJK + Latin），次选 Latin/CJK。
    // 路径字符串的单一来源是 font_search_paths.hpp；此处的**顺序**是本 UI
    // 自己的优先级（与 fox 文本引擎的那份不同，各自演化，勿合并）。
    const char *kCjkCandidates[] = {
        // Linux：Noto CJK / 文泉驿 / Droid
        fontpath::NOTO_CJK_SC_OTF,
        fontpath::NOTO_CJK_TTC,
        fontpath::NOTO_CJK_PKG_TTC,
        fontpath::WQY_MICROHEI,
        fontpath::WQY_ZENHEI,
        fontpath::DROID_FALLBACK_FULL,
        // Windows
        fontpath::WIN_MSYH,
        fontpath::WIN_SIMHEI,
        fontpath::WIN_SIMSUN,
        // macOS
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

    // ── 字符集 ────────────────────────────────────────────────────────
    //
    // 0x0020-0x00FF  基本拉丁
    // 0x2010-0x205F  常用标点（含 … U+2026、— U+2014）
    // 0x2190-0x21FF  箭头
    // 0x2500-0x257F  制表符（═ ─ │ ┌ 等）
    // 0x25A0-0x25FF  几何图形（■ □ ▲ ● 等）
    // 0x2600-0x26FF  杂项符号（✓ ✗ ⚠ ★ 等）
    // 0x3000-0x303F  CJK 标点与符号
    // 0x4E00-0x9FFF  CJK 统一表意文字（约 2.1 万字形，**图集内存的主要占用**）
    // 0xFF00-0xFFEF  全角形式（：！（）、，）
    //
    // ⚠ 全角这一段是必需的：中文界面里「：」「（」「）」几乎每句都有。
    //   漏掉的症状是这些字符变成豆腐块，而且走的是 stb_truetype 的
    //   字形缺失路径，**程序不报错**，只是画面不对。
    //
    // ⚠ 0x2500-0x26FF 这四段（合计 600 字形，约 0.5 MB）是为**编辑器**
    //   加的：代码里到处是框线注释和符号注释，缺了就是一屏问号。
    //   一开始没加，打开 state.rvdl 发现每个 ═ 都变成 ?，
    //   而在编辑器语境下这个「?」看起来像是**代码里真的有问号**，
    //   根本不会想到是字体缺字。
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

    // 纯拉丁档：给大字号用。34px 的 2.1 万个汉字要 ~19 MB 图集，
    // 而大字号在界面上只出现项目名和数字，不值得。
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

    // 字号阶梯。间隔约 1.5 倍 —— 太小看不出来，太大图集吃不消。
    const FontSpec kFontSpecs[FontSize::FONT_COUNT] = {
        {12.f, true},  // FONT_TINY    CJK：状态栏有中文
        {16.f, true},  // FONT_BODY    CJK：正文
        {24.f, true},  // FONT_TITLE   CJK：区块标题
        {34.f, false}, // FONT_DISPLAY 拉丁：项目名 / 数字
    };

    // 烘焙结果。与 atlas 同生命周期（atlas 在 RenderContext::init 里 stash 一次）。
    struct FontSlot
    {
        const nk_user_font *handle = nullptr;
        float px = 0.f;
    };
    FontSlot g_fonts[FontSize::FONT_COUNT];
    /// 烘焙进去的字形数。由本文件末尾的 set_atlas_glyphs 回填。
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
} // namespace

/// 记下图集字形数（RenderContext 在 bake 之后回填）。
///
/// ⚠ 必须定义在匿名命名空间**外面**：早先那个 set_atlas_size 被关在
///   匿名命名空间里、声明也只在 .cpp 里，于是全仓库**没有任何地方调得到**
///   —— 编译得过、链接得过，atlas_info() 于是恒为 0。
///   「回填函数没人调」这件事没有任何编译期信号，只能靠找引用发现。
void set_atlas_glyphs(int n) { g_atlas_glyphs = n; }

// ═══════════════════════════════════════════════════════════════════════
// 查找
// ═══════════════════════════════════════════════════════════════════════

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

// ═══════════════════════════════════════════════════════════════════════
// 烘焙
// ═══════════════════════════════════════════════════════════════════════

void load_i18n_fonts(struct nk_font_atlas *atlas)
{
    reset_slots();
    if (!atlas) return;

    std::string cjk, cyr;
    find_i18n_font(cjk, cyr);

    if (cjk.empty() && cyr.empty())
    {
        // 连系统字体都找不到：退回 ProggyClean（ASCII）。
        // 必须保证至少有一档可用，否则 nk_begin 会因为 style.font 为空而崩。
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

        // 主字体：有 CJK 就用 CJK（含拉丁字形），否则用西里尔字体
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

        // 西里尔字符用第二个字体合并（merge_mode 复用同一张图集）
        if (!cyr.empty())
        {
            struct nk_font_config cfg2 = nk_font_config(spec.px);
            cfg2.merge_mode = nk_true;
            cfg2.range = kLatinCyrillicRanges;
            nk_font_atlas_add_from_file(atlas, cyr.c_str(), spec.px, &cfg2);
        }
    }

    // 一档都没成功：兜底
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

// ═══════════════════════════════════════════════════════════════════════
// 查询
// ═══════════════════════════════════════════════════════════════════════

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

} // namespace qgui
