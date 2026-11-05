// ============================================================================
// fox_wrap_test.cpp —— 软换行（视觉行）算法单测
//
// 为什么值得单测
// --------------
// 软换行是「长行在屏幕上的哪里断」的唯一决策点，而它的 bug 全是静默的：
// 断点丢一个字节 → 那个字节从画面上消失；两行之间留缝/重叠 → 光标落点
// 与视觉不一致；宽度算错 → 行尾被裁掉一整列。这些在截图上看不出来（或者
// 看起来像「字体问题」），但每一个都让 text_area 的点击、方向键、选区
// 全部偏移。
//
// 本测试直接驱动 text/soft_wrap.hpp 的 wrap_line，用**真实字体度量**
// （fonts().load_system_ui）构造确定性输入，断言的是结构不变量：
//   · 行段连续无隙（row[i].end == row[i+1].begin）
//   · 行段覆盖全行（首行 begin == ls，末行 end == le）
//   · 拼回去 == 原文（一个字节都不丢）
//   · 词优先：断在空格之后，下一行不以空格开头
//   · 每行不超宽（除非是「单个码点比一行还宽」的合法溢出）
//   · CJK 无空格可依时按码点硬断，且不留空行
//
// 运行：clang++ -std=c++20 -I runtime/include -I runtime/include/ciUd/fox \
//          fox_wrap_test.cpp <fox TUs> -o fox_wrap_test && ./fox_wrap_test
// ============================================================================

#include "core/types.hpp"
#include "text/font.hpp"
#include "text/soft_wrap.hpp"

#include <cstdio>
#include <string>
#include <vector>

using namespace fox;

namespace
{
    int g_fail = 0;
    void check(bool ok, const char *what)
    {
        std::printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
        if (!ok) ++g_fail;
    }

    FontId g_font = 0;
    f32 g_lh = 0.0f;

    /// 把一行按 avail_w 换行，返回全部行段。
    std::vector<WrapSpan> wrap_all(const std::string &line, f32 avail_w)
    {
        std::vector<WrapSpan> spans(line.size() + 1);
        const int n = wrap_line(g_font, line, 0, line.size(), avail_w, spans.data(),
                                (int)spans.size());
        spans.resize((size_t)(n > 0 ? n : 0));
        return spans;
    }

    /// 结构不变量：连续、覆盖、无丢失。
    bool invariants_ok(const std::string &line, const std::vector<WrapSpan> &rows)
    {
        if (rows.empty()) return false;
        if (rows.front().begin != 0) return false;
        if (rows.back().end != line.size()) return false;
        for (size_t i = 0; i < rows.size(); ++i)
        {
            if (rows[i].end <= rows[i].begin && rows[i].end != rows[i].begin) return false;
            if (i > 0 && rows[i].begin != rows[i - 1].end) return false; // 无缝
        }
        // 拼回去必须一个字节不丢
        std::string joined;
        for (const WrapSpan &r : rows) joined.append(line, r.begin, r.end - r.begin);
        return joined == line;
    }

    /// 行段像素宽（用于「不超宽」断言）。
    f32 span_width(const std::string &line, const WrapSpan &r)
    {
        return text_width_n(g_font, line.data() + r.begin, r.end - r.begin);
    }

} // namespace

