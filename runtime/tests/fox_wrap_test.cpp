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

    std::vector<WrapSpan> wrap_all(const std::string &line, f32 avail_w)
    {
        std::vector<WrapSpan> spans(line.size() + 1);
        const int n = wrap_line(g_font, line, 0, line.size(), avail_w, spans.data(),
                                (int)spans.size());
        spans.resize((size_t)(n > 0 ? n : 0));
        return spans;
    }

    bool invariants_ok(const std::string &line, const std::vector<WrapSpan> &rows)
    {
        if (rows.empty()) return false;
        if (rows.front().begin != 0) return false;
        if (rows.back().end != line.size()) return false;
        for (size_t i = 0; i < rows.size(); ++i)
        {
            if (rows[i].end <= rows[i].begin && rows[i].end != rows[i].begin) return false;
            if (i > 0 && rows[i].begin != rows[i - 1].end) return false;
        }
        std::string joined;
        for (const WrapSpan &r : rows) joined.append(line, r.begin, r.end - r.begin);
        return joined == line;
    }

    f32 span_width(const std::string &line, const WrapSpan &r)
    {
        return text_width_n(g_font, line.data() + r.begin, r.end - r.begin);
    }

}

int main()
{
    g_font = fonts().load_system_ui(13.0f);
    if (!g_font || !fonts().face(g_font))
    {
        std::fprintf(stderr, "FOX-WRAP: FAIL - no system font available; this test needs real "
                             "glyph metrics to mean anything\n");
        return 1;
    }
    g_lh = fonts().line_height(g_font);

    std::printf("[1] 短行：一个视觉行\n");
    {
        const std::string line = "hello world";
        const f32 w = text_width(g_font, line.c_str()) * 4.0f;
        const auto rows = wrap_all(line, w);
        check(rows.size() == 1, "短行 → 1 个视觉行");
        check(invariants_ok(line, rows), "结构不变量成立");
        check(rows.size() == 1 && rows[0].begin == 0 && rows[0].end == line.size(),
              "唯一行段覆盖整行");
    }

    std::printf("[2] 长行：多视觉行，无丢失\n");
    {
        std::string line = "The quick brown fox jumps over the lazy dog. ";
        while (text_width(g_font, line.c_str()) < text_width(g_font, "MMMM") * 12.0f)
            line += "The quick brown fox jumps over the lazy dog. ";
        const f32 avail = text_width(g_font, "MMMMMM") * 2.0f;
        const auto rows = wrap_all(line, avail);
        check(rows.size() >= 3, "长行断成 ≥3 个视觉行");
        check(invariants_ok(line, rows), "连续 + 覆盖 + 无丢失");
        bool within = true;
        for (const WrapSpan &r : rows)
            if (span_width(line, r) > avail + 0.5f) within = false;
        check(within, "每个行段不超宽（词优先换行的结果）");
    }

    std::printf("[3] 词优先换行\n");
    {
        const std::string word = "aaaa ";
        std::string line;
        while (text_width(g_font, line.c_str()) < text_width(g_font, "MMMMMMMMMM"))
            line += word;
        const f32 avail = text_width(g_font, line.c_str()) / 3.0f;
        const auto rows = wrap_all(line, avail);
        check(rows.size() >= 2, "断成多行");
        bool breaks_after_space = true;
        for (size_t i = 1; i < rows.size(); ++i)
        {
            if (line[rows[i].begin - 1] != ' ') breaks_after_space = false;
            if (line[rows[i].begin] == ' ') breaks_after_space = false;
        }
        check(breaks_after_space, "每个断点都在空格之后（词优先，非硬断）");
    }

    std::printf("[4] CJK 硬断\n");
    {
        const std::string han = "量子计算";
        std::string line;
        while (line.size() < 60) line += han;
        const f32 avail = text_width(g_font, line.c_str()) / 4.0f;
        const auto rows = wrap_all(line, avail);
        check(rows.size() >= 2, "CJK 长行断成多行");
        check(invariants_ok(line, rows), "CJK 行段连续无丢失");
        bool on_boundary = true;
        for (const WrapSpan &r : rows)
            if ((r.end - r.begin) % 3 != 0) on_boundary = false;
        check(on_boundary, "硬断不劈开多字节码点（行段长是 3 的倍数）");
        bool no_empty = true;
        for (const WrapSpan &r : rows)
            if (r.end == r.begin) no_empty = false;
        check(no_empty, "没有空行段");
    }

    std::printf("[5] 超宽单码点\n");
    {
        const std::string line = "ab";
        const f32 avail = text_width_n(g_font, line.data(), 1) * 0.5f;
        const auto rows = wrap_all(line, avail);
        check(rows.size() == 2, "两个字符各占一行（都放不下）");
        check(invariants_ok(line, rows), "字符一个都没丢");
        bool overflow_ok = true;
        for (const WrapSpan &r : rows)
            if (span_width(line, r) <= avail) overflow_ok = false;
        check(overflow_ok, "行段宽度超过 avail —— 合法溢出（好过丢字）");
    }

    std::printf("[6] 退化宽度\n");
    {
        const std::string line = "anything at all";
        const auto rows = wrap_all(line, 0.0f);
        check(rows.size() == 1 && rows[0].begin == 0 && rows[0].end == line.size(),
              "avail_w <= 0 → 整行一个行段（由调用方裁剪）");
        const auto rows_neg = wrap_all(line, -100.0f);
        check(rows_neg.size() == 1, "负宽度同样退化而不是零行");
    }

    std::printf("[7] 空行 / 纯空格\n");
    {
        const std::string empty;
        check(wrap_all(empty, 200.0f).empty(), "空行 → 0 个行段（无内容可放）");
        const std::string spaces = "     ";
        const auto rows = wrap_all(spaces, 1.0f);
        check(!rows.empty(), "纯空格行至少有一个行段（光标要能落在上面）");
        check(invariants_ok(spaces, rows), "空格行结构不变量成立");
    }

    std::printf("[8] 文本指纹\n");
    {
        const std::string a = "same text";
        const std::string b = "same text";
        const std::string c = "same texU";
        check(text_fingerprint(a) == text_fingerprint(b), "同文同指纹");
        check(text_fingerprint(a) != text_fingerprint(c), "差一字节指纹不同");
        check(text_fingerprint(std::string()) == text_fingerprint(std::string()), "空文有确定指纹");
    }

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