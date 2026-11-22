#include "qhal/BroSyntax.hpp"
#include "qhal/BroStr.hpp"

#include <cstring>
#include <string>
#include <vector>

namespace
{
    std::string g_src;
    int32_t g_version = 0;

    std::vector<size_t> g_line_begin;
    std::string g_class;

    bool g_dirty = true;

    void rebuild();
    void classify();

    const char *const kKeywords[] = {
        "int", "char", "long", "short", "float", "double", "unsigned", "signed",
        "static", "sizeof", "goto", "typedef", "union", "auto", "register", "volatile",
        "inline", "bool", "true", "false", "null", "NULL", "nullptr", "void",
        "if", "else", "for", "while", "do", "switch", "case", "default", "break", "continue",
        "return", "function", "const", "let", "var", "class", "extends", "implements", "new",
        "this", "super", "import", "export", "from", "as", "async", "await", "yield", "try",
        "catch", "finally", "throw", "typeof", "instanceof", "in", "of", "delete",
        "interface", "type", "enum", "struct", "template", "namespace", "public", "private",
        "impl", "fn", "pub", "mut", "match", "use", "package", "func", "range", "chan", "go",
        "def", "elif", "lambda", "pass", "with", "assert", "raise", "except", "print", "None",
        "True", "False", "self", "not", "and", "or", "is",
        "SELECT", "FROM", "WHERE", "INSERT", "UPDATE", "DELETE", "JOIN", "GROUP", "ORDER", "BY",
        "LIMIT", "AS", "AND", "OR", "NOT", "NULL", "INTO", "VALUES", "SET", "CREATE", "TABLE",
        nullptr};

    bool is_keyword(const char *s, size_t n)
    {
        for (int i = 0; kKeywords[i]; ++i)
        {
            const size_t klen = std::strlen(kKeywords[i]);
            if (klen != n) continue;
            bool same = true;
            for (size_t j = 0; j < n; ++j)
                if (s[j] != kKeywords[i][j]) { same = false; break; }
            if (same) return true;
        }
        return false;
    }

    bool id_start(char c)
    {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '$' ||
               (unsigned char)c >= 0x80;
    }

    bool id_body(char c)
    {
        return id_start(c) || (c >= '0' && c <= '9');
    }

    bool digit(char c) { return c >= '0' && c <= '9'; }

    bool ensure()
    {
        if (!g_dirty && g_version == gh_ed_version()) return true;

        g_src.clear();
        const char *p = gh_ed_cstr();
        if (p) g_src.assign(p);
        g_version = gh_ed_version();
        g_dirty = false;
        rebuild();
        return true;
    }

    void rebuild()
    {
        g_line_begin.clear();
        g_line_begin.push_back(0);
        for (size_t i = 0; i < g_src.size(); ++i)
        {
            if (g_src[i] != '\n') continue;
            if (i + 1 < g_src.size()) g_line_begin.push_back(i + 1);
        }
        classify();
    }

    void classify()
    {
        g_class.assign(g_src.size(), 'p');
        size_t i = 0;
        const size_t n = g_src.size();

        bool in_block = false;
        bool in_triple = false;

        while (i < n)
        {
            const char c = g_src[i];

            if (in_block)
            {
                g_class[i] = 'c';
                if (c == '*' && i + 1 < n && g_src[i + 1] == '/')
                {
                    g_class[i + 1] = 'c';
                    in_block = false;
                    i += 2;
                    continue;
                }
                ++i;
                continue;
            }
            if (in_triple)
            {
                g_class[i] = 'c';
                if (c == '"' && i + 2 < n && g_src[i + 1] == '"' && g_src[i + 2] == '"')
                {
                    g_class[i + 1] = 'c';
                    g_class[i + 2] = 'c';
                    in_triple = false;
                    i += 3;
                    continue;
                }
                ++i;
                continue;
            }

            if (c == '/' && i + 1 < n && g_src[i + 1] == '/')
            {
                while (i < n && g_src[i] != '\n') g_class[i++] = 'c';
                continue;
            }
            if (c == '#')
            {
                while (i < n && g_src[i] != '\n') g_class[i++] = 'c';
                continue;
            }
            if (c == '-' && i + 1 < n && g_src[i + 1] == '-')
            {
                while (i < n && g_src[i] != '\n') g_class[i++] = 'c';
                continue;
            }

            if (c == '"' || c == '\'' || c == '`')
            {
                const char q = c;
                if (q == '"' && i + 2 < n && g_src[i + 1] == '"' && g_src[i + 2] == '"')
                {
                    in_triple = true;
                    i += 3;
                    g_class[i - 3] = g_class[i - 2] = g_class[i - 1] = 's';
                    continue;
                }
                g_class[i++] = 's';
                while (i < n)
                {
                    const char d = g_src[i];
                    if (d == '\\' && i + 1 < n)
                    {
                        g_class[i] = 's';
                        g_class[i + 1] = 's';
                        i += 2;
                        continue;
                    }
                    if (d == '\n') break;
                    g_class[i++] = 's';
                    if (d == q) break;
                }
                continue;
            }

            if (c == '#' || (c == '@' && i + 1 < n && g_src[i + 1] == 'i'))
            {
                size_t j = i;
                while (j < n && g_src[j] != '\n' && g_src[j] != ' ') g_class[j++] = 'r';
                i = j;
                continue;
            }

            if (digit(c) || (c == '.' && i + 1 < n && digit(g_src[i + 1])))
            {
                size_t j = i;
                while (j < n && (id_body(g_src[j]) || g_src[j] == '.')) ++j;
                for (size_t k = i; k < j; ++k) g_class[k] = 'n';
                i = j;
                continue;
            }

            if (id_start(c))
            {
                size_t j = i;
                while (j < n && id_body(g_src[j])) ++j;
                const size_t len = j - i;

                bool upper = (c >= 'A' && c <= 'Z');
                if (upper)
                {
                    for (size_t k = i; k < j; ++k) g_class[k] = 't';
                }
                else if (is_keyword(g_src.data() + i, len))
                {
                    for (size_t k = i; k < j; ++k) g_class[k] = 'k';
                }
                else if (j < n && g_src[j] == '(')
                {
                    for (size_t k = i; k < j; ++k) g_class[k] = 'f';
                }
                else if (g_class[i] != 'c' && g_class[i] != 's')
                {
                    for (size_t k = i; k < j; ++k) g_class[k] = 'p';
                }
                i = j;
                continue;
            }

            if (c != ' ' && c != '\n' && c != '\t' && c != '\r') g_class[i] = 'o';
            ++i;
        }

        for (size_t k = 0; k < n; ++k)
        {
            const char c = g_src[k];
            if (g_class[k] == 'p' && (c == ' ' || c == '\t' || c == '\r'))
                g_class[k] = ' ';
        }
    }

