// ═══════════════════════════════════════════════════════════════════════
// Bro 代码着色 + 行索引
// ═══════════════════════════════════════════════════════════════════════
//
// 为什么放在 C++ 而不是 rvdl：
//
// 界面上只需要画**可见的**那几十行，看起来在 rvdl 里逐码点扫也行。
// 但「找第 N 行的起点」要从缓冲开头扫一遍 —— 一个 500 行的文件，
// 每帧 40 行 × 40k 码点 = 160 万次 gh_str_codepoint() 调用。
// 1900fps 下每秒 30 亿次，这是纯浪费。
//
// 所以这里做两件事，都缓存到「缓冲变了才重建」：
//   1. 行索引：行首码点偏移的数组
//   2. 词法分类：每个码点一个类别码（关键字/字符串/注释/数字/类型/函数…）
//
// rvdl 侧于是变成「每可见行一次 FFI + 一次顺序遍历」，
// 复杂度从 O(文件大小 × 可见行数) 降到 O(可见行数 × 行长)。
//
// ── 类别码 ──────────────────────────────────────────────────────────────
// 每一位字符是一个 ASCII 字母，rvdl 侧有 ascii_str() 查表拿它，
// 所以返回值可以直接当字符串用。必须保证没有 \0 和非 ASCII：
// \0 会截断，非 ASCII 会被 UTF-8 解码成多字节，打乱「一码点一字符」的约定。
//
//   p 普通    k 关键字   s 字符串   c 注释   n 数字
//   t 类型    f 函数名   o 运算符/标点   r 预处理指令
//
// ── 这不是完整语法分析器 ────────────────────────────────────────────────
//
// 只做单遍词法扫描，不查符号表、不管作用域、不区分字符串模板。
// 目的是「扫一眼能看出结构」，不是「拿来做格式化或重写」。
// 认不准的按普通文本处理 —— 宁可少染色，不要染错。

#include "qhal/BroSyntax.hpp"
#include "qhal/BroStr.hpp"

#include <cstring>
#include <string>
#include <vector>

namespace
{
    // ── 缓冲快照 ────────────────────────────────────────────────────────
    //
    // 直接读 gh_ed_* 的内部状态是不可行的（那些函数只暴露公开接口），
    // 所以这里通过公开 API 把内容拷一份。拷贝成本由「只在版本变化时」
    // 摊掉：版本号由 gh_ed_version() 给出，宿主改内容就变。
    std::string g_src;
    int32_t g_version = 0;

    // 行首偏移（字节）
    std::vector<size_t> g_line_begin;
    // 类别（每字节一个，仅 ASCII 可见）
    std::string g_class;

    bool g_dirty = true;

    void rebuild();
    void classify();

    // ── 关键字表 ────────────────────────────────────────────────────────
    //
    // 故意只放「几乎所有主流语言都常见」的那批，
    // 少染几个字不会错认，多染才会误导阅读。
    // 但 C / C++ 的那批必须齐：Bro 自己的 rvdl 和 C++ 源码里
    // int / char / static / void 到处都是，漏掉的话代码视图里
    // 满屏都是没色的标识词，一眼看不出类型边界。
    const char *const kKeywords[] = {
        // C / C++ / Java / C#
        "int", "char", "long", "short", "float", "double", "unsigned", "signed",
        "static", "sizeof", "goto", "typedef", "union", "auto", "register", "volatile",
        "inline", "bool", "true", "false", "null", "NULL", "nullptr", "void",
        // 脚本 / Web
        "if", "else", "for", "while", "do", "switch", "case", "default", "break", "continue",
        "return", "function", "const", "let", "var", "class", "extends", "implements", "new",
        "this", "super", "import", "export", "from", "as", "async", "await", "yield", "try",
        "catch", "finally", "throw", "typeof", "instanceof", "in", "of", "delete",
        "interface", "type", "enum", "struct", "template", "namespace", "public", "private",
        // Rust / Go / 其它
        "impl", "fn", "pub", "mut", "match", "use", "package", "func", "range", "chan", "go",
        // Python
        "def", "elif", "lambda", "pass", "with", "assert", "raise", "except", "print", "None",
        "True", "False", "self", "not", "and", "or", "is",
        // SQL
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
               (unsigned char)c >= 0x80; // CJK / 全角也算标识符，代码注释里全是它
    }

