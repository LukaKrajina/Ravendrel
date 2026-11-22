#pragma once

#include "core/types.hpp"
#include "font.hpp"

#include <string>

namespace fox
{

    struct WrapSpan
    {
        size_t begin;
        size_t end;
    };

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
        f32 w = 0.0f;
        bool have_break = false;
        size_t break_pos = 0;
        f32 w_at_break = 0.0f;

        size_t i = ls;
        while (i < le)
        {
            const size_t cl = utf8_len_at(s, i, le);
            const f32 cw = text_width_n(f, s.data() + i, cl);
            const char ch = s[i];

            if (w + cw > avail_w && i > row_start)
            {
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
                break_pos = i + cl;
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

}