    int32_t count_codepoints(size_t b, size_t e)
    {
        if (e > g_src.size()) e = g_src.size();
        int32_t n = 0;
        for (size_t i = b; i < e; ++i)
            if ((static_cast<unsigned char>(g_src[i]) & 0xC0) != 0x80) n++;
        return n;
    }

    size_t line_start(size_t line)
    {
        if (line >= g_line_begin.size()) return g_src.size();
        return g_line_begin[line];
    }

    size_t line_end(size_t line)
    {
        if (line + 1 < g_line_begin.size()) return g_line_begin[line + 1] - 1;
        return g_src.size();
    }

    bool line_valid(int32_t line)
    {
        return line >= 0 && (size_t)line < g_line_begin.size();
    }

    const char *line_class(size_t line)
    {
        static std::string out;
        if (!line_valid((int32_t)line)) return "";
        const size_t b = line_start(line);
        const size_t e = line_end(line);
        out.assign(g_class, b, e - b);
        return out.c_str();
    }
}

extern "C"
{


void gh_syn_invalidate(void)
{
    g_dirty = true;
}


int32_t gh_syn_line_count(void)
{
    ensure();
    return (int32_t)g_line_begin.size();
}

int32_t gh_syn_line_begin(int32_t line)
{
    ensure();
    if (!line_valid(line)) return -1;
    return (int32_t)line_start((size_t)line);
}

int32_t gh_syn_line_end(int32_t line)
{
    ensure();
    if (!line_valid(line)) return -1;
    return (int32_t)line_end((size_t)line);
}

int32_t gh_syn_line_len(int32_t line)
{
    ensure();
    if (!line_valid(line)) return 0;
    const size_t l = (size_t)line;
    return (int32_t)(line_end(l) - line_start(l));
}


const char *gh_syn_line(int32_t line)
{
    ensure();
    if (line < 0) return "";
    return line_class((size_t)line);
}

int32_t gh_syn_class_of(int32_t line, int32_t col)
{
    ensure();
    if (!line_valid(line)) return 0;
    const size_t l = (size_t)line;
    if (col < 0 || (size_t)col >= line_end(l) - line_start(l)) return 0;
    return (int32_t)g_class[line_start(l) + col];
}

const char *gh_syn_slice(int32_t line, int32_t b0, int32_t b1)
{
    ensure();
    static std::string out;
    out.clear();
    if (!line_valid(line)) return out.c_str();
    const size_t lb = line_start((size_t)line);
    const size_t len = line_end((size_t)line) - lb;
    if (b0 < 0) b0 = 0;
    if (b1 > (int32_t)len) b1 = (int32_t)len;
    if (b1 <= b0) return out.c_str();
    out.assign(g_src, lb + (size_t)b0, (size_t)(b1 - b0));
    return out.c_str();
}

int32_t gh_syn_cp_index(int32_t line, int32_t byte_col)
{
    ensure();
    if (!line_valid(line)) return -1;
    const size_t l = (size_t)line;
    size_t target = line_start(l) + (size_t)(byte_col > 0 ? byte_col : 0);
    const size_t le = line_end(l);
    if (target > le) target = le;
    return count_codepoints(0, target);
}

int32_t gh_syn_cp_line(int32_t cp_index)
{
    ensure();
    int32_t n = 0;
    const size_t lines = g_line_begin.size();
    for (size_t l = 0; l < lines; ++l)
    {
        const int32_t cnt = count_codepoints(line_start(l), line_end(l));
        if (cp_index < n + cnt) return (int32_t)l;
        n += cnt;
    }
    return cp_index == n && lines > 0 ? (int32_t)lines - 1 : -1;
}

int32_t gh_syn_cp_col(int32_t cp_index)
{
    ensure();
    const int32_t line = gh_syn_cp_line(cp_index);
    if (line < 0) return -1;
    int32_t before = 0;
    for (int32_t l = 0; l < line; ++l)
        before += count_codepoints(line_start((size_t)l), line_end((size_t)l));
    const int32_t k = cp_index - before;
    if (k < 0) return 0;

    const size_t b = line_start((size_t)line);
    const size_t e = line_end((size_t)line);
    size_t i = b;
    int32_t seen = 0;
    while (i < e && seen < k)
    {
        ++i;
        while (i < e && (static_cast<unsigned char>(g_src[i]) & 0xC0) == 0x80) i++;
        seen++;
    }
    return (int32_t)(i - b);
}


int32_t gh_syn_bytes(void)
{
    ensure();
    return (int32_t)g_src.size();
}

int32_t gh_syn_language(void)
{
    ensure();
    if (g_src.size() >= 2 && g_src[0] == '#' && g_src[1] == '!') return 1;
    return 0;
}

}