    bool id_body(char c)
    {
        return id_start(c) || (c >= '0' && c <= '9');
    }

    bool digit(char c) { return c >= '0' && c <= '9'; }

    /// 重新同步快照。返回值 false 表示缓冲读不到（此时不留旧内容）
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
        // 行索引
        //
        // 约定：**末尾的换行不额外算一行**。
        // "a\nb" 是 2 行，"a\nb\n" 也是 2 行 —— 那个末尾换行只是
        // 第二行的行终止符，不是第三行的开头。
        //
        // 如果按 split('\n') 的直觉实现，"a\nb\n" 会变成 3 行，
        // 编辑器底部就多出一条永远选不中的空行，而且滚动到底
        // 也到不了真正的文件末尾。
        g_line_begin.clear();
        g_line_begin.push_back(0);
        for (size_t i = 0; i < g_src.size(); ++i)
        {
            if (g_src[i] != '\n') continue;
            // 只有后面还有内容才开新行
            if (i + 1 < g_src.size()) g_line_begin.push_back(i + 1);
        }
        // 空文档也保留 1 行：否则编辑器里没有「第 1 行」，
        // 光标无处可放，「空文件」和「还没打开文件」长得一样。
        classify();
    }

    /// 词法分类。单遍扫描，处理字符串与注释的跨行延续。
    void classify()
    {
        g_class.assign(g_src.size(), 'p');
        size_t i = 0;
        const size_t n = g_src.size();

        // 跨行状态：块注释（/* */）和续行字符串（""" ... """）
        bool in_block = false;
        bool in_triple = false;

        while (i < n)
        {
            const char c = g_src[i];

            // ── 块注释 ────────────────────────────────────────────
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

            // ── 行注释 ────────────────────────────────────────────
            // `//`、`#`、`--` 三种都认。多认一点比少认好：
            // 认错了最多把一行染灰，认漏了注释会盖到后面的关键字上。
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

            // ── 字符串 ────────────────────────────────────────────
            if (c == '"' || c == '\'' || c == '`')
            {
                const char q = c;
                // 三引号（Python / SQL 块）跨行
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
                    if (d == '\\' && i + 1 < n)   // 转义：两个字节都是字符串
                    {
                        g_class[i] = 's';
                        g_class[i + 1] = 's';
                        i += 2;
                        continue;
                    }
                    if (d == '\n') break;          // 未闭合：停在本行末，别把整文件吞了
                    g_class[i++] = 's';
                    if (d == q) break;
                }
                continue;
            }

            // ── 预处理指令（行首的 #include / #define）────────────
            if (c == '#' || (c == '@' && i + 1 < n && g_src[i + 1] == 'i'))
            {
                size_t j = i;
                while (j < n && g_src[j] != '\n' && g_src[j] != ' ') g_class[j++] = 'r';
                i = j;
                continue;
            }

            // ── 数字 ──────────────────────────────────────────────
            if (digit(c) || (c == '.' && i + 1 < n && digit(g_src[i + 1])))
            {
                size_t j = i;
                while (j < n && (id_body(g_src[j]) || g_src[j] == '.')) ++j;
                for (size_t k = i; k < j; ++k) g_class[k] = 'n';
                i = j;
                continue;
            }

            // ── 标识符 ────────────────────────────────────────────
            if (id_start(c))
            {
                size_t j = i;
                while (j < n && id_body(g_src[j])) ++j;
                const size_t len = j - i;

                // 大写开头 → 类型（TS 的类型、Python 的类名、常量）
                bool upper = (c >= 'A' && c <= 'Z');
                if (upper)
                {
                    for (size_t k = i; k < j; ++k) g_class[k] = 't';
                }
                else if (is_keyword(g_src.data() + i, len))
                {
                    for (size_t k = i; k < j; ++k) g_class[k] = 'k';
                }
                // 后面紧跟 '(' → 函数名（关键字后面跟括号不算）
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

            // ── 标点 / 运算符 ────────────────────────────────────
            if (c != ' ' && c != '\n' && c != '\t' && c != '\r') g_class[i] = 'o';
            ++i;
        }
    }

    /// [b, e) 区间里有多少个码点。续接字节（0b10xxxxxx）不算。
    int32_t count_codepoints(size_t b, size_t e)
    {
        if (e > g_src.size()) e = g_src.size();
        int32_t n = 0;
        for (size_t i = b; i < e; ++i)
            if ((static_cast<unsigned char>(g_src[i]) & 0xC0) != 0x80) n++;
        return n;
    }

    /// 第 line 行的起始字节偏移。越界返回 g_src.size()（不是越界值）。
    ///
    /// 全部访问器共用这一个函数，而不是各自写 `g_line_begin[line + 1]`。
    /// 原因：那一行在**最后一行**会越界读 —— 而每个访问器都得自己
    /// 判断「要不要夹到末尾」，漏一个就是一个随文件长度触发的错。
    /// 踩过一次：line_len 在两行文件上返回 -1。
    size_t line_start(size_t line)
    {
        if (line >= g_line_begin.size()) return g_src.size();
        return g_line_begin[line];
    }

    /// 第 line 行结束的字节偏移（不含换行符）
    size_t line_end(size_t line)
    {
        if (line + 1 < g_line_begin.size()) return g_line_begin[line + 1] - 1;
        return g_src.size();
    }

    bool line_valid(int32_t line)
    {
        return line >= 0 && (size_t)line < g_line_begin.size();
    }

    /// 返回第 line 行（0 基）的类别串
    const char *line_class(size_t line)
    {
        static std::string out;
        if (!line_valid((int32_t)line)) return "";
        const size_t b = line_start(line);
        const size_t e = line_end(line);
        out.assign(g_class, b, e - b);
        return out.c_str();
    }
} // namespace

