// ============================================================
// fox_metrics_test.cpp —— 字形基线（baseline）不变量
//
// Fox 曾经把**每一个字形**都画在基线下方 4~7 像素处，而且每个字的高度还
// 不一样（`bitmap_top = face_->ascent + lgy`：符号错了一项，量纲也错了一支）。
// 这在"看起来还行"和"整行文字歪着"之间没有清晰界限 —— 人眼对 3 像素的
// 整体下移几乎免疫，尤其是在深色标题栏上。它活了很久，直到有人把渲染结果
// 裁开、量了像素才被发现。
//
// 所以这里不检查"看起来对"，而是检查三条**可以量**的不变量：
//
//   [1] 字形的位图底边必须落在基线上下 1 像素以内（字体设计的事实）
//   [2] 同一个字，无论被问到多少次，度量必须完全相同（度量是缓存的）
//   [3] atlas 代际号必须随"新字形第一次被栅格化"单调增长 —— damage 跟踪
//       就是靠这个数发现"显示列表没变但像素变了"
//
// 一个真字体的度量也顺便被打印出来，因为失败时第一件想知道的事永远是
// "到底用的是哪个文件"（候选列表有十几个路径）。
//
// 无需窗口、无需 GPU、无需 Vulkan。运行：
//   clang++ -std=c++20 -O2 -I runtime/include/ciUd/fox runtime/tests/fox_metrics_test.cpp \
//       <font.cpp 依赖> -o fox_metrics_test && ./fox_metrics_test
// ============================================================

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

    /// 字形位图的底边相对基线的位置（屏幕坐标，**向下为正**）。
    ///
    /// 摆放公式（shape_text）：top = baseline - bitmap_top ⇒
    ///     bottom = top + h = baseline - bitmap_top + h
    /// 所以「底边在基线下方多少」= h - bitmap_top。
    ///
    /// ⚠ 这里最初按 bitmap_top 本身写，量到 +7 px 就以为是回归 —— 其实是
    ///   测试把既约量当成了位置量。这正是这类测试最容易犯的错：写错了
    ///   方向，失败看起来像 bug。
    f32 glyph_bottom_rel(const AtlasGlyph &g) { return g.pixel_rect.h - g.bitmap_top; }
} // namespace

int main()
{
    FontManager &fm = fonts();

    // 用系统 UI 字体：与真实运行路径完全一致（包括候选列表里的 CJK 字体）。
    const f32 size = 16.0f;
    FontId font = fm.load_system_ui(size);
    if (font == FONT_INVALID)
    {
        std::printf("跳过：本机没有找到可用的系统字体（不是失败）\n");
        return 0;
    }

    const FontFace *face = fm.face(font);
    // ⚠ 打印实际打开的文件：候选列表有十几个路径，失败时第一件想知道的事
    //   永远是"到底用的哪个文件"。FontManager 通过 source_of() 暴露它
    //   （FontFace 自己不知道来源）。
    std::printf("字体: %s (ascent=%.2f descent=%.2f line=%.2f pixel=%.2f)\n",
                fm.source_of(font).c_str(), face ? face->ascent : 0.0f,
                face ? face->descent : 0.0f, face ? face->line_height : 0.0f,
                face ? face->pixel_size : 0.0f);

    // ── [1] 基线不变量 ────────────────────────────────────────────────
    //
    // 没有降部的字符，底边必须落在基线上（|bottom| <= 1px）。有降部的
    // 字符（g/p/y）允许到 descent 那么低，但**必须低于基线**（bottom < 0），
    // 否则说明位图被整体抬起来了。
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
        // 降部至少要低于基线 1 像素，否则位图被整体抬起来了
        if (glyph_bottom_rel(g) < 1.5f) descended = false;
    }
    if (any_descender)
        check(descended, "有降部字形（g/p/y）确实低于基线");

    // 顶边必须落在基线**上方**（bitmap_top > 0）；等于 0 说明整条摆放公式
    // 退化成"字形贴着基线往下画"，那正是曾经的那个 bug。
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

    // 每个字形的高度都必须与字体给出的像素尺寸相称：16px 字体里出现一个
    // 60px 高的字形，说明位图被缩放错了（而不是"字体就是这样"）。
    bool sizes_sane = true;
    for (const char *p = "AWMgqj"; *p; ++p)
    {
        const AtlasGlyph &g = fm.glyph_at(font, (u32)(u8)*p);
        if (g.empty) continue;
        if (g.pixel_rect.h <= 0.0f || g.pixel_rect.h > size * 1.6f) sizes_sane = false;
        if (g.m.advance <= 0.0f || g.m.advance > size * 2.0f) sizes_sane = false;
        // 位图顶边不能高过字体的 ascent（否则字形会顶出行盒）
        if (g.bitmap_top > face->ascent + 1.0f) sizes_sane = false;
    }
    check(sizes_sane, "字形高度/advance 与字体像素尺寸相称");

    // ── [2] 度量是稳定的 ──────────────────────────────────────────────
    // 字形被缓存，第二次询问必须给出完全相同的度量（含浮点位模式）。
    {
        const AtlasGlyph &a = fm.glyph_at(font, (u32)'A');
        const AtlasGlyph b = a; // 拷贝
        const AtlasGlyph &a2 = fm.glyph_at(font, (u32)'A');
        check(std::memcmp(&b, &a2, sizeof(AtlasGlyph)) == 0, "重复询问同一字形得到相同度量");
    }

    // ── [3] atlas 代际号单调 ──────────────────────────────────────────
    //
    // damage 跟踪靠这个数发现"显示列表没变、像素却变了"（字形是惰性
    // 栅格化的）。它必须是单调递增的 —— 只要新增一个字，值就得变。
    {
        const u32 before_all = fm.atlas_generation();

        // 先确认它已经不再变化（重复询问同一个字不应再让它增长）
        fm.glyph_at(font, (u32)'A');
        fm.glyph_at(font, (u32)'A');
        const u32 stable = fm.atlas_generation();
        check(stable == before_all, "重复询问已缓存的字形不会改变代际号");

        // 找一个本字体里还没被问过的码点（CJK 常用字，绝大多数场合没被
        // 上面的 ASCII 覆盖）。
        GlyphAtlas *atlas = fm.atlas(font);
        const u32 before_new = atlas->generation();
        const AtlasGlyph &g = fm.glyph_at(font, 0x9F99 /* 龙 */);
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