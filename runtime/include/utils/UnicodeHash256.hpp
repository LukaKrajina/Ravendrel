#pragma once
#include <array>
#include <cstdint>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <vector>
#include "../../../vendor/pinyin/unihan_pinyin.hpp"
#include "../qchain/primitives/hash.hpp"

namespace qhal
{

    inline std::vector<uint32_t> utf8_decode(const std::string &s)
    {
        std::vector<uint32_t> cps;
        cps.reserve(s.size());
        size_t i = 0;
        while (i < s.size())
        {
            unsigned char c = static_cast<unsigned char>(s[i]);
            uint32_t cp = 0;
            int len = 1;
            if (c < 0x80)
            {
                cp = c;
            }
            else if ((c >> 5) == 0x06)
            {
                cp = c & 0x1F;
                len = 2;
            }
            else if ((c >> 4) == 0x0E)
            {
                cp = c & 0x0F;
                len = 3;
            }
            else if ((c >> 3) == 0x1E)
            {
                cp = c & 0x07;
                len = 4;
            }
            else
            {
                cp = c;
            }
            for (int k = 1; k < len && i + k < s.size(); ++k)
                cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3F);
            cps.push_back(cp);
            i += static_cast<size_t>(len);
        }
        return cps;
    }

    inline const char *pinyin_lookup(uint32_t cp)
    {
        using namespace qhal::pinyin;
        size_t lo = 0, hi = kTableSize;
        while (lo < hi)
        {
            size_t mid = (lo + hi) / 2;
            if (kTable[mid].codepoint < cp)
                lo = mid + 1;
            else
                hi = mid;
        }
        if (lo < kTableSize && kTable[lo].codepoint == cp)
            return kTable[lo].pinyin;
        return nullptr;
    }

    inline const std::unordered_map<uint32_t, std::string> &symbol_table()
    {
        static const std::unordered_map<uint32_t, std::string> table = {
            {0x2665, "heart"}, {0x2605, "star"}, {0x2606, "star"}, {0x262F, "taiji"},
            {0x221E, "infinity"}, {0x2192, "arrow"}, {0x2190, "arrow"}, {0x2191, "up"},
            {0x2193, "down"}, {0x2295, "plus"}, {0x2297, "times"}, {0x03C0, "pi"},
            {0x221A, "sqrt"}, {0x2211, "sum"}, {0x220F, "product"}, {0x222B, "integral"},
            {0x2202, "partial"}, {0x03A9, "omega"}, {0x03B1, "alpha"}, {0x03B2, "beta"},
            {0x03B3, "gamma"}, {0x03BB, "lambda"}, {0x03C8, "psi"}, {0x2207, "nabla"},
            {0xFF0C, "comma"}, {0x3002, "period"}, {0xFF01, "exclaim"}, {0xFF1F, "question"},
            {0x3001, "comma"}, {0xFF1B, "semicolon"}, {0xFF1A, "colon"},
            {0x201C, "quote"}, {0x201D, "quote"}, {0x2018, "quote"}, {0x2019, "quote"},
            {0xFF08, "paren"}, {0xFF09, "paren"}, {0x3010, "bracket"}, {0x3011, "bracket"},
            {0x300A, "book"}, {0x300B, "book"}, {0x2014, "dash"}, {0x2026, "ellipsis"},
        };
        return table;
    }

    inline std::string transliterate_char(uint32_t cp)
    {
        if (cp < 0x80)
        {
            if (cp == ' ')
                return "_";
            return std::string(1, static_cast<char>(cp));
        }
        const char *py = pinyin_lookup(cp);
        if (py)
            return std::string(py);
        const auto &st = symbol_table();
        auto it = st.find(cp);
        if (it != st.end())
            return it->second;
        char buf[16];
        std::snprintf(buf, sizeof(buf), "u%x", cp);
        return std::string(buf);
    }

    inline std::string transliterate(const std::string &utf8)
    {
        std::string out;
        out.reserve(utf8.size() * 2);
        for (uint32_t cp : utf8_decode(utf8))
            out += transliterate_char(cp);
        return out;
    }

    inline std::array<uint8_t, 32> unicode_hash256(const std::string &utf8)
    {
        std::string ascii = transliterate(utf8);
        return qchain::crypto::sha256(ascii.data(), ascii.size());
    }
}