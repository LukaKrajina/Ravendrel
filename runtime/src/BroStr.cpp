#include "qhal/BroStr.hpp"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <filesystem>
#include <chrono>

#ifdef _WIN32
#include <windows.h>
#endif

namespace
{
    void append_utf8(std::string &out, uint32_t cp)
    {
        if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) cp = 0xFFFD;
        if (cp < 0x80)
        {
            out.push_back(static_cast<char>(cp));
        }
        else if (cp < 0x800)
        {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
        else if (cp < 0x10000)
        {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
        else
        {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }

    uint32_t decode_utf8(const char *&p, const char *end)
    {
        const unsigned char *u = reinterpret_cast<const unsigned char *>(p);
        if (u >= reinterpret_cast<const unsigned char *>(end)) return 0;
        const unsigned char b0 = u[0];
        int n = 1;
        uint32_t cp = b0;
        if (b0 >= 0xF0) { n = 4; cp = b0 & 0x07u; }
        else if (b0 >= 0xE0) { n = 3; cp = b0 & 0x0Fu; }
        else if (b0 >= 0xC0) { n = 2; cp = b0 & 0x1Fu; }
        if (p + n > end) n = 1;
        for (int i = 1; i < n; ++i)
        {
            const unsigned char b = u[i];
            if ((b & 0xC0) != 0x80) { n = 1; cp = 0xFFFD; break; }
            cp = (cp << 6) | (b & 0x3Fu);
        }
        p += n;
        if (n == 1 && cp >= 0x80) cp = 0xFFFD;
        return cp;
    }

    struct StrBuf
    {
        std::u32string cp;
        int32_t cursor = 0;
        int32_t anchor = 0;

        std::string utf8;
        std::string sel;

        int32_t version = 0;

        struct Snapshot
        {
            std::u32string cp;
            int32_t cursor;
            int32_t anchor;
        };
        static constexpr size_t kUndoMax = 64;
        std::vector<Snapshot> undo_stack;
        std::vector<Snapshot> redo_stack;

        int32_t tx_depth = 0;
        std::u32string tx_cp;
        int32_t tx_cursor = 0;
        int32_t tx_anchor = 0;

        void tx_begin()
        {
            if (tx_depth++ == 0)
            {
                tx_cp = cp;
                tx_cursor = cursor;
                tx_anchor = anchor;
            }
        }

        int32_t tx_commit()
        {
            if (tx_depth > 0 && --tx_depth == 0)
            {
                if (cp != tx_cp || cursor != tx_cursor || anchor != tx_anchor)
                {
                    push_undo();
                    undo_stack.back() = {tx_cp, tx_cursor, tx_anchor};
                }
            }
            return 1;
        }

        void push_undo()
        {
            if (tx_depth > 0) return;
            undo_stack.push_back({cp, cursor, anchor});
            if (undo_stack.size() > kUndoMax) undo_stack.erase(undo_stack.begin());
            redo_stack.clear();
        }

        void clear_history()
        {
            undo_stack.clear();
            redo_stack.clear();
        }

        std::u32string loaded_cp;
        bool has_loaded = false;
        bool valid() const { return has_loaded; }
        bool dirty() const { return has_loaded && cp != loaded_cp; }
        void mark_saved() { loaded_cp = cp; }
        void mark_loaded()
        {
            has_loaded = true;
            loaded_cp = cp;
        }

        int32_t undo()
        {
            if (undo_stack.empty()) return 0;
            redo_stack.push_back({cp, cursor, anchor});
            const Snapshot s = undo_stack.back();
            undo_stack.pop_back();
            cp = s.cp;
            cursor = s.cursor;
            anchor = s.anchor;
            invalidate();
            return 1;
        }

        int32_t redo()
        {
            if (redo_stack.empty()) return 0;
            undo_stack.push_back({cp, cursor, anchor});
            const Snapshot s = redo_stack.back();
            redo_stack.pop_back();
            cp = s.cp;
            cursor = s.cursor;
            anchor = s.anchor;
            invalidate();
            return 1;
        }

        int32_t size() const { return static_cast<int32_t>(cp.size()); }

        const std::u32string &raw() const { return cp; }










        int32_t replace_selection(const std::u32string &add)
        {
            if (!has_sel()) return 0;
            const int32_t a = sel_lo();
            const int32_t b = sel_hi();
            push_undo();
            cp.erase(cp.begin() + a, cp.begin() + b);
            cp.insert(cp.begin() + a, add.begin(), add.end());
            cursor = a + static_cast<int32_t>(add.size());
            anchor = cursor;
            invalidate();
            return 1;
        }

        bool replace_all(const std::u32string &next)
        {
            if (next == cp) return false;
            push_undo();
            cp = next;
            cursor = size();
            anchor = cursor;
            invalidate();
            return true;
        }

        int32_t clamp(int32_t pos) const
        {
            if (pos < 0) return 0;
            const int32_t n = size();
            return pos > n ? n : pos;
        }

        int32_t sel_lo() const { return anchor < cursor ? anchor : cursor; }
        int32_t sel_hi() const { return anchor < cursor ? cursor : anchor; }
        bool has_sel() const { return anchor != cursor; }

        void invalidate()
        {
            utf8.clear();
            sel.clear();
            ++version;
        }

        const std::string &utf8_of()
        {
            if (utf8.empty() && !cp.empty())
                for (uint32_t c : cp) append_utf8(utf8, c);
            return utf8;
        }

        const std::string &sel_of()
        {
            sel.clear();
            if (!has_sel()) return sel;
            const int32_t a = sel_lo(), b = sel_hi();
            for (int32_t i = a; i < b && i < size(); ++i) append_utf8(sel, cp[i]);
            return sel;
        }

        void reset()
        {
            cp.clear();
            cursor = 0;
            anchor = 0;
            invalidate();
        }

        int32_t add(int32_t c)
        {
            if (c < 0 || c > 0x10FFFF) return size();
            push_undo();
            cp.insert(cp.begin() + cursor, static_cast<char32_t>(c));
            cursor += 1;
            anchor = cursor;
            invalidate();
            return size();
        }

        int32_t backspace()
        {
            if (has_sel()) return delete_selection();
            if (cursor <= 0) return 0;
            push_undo();
            cp.erase(cp.begin() + (cursor - 1));
            cursor -= 1;
            anchor = cursor;
            invalidate();
            return 1;
        }

        int32_t set(const char *u8)
        {
            cp.clear();
            if (u8)
            {
                const char *p = u8;
                const char *end = u8 + std::strlen(u8);
                while (p < end) cp.push_back(static_cast<char32_t>(decode_utf8(p, end)));
            }
            cursor = size();
            anchor = cursor;
            clear_history();
            invalidate();
            return size();
        }

        int32_t append(const char *u8)
        {
            if (!u8) return size();
            const char *p = u8;
            const char *end = u8 + std::strlen(u8);
            std::u32string add;
            while (p < end) add.push_back(static_cast<char32_t>(decode_utf8(p, end)));
            if (add.empty()) return size();
            push_undo();
            cp.insert(cp.begin() + cursor, add.begin(), add.end());
            cursor += static_cast<int32_t>(add.size());
            anchor = cursor;
            invalidate();
            return size();
        }

        int32_t at(int32_t index) const
        {
            if (index < 0 || index >= size()) return -1;
            return static_cast<int32_t>(cp[index]);
        }

        int32_t insert(int32_t index, int32_t c)
        {
            if (c < 0 || c > 0x10FFFF) return 0;
            const int32_t i = clamp(index);
            push_undo();
            cp.insert(cp.begin() + i, static_cast<char32_t>(c));
            cursor = i + 1;
            anchor = cursor;
            invalidate();
            return 1;
        }

        int32_t del(int32_t index)
        {
            if (index < 0 || index >= size()) return 0;
            push_undo();
            cp.erase(cp.begin() + index);
            if (cursor > size()) cursor = size();
            anchor = cursor;
            invalidate();
            return 1;
        }

        int32_t truncate(int32_t count)
        {
            if (count < 0) count = 0;
            if (count > size()) count = size();
            if (count == size()) return count;
            push_undo();
            cp.resize(static_cast<size_t>(count));
            cursor = clamp(cursor);
            anchor = cursor;
            invalidate();
            return count;
        }

        int32_t set_cursor(int32_t pos)
        {
            cursor = clamp(pos);
            anchor = cursor;
            return cursor;
        }

        int32_t select_extend(int32_t pos)
        {
            cursor = clamp(pos);
            return has_sel() ? sel_hi() - sel_lo() : 0;
        }

        int32_t set_anchor(int32_t pos)
        {
            anchor = clamp(pos);
            return anchor;
        }

        int32_t select_clear()
        {
            anchor = cursor;
            return 0;
        }

        int32_t delete_selection()
        {
            if (!has_sel()) return 0;
            const int32_t a = sel_lo(), b = sel_hi();
            push_undo();
            cp.erase(cp.begin() + a, cp.begin() + b);
            cursor = a;
            anchor = a;
            invalidate();
            return 1;
        }

        int32_t insert_str(const char *u8)
        {
            if (!u8) return 0;
            const char *p = u8;
            const char *end = u8 + std::strlen(u8);
            std::u32string add;
            while (p < end) add.push_back(static_cast<char32_t>(decode_utf8(p, end)));
            return insert_raw(add);
        }

        int32_t insert_raw(const std::u32string &add)
        {
            if (add.empty()) return 0;
            if (has_sel()) delete_selection();
            push_undo();
            const int32_t at = cursor;
            cp.insert(cp.begin() + at, add.begin(), add.end());
            cursor = at + static_cast<int32_t>(add.size());
            anchor = cursor;
            invalidate();
            return static_cast<int32_t>(add.size());
        }

        int32_t newline()
        {
            if (has_sel()) delete_selection();
            const int32_t at = cursor;

            int32_t ls = at;
            while (ls > 0 && cp[ls - 1] != U'\n') ls -= 1;

            int32_t le = ls;
            while (le < at && (cp[le] == U' ' || cp[le] == U'\t')) le += 1;

            std::u32string add;
            add.push_back(U'\n');
            add.insert(add.end(), cp.begin() + ls, cp.begin() + le);

            push_undo();
            cp.insert(cp.begin() + at, add.begin(), add.end());
            cursor = at + static_cast<int32_t>(add.size());
            anchor = cursor;
            invalidate();
            return static_cast<int32_t>(add.size());
        }
    };

    StrBuf g_draft;
    StrBuf g_doc;
    StrBuf g_find[2];

    std::string g_doc_path;
    int64_t g_doc_mtime = 0;
    std::string g_save_error;

    std::string norm_path(const std::string &p)
    {
        return p.empty() ? p : std::filesystem::weakly_canonical(p).string();
    }

    int64_t file_mtime(const std::string &p)
    {
        std::error_code ec;
        auto t = std::filesystem::last_write_time(p, ec);
        if (ec) return 0;
        return static_cast<int64_t>(
            std::chrono::duration_cast<std::chrono::seconds>(t.time_since_epoch()).count());
    }
}

extern "C"
{


void gh_str_reset(void) { g_draft.reset(); }
int32_t gh_str_add(int32_t cp) { return g_draft.add(cp); }
int32_t gh_str_add_ascii(int32_t c) { return g_draft.add(c); }
int32_t gh_str_backspace(void) { return g_draft.backspace(); }
int32_t gh_str_set(const char *utf8) { return g_draft.set(utf8); }
int32_t gh_str_append(const char *utf8) { return g_draft.append(utf8); }
int32_t gh_str_insert_str(const char *utf8) { return g_draft.insert_str(utf8); }

int32_t gh_str_len(void) { return g_draft.size(); }
int32_t gh_str_bytes(void) { return static_cast<int32_t>(g_draft.utf8_of().size()); }
int32_t gh_str_version(void) { return g_draft.version; }
int32_t gh_str_codepoint(int32_t index) { return g_draft.at(index); }
const char *gh_str_cstr(void) { return g_draft.utf8_of().c_str(); }

int32_t gh_str_insert(int32_t index, int32_t cp) { return g_draft.insert(index, cp); }
int32_t gh_str_delete(int32_t index) { return g_draft.del(index); }
int32_t gh_str_truncate(int32_t count) { return g_draft.truncate(count); }

int32_t gh_str_set_cursor(int32_t pos) { return g_draft.set_cursor(pos); }
int32_t gh_str_cursor(void) { return g_draft.cursor; }
int32_t gh_str_select_extend(int32_t pos) { return g_draft.select_extend(pos); }
int32_t gh_str_set_anchor(int32_t pos) { return g_draft.set_anchor(pos); }
int32_t gh_str_anchor(void) { return g_draft.anchor; }
int32_t gh_str_select_clear(void) { return g_draft.select_clear(); }
int32_t gh_str_sel_start(void) { return g_draft.sel_lo(); }
int32_t gh_str_sel_end(void) { return g_draft.sel_hi(); }
int32_t gh_str_has_selection(void) { return g_draft.has_sel() ? 1 : 0; }
int32_t gh_str_delete_selection(void) { return g_draft.delete_selection(); }
const char *gh_str_selection(void) { return g_draft.sel_of().c_str(); }

void gh_str_dump(void)
{
    std::fprintf(stderr, "[gh:str] cp=%d bytes=%d cursor=%d sel=[%d,%d) ver=%d\n",
                 static_cast<int>(g_draft.size()), static_cast<int>(g_draft.utf8_of().size()),
                 static_cast<int>(g_draft.cursor), static_cast<int>(g_draft.sel_lo()),
                 static_cast<int>(g_draft.sel_hi()), static_cast<int>(g_draft.version));
}


void gh_ed_reset(void) { g_doc.reset(); }

int32_t gh_ed_set(const char *utf8, const char *path)
{
    const int32_t n = g_doc.set(utf8);
    g_doc.mark_loaded();
    if (path && *path)
    {
        g_doc_path = norm_path(path);
        g_doc_mtime = file_mtime(g_doc_path);
    }
    g_save_error.clear();
    return n;
}
int32_t gh_ed_append(const char *utf8) { return g_doc.append(utf8); }
int32_t gh_ed_add(int32_t cp) { return g_doc.add(cp); }
int32_t gh_ed_backspace(void) { return g_doc.backspace(); }

int32_t gh_ed_len(void) { return g_doc.size(); }
int32_t gh_ed_bytes(void) { return static_cast<int32_t>(g_doc.utf8_of().size()); }
int32_t gh_ed_version(void) { return g_doc.version; }
int32_t gh_ed_codepoint(int32_t index) { return g_doc.at(index); }
const char *gh_ed_cstr(void) { return g_doc.utf8_of().c_str(); }

int32_t gh_ed_set_cursor(int32_t pos) { return g_doc.set_cursor(pos); }
int32_t gh_ed_cursor(void) { return g_doc.cursor; }
int32_t gh_ed_select_extend(int32_t pos) { return g_doc.select_extend(pos); }
int32_t gh_ed_set_anchor(int32_t pos) { return g_doc.set_anchor(pos); }
int32_t gh_ed_anchor(void) { return g_doc.anchor; }
int32_t gh_ed_select_clear(void) { return g_doc.select_clear(); }
int32_t gh_ed_sel_start(void) { return g_doc.sel_lo(); }
int32_t gh_ed_sel_end(void) { return g_doc.sel_hi(); }
int32_t gh_ed_has_selection(void) { return g_doc.has_sel() ? 1 : 0; }
int32_t gh_ed_delete_selection(void) { return g_doc.delete_selection(); }
const char *gh_ed_selection(void) { return g_doc.sel_of().c_str(); }


int32_t gh_ed_insert_str(const char *utf8) { return g_doc.insert_str(utf8); }

int32_t gh_ed_newline(void) { return g_doc.newline(); }

int32_t gh_ed_delete_at(int32_t index) { return g_doc.del(index); }

const char *gh_utf8_cp(int32_t cp)
{
    static std::string out;
    out.clear();
    if (cp < 0 || cp > 0x10FFFF) return out.c_str();
    append_utf8(out, static_cast<uint32_t>(cp));
    return out.c_str();
}
int32_t gh_ed_undo(void) { return g_doc.undo(); }
int32_t gh_ed_redo(void) { return g_doc.redo(); }
int32_t gh_ed_can_undo(void) { return g_doc.undo_stack.empty() ? 0 : 1; }
int32_t gh_ed_can_redo(void) { return g_doc.redo_stack.empty() ? 0 : 1; }
int32_t gh_ed_dirty(void) { return g_doc.dirty() ? 1 : 0; }


namespace {

std::u32string to_u32(const char *u8)
{
    std::u32string out;
    if (!u8) return out;
    const char *p = u8;
    const char *end = u8 + std::strlen(u8);
    while (p < end) out.push_back(static_cast<char32_t>(decode_utf8(p, end)));
    return out;
}

int32_t find_fwd(const std::u32string &cp, const std::u32string &needle, int32_t from)
{
    if (needle.empty()) return -1;
    if (from < 0) from = 0;
    const std::u32string::size_type n = needle.size();
    if (static_cast<std::u32string::size_type>(from) + n > cp.size()) return -1;
    for (std::u32string::size_type i = static_cast<std::u32string::size_type>(from);
         i + n <= cp.size(); ++i)
        if (cp.compare(i, n, needle) == 0) return static_cast<int32_t>(i);
    return -1;
}

int32_t find_bwd(const std::u32string &cp, const std::u32string &needle, int32_t from)
{
    if (needle.empty()) return -1;
    if (from > static_cast<int32_t>(cp.size())) from = static_cast<int32_t>(cp.size());
    const std::u32string::size_type n = needle.size();
    int32_t hi = from;
    if (static_cast<std::u32string::size_type>(hi) + n > cp.size())
        hi = static_cast<int32_t>(cp.size()) - static_cast<int32_t>(n);
    for (int32_t i = hi; i >= 0; --i)
        if (cp.compare(static_cast<std::u32string::size_type>(i), n, needle) == 0) return i;
    return -1;
}

}

int32_t gh_ed_find(const char *needle, int32_t from_cp)
{
    return find_fwd(g_doc.raw(), to_u32(needle), from_cp);
}

int32_t gh_ed_rfind(const char *needle, int32_t from_cp)
{
    return find_bwd(g_doc.raw(), to_u32(needle), from_cp);
}

int32_t gh_ed_find_count(const char *needle)
{
    const std::u32string cp = g_doc.raw();
    const std::u32string n = to_u32(needle);
    if (n.empty() || n.size() > cp.size()) return 0;
    int32_t total = 0;
    for (std::u32string::size_type i = 0; i + n.size() <= cp.size(); ++i)
        if (cp.compare(i, n.size(), n) == 0)
        {
            ++total;
            i += n.size() - 1;
        }
    return total;
}







int32_t gh_ed_ordinal(const char *needle, int32_t cp)
{
    const std::u32string text = g_doc.raw();
    const std::u32string n = to_u32(needle);
    if (n.empty() || cp < 0) return -1;
    if (static_cast<std::u32string::size_type>(cp) + n.size() > text.size()) return -1;
    if (text.compare(static_cast<std::u32string::size_type>(cp), n.size(), n) != 0) return -1;
    int32_t idx = 0;
    for (std::u32string::size_type i = 0; i < cp; )
    {
        if (text.compare(i, n.size(), n) == 0)
        {
            idx += 1;
            i += n.size();
        }
        else
        {
            i += 1;
        }
    }
    return idx + 1;
}










int32_t gh_ed_replace_selection(const char *replacement)
{
    return g_doc.replace_selection(to_u32(replacement));
}


namespace {

struct McCursor
{
    int32_t anchor;
    int32_t cursor;
};

constexpr size_t kMcMax = 32;
std::vector<McCursor> g_mc;

McCursor apply_one(const McCursor &c, int32_t op, const std::u32string &text)
{
    g_doc.set_cursor(c.cursor);
    g_doc.select_extend(c.anchor);
    switch (op)
    {
    case 0:
        if (c.anchor != c.cursor) g_doc.replace_selection(text);
        else g_doc.insert_raw(text);
        return {g_doc.cursor, g_doc.cursor};
    case 1:
        if (c.anchor != c.cursor) g_doc.delete_selection();
        else g_doc.backspace();
        return {g_doc.cursor, g_doc.cursor};
    default:
        if (c.anchor != c.cursor) g_doc.delete_selection();
        else g_doc.del(g_doc.cursor);
        return {g_doc.cursor, g_doc.cursor};
    }
}

void apply_all(int32_t op, const std::u32string &text)
{
    if (g_mc.empty())
    {
        McCursor one{g_doc.anchor, g_doc.cursor};
        g_doc.tx_begin();
        g_mc.push_back(apply_one(one, op, text));
        g_doc.tx_commit();
        const McCursor r = g_mc.back();
        g_mc.clear();
        g_doc.set_cursor(r.cursor);
        return;
    }

    g_doc.tx_begin();
    std::vector<McCursor> next;
    next.reserve(g_mc.size());
    for (size_t i = g_mc.size(); i-- > 0;)
    {
        const McCursor r = apply_one(g_mc[i], op, text);
        next.push_back(r);
    }
    std::reverse(next.begin(), next.end());
    g_doc.tx_commit();
    g_mc.swap(next);
    const McCursor &p = g_mc.back();
    g_doc.set_cursor(p.cursor);
}

bool is_word_cp(char32_t c)
{
    if (c < 128)
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
    return true;
}

}

int32_t gh_ed_mc_clear()
{
    g_mc.clear();
    return 0;
}

int32_t gh_ed_mc_count() { return static_cast<int32_t>(g_mc.size()); }

int32_t gh_ed_mc_add(int32_t pos)
{
    if (g_mc.size() >= kMcMax) return 0;
    const int32_t p = g_doc.clamp(pos);
    for (const McCursor &c : g_mc)
        if (c.anchor == p && c.cursor == p) return 0;
    if (g_mc.empty()) g_mc.push_back({g_doc.anchor, g_doc.cursor});
    g_mc.push_back({p, p});
    return static_cast<int32_t>(g_mc.size());
}

int32_t gh_ed_mc_pos(int32_t i)
{
    if (i < 0 || i >= static_cast<int32_t>(g_mc.size())) return -1;
    return g_mc[static_cast<size_t>(i)].cursor;
}

int32_t gh_ed_mc_sel(int32_t i)
{
    if (i < 0 || i >= static_cast<int32_t>(g_mc.size())) return -1;
    const McCursor &c = g_mc[static_cast<size_t>(i)];
    if (c.anchor == c.cursor) return -1;
    return c.anchor < c.cursor ? c.anchor : c.cursor;
}






int32_t gh_ed_mc_add_next()
{
    const std::u32string &cp = g_doc.raw();
    if (cp.empty()) return static_cast<int32_t>(g_mc.size());

    std::u32string want;
    int32_t a = -1;
    int32_t b = -1;
    if (g_mc.empty())
    {
        a = g_doc.sel_lo();
        b = g_doc.sel_hi();
        if (a == b)
        {
            int32_t lo = g_doc.cursor;
            int32_t hi = g_doc.cursor;
            while (lo > 0 && is_word_cp(cp[static_cast<size_t>(lo - 1)])) --lo;
            while (hi < g_doc.size() && is_word_cp(cp[static_cast<size_t>(hi)])) ++hi;
            a = lo;
            b = hi;
            if (b <= a) return 0;
            g_doc.set_cursor(a);
            g_doc.select_extend(b);
            return 0;
        }
        want = cp.substr(static_cast<size_t>(a), static_cast<size_t>(b - a));
        if (want.empty()) return 0;
        g_mc.push_back({a, b});
    }
    else
    {
        const McCursor &p = g_mc.back();
        a = p.anchor < p.cursor ? p.anchor : p.cursor;
        b = p.anchor < p.cursor ? p.cursor : p.anchor;
        if (a < 0 || b <= a || b > g_doc.size()) return static_cast<int32_t>(g_mc.size());
        want = cp.substr(static_cast<size_t>(a), static_cast<size_t>(b - a));
    }
    if (want.empty()) return static_cast<int32_t>(g_mc.size());

    const int32_t from = b;
    int32_t at = find_fwd(cp, want, from);
    if (at < 0) at = find_fwd(cp, want, 0);
    if (at < 0) return static_cast<int32_t>(g_mc.size());
    for (const McCursor &c : g_mc)
        if ((c.anchor < c.cursor ? c.anchor : c.cursor) == at) return static_cast<int32_t>(g_mc.size());
    g_mc.push_back({at, at + static_cast<int32_t>(want.size())});
    const McCursor &p = g_mc.back();
    g_doc.set_cursor(p.cursor);
    g_doc.set_anchor(p.anchor);
    return static_cast<int32_t>(g_mc.size());
}

int32_t gh_ed_mc_type(int32_t cp)
{
    const std::u32string one(1, static_cast<char32_t>(cp));
    apply_all(0, one);
    return 1;
}

int32_t gh_ed_mc_str(const char *utf8)
{
    const std::u32string t = to_u32(utf8);
    if (t.empty()) return 0;
    apply_all(0, t);
    return 1;
}

int32_t gh_ed_mc_backspace()
{
    apply_all(1, std::u32string());
    return 1;
}

int32_t gh_ed_mc_delete()
{
    apply_all(2, std::u32string());
    return 1;
}

int32_t gh_ed_undo_depth(void) { return static_cast<int32_t>(g_doc.undo_stack.size()); }

int32_t gh_ed_replace_all(const char *needle, const char *replacement)
{
    const std::u32string n = to_u32(needle);
    const std::u32string rep = to_u32(replacement);
    if (n.empty()) return 0;

    std::u32string out;
    out.reserve(g_doc.size());
    const std::u32string &cp = g_doc.raw();
    int32_t done = 0;
    std::u32string::size_type i = 0;
    while (i < cp.size())
    {
        if (i + n.size() <= cp.size() && cp.compare(i, n.size(), n) == 0)
        {
            out += rep;
            i += n.size();
            done += 1;
        }
        else
        {
            out.push_back(cp[i]);
            i += 1;
        }
    }
    if (done == 0) return 0;
    return g_doc.replace_all(out) ? done : 0;
}


namespace {
StrBuf *find_buf(int32_t field) { return (field == 1) ? &g_find[1] : &g_find[0]; }
}

int32_t gh_find_set(int32_t field, const char *utf8) { return find_buf(field)->set(utf8); }
int32_t gh_find_add(int32_t field, int32_t cp) { return find_buf(field)->add(cp); }
int32_t gh_find_backspace(int32_t field) { return find_buf(field)->backspace(); }
int32_t gh_find_clear(int32_t field)
{
    StrBuf *b = find_buf(field);
    b->reset();
    return 0;
}
int32_t gh_find_len(int32_t field) { return find_buf(field)->size(); }
const char *gh_find_cstr(int32_t field) { return find_buf(field)->utf8_of().c_str(); }








int32_t gh_find_codepoint(int32_t field, int32_t index)
{
    StrBuf *b = find_buf(field);
    if (index < 0 || index >= b->size()) return -1;
    return b->at(index);
}




namespace {
    bool atomic_write(const std::string &path, const std::string &data, std::string &err)
    {
        const std::string tmp = path + ".ghhand-tmp";
        {
            std::FILE *f = std::fopen(tmp.c_str(), "wb");
            if (!f)
            {
                err = "打不开临时文件：" + tmp;
                return false;
            }
            const size_t n = std::fwrite(data.data(), 1, data.size(), f);
            const bool ok = (n == data.size());
            std::fclose(f);
            if (!ok)
            {
                std::error_code ignored;
                std::filesystem::remove(tmp, ignored);
                err = "写临时文件时磁盘满了或者出错了";
                return false;
            }
        }
#ifdef _WIN32
        if (!MoveFileExA(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING))
        {
            std::error_code ignored;
            std::filesystem::remove(tmp, ignored);
            err = "替换目标文件失败（文件可能被别的程序占着）";
            return false;
        }
        return true;
#else
        std::error_code ec;
        std::filesystem::rename(tmp, path, ec);
        if (ec)
        {
            err = "替换目标文件失败：" + ec.message();
            return false;
        }
        return true;
#endif
    }
}

int32_t gh_ed_save(const char *utf8_path)
{
    g_save_error.clear();

    if (!g_doc.valid())
    {
        g_save_error = "没有打开任何文件";
        return 4;
    }

    if (!utf8_path || !*utf8_path)
    {
        g_save_error = "保存必须给出路径，不接受空路径";
        return 1;
    }
    const std::string want = norm_path(utf8_path);
    if (want != g_doc_path)
    {
        g_save_error = "只能保存到已打开的那个文件，不接受改路径";
        return 1;
    }

    const int64_t now = file_mtime(want);
    if (now != g_doc_mtime)
    {
        g_save_error = "这个文件在打开之后被别的程序改过了。为了不覆盖掉那些改动，已拒绝保存。";
        return 2;
    }

    std::string err;
    if (!atomic_write(want, g_doc.utf8_of(), err))
    {
        g_save_error = err;
        return 3;
    }

    g_doc_mtime = file_mtime(want);
    g_doc.mark_saved();
    return 0;
}

const char *gh_save_error(void) { return g_save_error.c_str(); }

}