extern "C"
{

// ── 失效通知 ────────────────────────────────────────────────────────────

// 宿主在每次修改缓冲后调用。版本号变了才重建索引。
void gh_syn_invalidate(void)
{
    g_dirty = true;
}

// ── 行索引 ──────────────────────────────────────────────────────────────

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

// ── 词法分类 ────────────────────────────────────────────────────────────

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
    // 数一遍前面有多少个非续接字节（续接字节是 0b10xxxxxx）
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
    // 光标可以落在文件末尾（= 最后一个字符之后），那仍算最后一行。
    return cp_index == n && lines > 0 ? (int32_t)lines - 1 : -1;
}

int32_t gh_syn_cp_col(int32_t cp_index)
{
    ensure();
    const int32_t line = gh_syn_cp_line(cp_index);
    if (line < 0) return -1;
    // 先数前面所有行一共多少码点，剩下的就是行内偏移
    int32_t before = 0;
    for (int32_t l = 0; l < line; ++l)
        before += count_codepoints(line_start((size_t)l), line_end((size_t)l));
    const int32_t k = cp_index - before;
    if (k < 0) return 0;

    // 从行首走 k 个字符，返回字节列（一个中文算一整格）
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

// ── 文件信息 ────────────────────────────────────────────────────────────

int32_t gh_syn_bytes(void)
{
    ensure();
    return (int32_t)g_src.size();
}

/// 语言 guess（从首行的 shebang 或文件路径后缀）。rvdl 侧只用来决定配色口径。
int32_t gh_syn_language(void)
{
    ensure();
    if (g_src.size() >= 2 && g_src[0] == '#' && g_src[1] == '!') return 1;   // 脚本
    return 0;                                                            // 通用
}

} // extern "C"
