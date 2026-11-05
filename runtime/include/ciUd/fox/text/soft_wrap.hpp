#pragma once
// Fox text/soft_wrap.hpp - 多行文字的视觉行折行
//
// 软折行是布局的属性，不是编辑模型的：TextBuffer 只把 '\n' 当行分隔符
// （为什么这样分开比功能本身更值，见 text_buffer.hpp 的 "lines" 一节），
// 本头文件回答布局需要的那一个问题——"这条逻辑行在哪里断成视觉行"。
//
// 算法优先按词断行，断不开再硬切：
//   · 在行内放得下的最后一个空格/制表符**之后**断（按词折行）；
//   · 一个都放不下（CJK 没有空格）时，在会溢出的码点**之前**断——CJK
//     一行哪儿都能断，这是它的标准行为；
//   · 单个码点比整行还宽时独占一行并溢出，因为丢掉它就丢了文字。
//
// O(行长)：每个码点一次 text_width_n，不重复测量。断点处的宽度记账是
// 增量的（w_at_break），这才让一万字符的行不会变成平方复杂度。

#include "core/types.hpp"
#include "font.hpp"

#include <string>

namespace fox
{

    /// 一个视觉行：逻辑行内的一个字节区间。
    struct WrapSpan
    {
        size_t begin; ///< 该行第一个字节
        size_t end;   ///< 最后一个字节的下一个（不含 '\n'）
    };

    /// 对文本做 FNV-1a：折行缓存的变更探测器。O(n)、约 1 ns/字节，
    /// 比它守护的字形测量便宜两个数量级。
    inline u64 text_fingerprint(const std::string &s)
    {
        u64 h = 0xCBF29CE484222325ull;
        for (unsigned char ch : s)
        {
            h ^= ch;
            h *= 0x100000001B3ull;
        }
        return h;
    }

    /// `i` 处 UTF-8 序列的码点长度，钳到 `le`。区间末尾的截断序列按剩余
    /// 字节计，不越出 `le`。
    inline size_t utf8_len_at(const std::string &s, size_t i, size_t le)
    {
        const unsigned char c0 = (unsigned char)s[i];
        size_t cl = 1;
        if (c0 >= 0xF0) cl = 4;
        else if (c0 >= 0xE0) cl = 3;
        else if (c0 >= 0xC0) cl = 2;
        if (i + cl > le) cl = le - i;
        return cl ? cl : 1;
    }

    /// 把一条逻辑行 [ls, le) 折成宽度不超过 `avail_w` 像素的视觉行。
    /// 最多写 `max_out` 个跨度，返回写了多少。
    ///
    /// 只有空区间（le <= ls）才返回 0——一行不占任何视觉行意味着光标
    /// 无处可放，而调用方拿结果当下标访问 rows[]，真实内容绝不能出现
    /// 这种情况。`avail_w <= 0` 退化为覆盖整行的单行（由调用方裁剪）；
    /// 只有这种形状能保证光标可达。
    inline int wrap_line(FontId f, const std::string &s, size_t ls, size_t le, f32 avail_w,
                         WrapSpan *out, int max_out)
    {
        if (le <= ls || max_out <= 0) return 0;
        if (avail_w <= 0.0f)
        {
            out[0].begin = ls;
            out[0].end = le;
            return 1;
        }

        int rows = 0;
        size_t row_start = ls;
        f32 w = 0.0f;            ///< [row_start, i) 的宽度
        bool have_break = false; ///< 本行内有一个放得下的断点
        size_t break_pos = 0;    ///< 下一行从这里开始（空格之后）
        f32 w_at_break = 0.0f;   ///< [row_start, break_pos) 的宽度

        size_t i = ls;
        while (i < le)
        {
            const size_t cl = utf8_len_at(s, i, le);
            const f32 cw = text_width_n(f, s.data() + i, cl);
            const char ch = s[i];

            if (w + cw > avail_w && i > row_start)
            {
                // 断行：优先用行内最后一个断点，否则在这个码点前硬切。
                // 两种情况下溢出的码点都属于新行，它的宽度由下面的算术
                // 结转——所以 `continue`，不要再加一次 cw。
                size_t next_start = i;
                f32 new_w = cw;
                if (have_break)
                {
                    next_start = break_pos;
                    new_w = (w + cw) - w_at_break;
                }
                if (rows < max_out)
                {
                    out[rows].begin = row_start;
                    out[rows].end = next_start;
                    ++rows;
                }
                row_start = next_start;
                w = new_w;
                have_break = false;
                if (ch == ' ' || ch == '\t' || ch == '\r')
                {
                    // 溢出的断行字符开启下一行，它本身又是再下一行的断点。
                    have_break = true;
                    break_pos = i + cl;
                    w_at_break = w;
                }
                i += cl;
                continue;
            }

            w += cw;
            if (ch == ' ' || ch == '\t' || ch == '\r')
            {
                have_break = true;
                break_pos = i + cl; ///< 断在空格之后
                w_at_break = w;
            }
            i += cl;
        }

        if (rows < max_out)
        {
            out[rows].begin = row_start;
            out[rows].end = le;
            ++rows;
        }
        return rows;
    }

} // namespace fox