int main()
{
    // 真实字体：换行的每一个决策都过字体度量，用假度量等于测另一个算法。
    g_font = fonts().load_system_ui(13.0f);
    if (!g_font || !fonts().face(g_font))
    {
        std::fprintf(stderr, "FOX-WRAP: FAIL - no system font available; this test needs real "
                             "glyph metrics to mean anything\n");
        return 1;
    }
    g_lh = fonts().line_height(g_font);

    // ── [1] 短行不换行 ────────────────────────────────────────────────
    std::printf("[1] 短行：一个视觉行\n");
    {
        const std::string line = "hello world";
        const f32 w = text_width(g_font, line.c_str()) * 4.0f; // 4 倍宽，绝不断
        const auto rows = wrap_all(line, w);
        check(rows.size() == 1, "短行 → 1 个视觉行");
        check(invariants_ok(line, rows), "结构不变量成立");
        check(rows.size() == 1 && rows[0].begin == 0 && rows[0].end == line.size(),
              "唯一行段覆盖整行");
    }

    // ── [2] 长行必断，且不丢字节 ──────────────────────────────────────
    std::printf("[2] 长行：多视觉行，无丢失\n");
    {
        // 构造一条确定超过 3 倍行宽的行
        std::string line = "The quick brown fox jumps over the lazy dog. ";
        while (text_width(g_font, line.c_str()) < text_width(g_font, "MMMM") * 12.0f)
            line += "The quick brown fox jumps over the lazy dog. ";
        const f32 avail = text_width(g_font, "MMMMMM") * 2.0f; // 约 12 字符宽
        const auto rows = wrap_all(line, avail);
        check(rows.size() >= 3, "长行断成 ≥3 个视觉行");
        check(invariants_ok(line, rows), "连续 + 覆盖 + 无丢失");
        bool within = true;
        for (const WrapSpan &r : rows)
            if (span_width(line, r) > avail + 0.5f) within = false;
        check(within, "每个行段不超宽（词优先换行的结果）");
    }

    // ── [3] 词优先：断在空格之后 ──────────────────────────────────────
    std::printf("[3] 词优先换行\n");
    {
        // 用可预测的词序列：每个词同宽（"aaaa " 重复）
        const std::string word = "aaaa ";
        std::string line;
        while (text_width(g_font, line.c_str()) < text_width(g_font, "MMMMMMMMMM"))
            line += word;
        // 行宽 ≈ 4 个词：应断在某个空格之后
        const f32 avail = text_width(g_font, line.c_str()) / 3.0f;
        const auto rows = wrap_all(line, avail);
        check(rows.size() >= 2, "断成多行");
        bool breaks_after_space = true;
        for (size_t i = 1; i < rows.size(); ++i)
        {
            // 上一行的最后一个字节必须是空格（词优先：空格留在行尾）
            if (line[rows[i].begin - 1] != ' ') breaks_after_space = false;
            // 下一行不以空格开头
            if (line[rows[i].begin] == ' ') breaks_after_space = false;
        }
        check(breaks_after_space, "每个断点都在空格之后（词优先，非硬断）");
    }

    // ── [4] CJK 无空格：按码点硬断，不留空行 ──────────────────────────
    std::printf("[4] CJK 硬断\n");
    {
        // 每个汉字 3 字节；字体里应有 CJK 字形（load_system_ui 优先 CJK 字体）
        const std::string han = "量子计算";
        std::string line;
        while (line.size() < 60) line += han;
        const f32 avail = text_width(g_font, line.c_str()) / 4.0f;
        const auto rows = wrap_all(line, avail);
        check(rows.size() >= 2, "CJK 长行断成多行");
        check(invariants_ok(line, rows), "CJK 行段连续无丢失");
        // 硬断按码点边界：每个行段长应为 3 的倍数（不劈开汉字）
        bool on_boundary = true;
        for (const WrapSpan &r : rows)
            if ((r.end - r.begin) % 3 != 0) on_boundary = false;
        check(on_boundary, "硬断不劈开多字节码点（行段长是 3 的倍数）");
        bool no_empty = true;
        for (const WrapSpan &r : rows)
            if (r.end == r.begin) no_empty = false;
        check(no_empty, "没有空行段");
    }

    // ── [5] 单个超宽码点：独占一行并溢出（不丢字）─────────────────────
    std::printf("[5] 超宽单码点\n");
    {
        const std::string line = "ab";
        const f32 avail = text_width_n(g_font, line.data(), 1) * 0.5f; // 半个字符宽
        const auto rows = wrap_all(line, avail);
        check(rows.size() == 2, "两个字符各占一行（都放不下）");
        check(invariants_ok(line, rows), "字符一个都没丢");
        bool overflow_ok = true;
        for (const WrapSpan &r : rows)
            if (span_width(line, r) <= avail) overflow_ok = false; // 确实溢出了
        check(overflow_ok, "行段宽度超过 avail —— 合法溢出（好过丢字）");
    }

    // ── [6] 退化宽度：一个行段（光标仍可达）────────────────────────────
    std::printf("[6] 退化宽度\n");
    {
        const std::string line = "anything at all";
        const auto rows = wrap_all(line, 0.0f);
        check(rows.size() == 1 && rows[0].begin == 0 && rows[0].end == line.size(),
              "avail_w <= 0 → 整行一个行段（由调用方裁剪）");
        const auto rows_neg = wrap_all(line, -100.0f);
        check(rows_neg.size() == 1, "负宽度同样退化而不是零行");
    }

    // ── [7] 空行与纯空格行 ────────────────────────────────────────────
    std::printf("[7] 空行 / 纯空格\n");
    {
        const std::string empty;
        check(wrap_all(empty, 200.0f).empty(), "空行 → 0 个行段（无内容可放）");
        const std::string spaces = "     ";
        const auto rows = wrap_all(spaces, 1.0f); // 一个空格都放不下
        check(!rows.empty(), "纯空格行至少有一个行段（光标要能落在上面）");
        check(invariants_ok(spaces, rows), "空格行结构不变量成立");
    }

    // ── [8] 指纹：变则不同，不变则相同 ─────────────────────────────────
    std::printf("[8] 文本指纹\n");
    {
        const std::string a = "same text";
        const std::string b = "same text";
        const std::string c = "same texU";
        check(text_fingerprint(a) == text_fingerprint(b), "同文同指纹");
        check(text_fingerprint(a) != text_fingerprint(c), "差一字节指纹不同");
        check(text_fingerprint(std::string()) == text_fingerprint(std::string()), "空文有确定指纹");
    }

    // ── [9] 幂等：同输入同输出（缓存键的基石）─────────────────────────
    std::printf("[9] 确定性\n");
    {
        std::string line = "determinism matters for a cache key. ";
        while (line.size() < 100) line += "determinism matters for a cache key. ";
        const f32 avail = text_width(g_font, line.c_str()) / 5.0f;
        const auto r1 = wrap_all(line, avail);
        const auto r2 = wrap_all(line, avail);
        bool same = r1.size() == r2.size();
        for (size_t i = 0; same && i < r1.size(); ++i)
            same = r1[i].begin == r2[i].begin && r1[i].end == r2[i].end;
        check(same, "同一输入两次换行结果逐字节相同");
    }

    std::printf("\n%s（失败 %d 项）\n", g_fail == 0 ? "FOX-WRAP: 全部通过" : "FOX-WRAP: 失败", g_fail);
    return g_fail == 0 ? 0 : 1;
}
