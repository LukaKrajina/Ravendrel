// ═══════════════════════════════════════════════════════════════════════
// Bro 可变字符串实现
// ═══════════════════════════════════════════════════════════════════════
//
// 见 BroStr.hpp 顶部：为什么接口按码点而不是字节设计。
//
// 内部存法：std::u32string（码点数组）。rvdl 侧的操作全是「码点下标」，
// 只有对外暴露 cstr / selection 时才编码成 UTF-8 —— 编码只发生在这两处，
// 所以不可能出现「从中间切开一个汉字」的情况。
//
// ── 为什么是两个实例 ────────────────────────────────────────────────────
//
// 第一版只有一块缓冲，理由是「rvdl 侧只有一个输入框」。
// 有了编辑器视图之后这个前提不成立了：
//   实例 0（gh_str_*）输入区草稿
//   实例 1（gh_ed_*）  当前打开的代码文件
// 两者必须分开，因为它们的生命周期完全不同：草稿要跨「打开文件→关掉文件」
// 保留，文件内容要随切换重新载入。挤在一块的话每帧都要判断
// 「我现在该写哪一块」，而漏判一次就是用户输入的草稿被文件内容覆盖。

#include "qhal/BroStr.hpp"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <filesystem>
#include <chrono>

#ifdef _WIN32
#include <windows.h>   // MoveFileExA：原子替换
#endif

namespace
{
    // ── UTF-8 编解码 ──────────────────────────────────────────────────
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

    /// 从 utf8 读一个码点并推进 p。非法字节返回 U+FFFD 且只前进 1，
    /// 保证不会死循环。
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

    /// 一块可编辑文本缓冲。
    struct StrBuf
    {
        std::u32string cp;
        int32_t cursor = 0;
        int32_t anchor = 0;   // == cursor 表示无选区

        std::string utf8;     // cstr 的缓存
        std::string sel;      // selection 的缓存

        // 每次内容变更递增。BroSyntax.cpp 靠它判断缓存要不要重建 ——
        // 光标移动不递增（内容没变，只有版本无关的选区变了）。
        int32_t version = 0;

        // ── 撤销 / 重做 ──────────────────────────────────────────────
        //
        // 存**整份快照**而不是「反向操作」。理由：
        //   · 快照一定正确。攒反向操作要逐个操作类型写反向代码，
        //     而漏一种（比如「插入一段字符串」）就撤销不回去，
        //     而且症状是「按了撤销，代码乱掉」，极难定位。
        //   · 成本可接受。一个 500 行的文件快照约 20 KB，
        //     64 级 ≈ 1.3 MB —— 换掉一整类 bug，划算。
        //
        // 光标移动不记快照：没有内容变化，撤销它对用户没有意义。
        struct Snapshot
        {
            std::u32string cp;
            int32_t cursor;
            int32_t anchor;
        };
        static constexpr size_t kUndoMax = 64;
        std::vector<Snapshot> undo_stack;
        std::vector<Snapshot> redo_stack;

        // ── 事务（多光标用）─────────────────────────────────────────────
        //
        // 「一次操作 = 一个撤销点」是多处逻辑共同依赖的约定。
        // 单光标时天然成立：每个编辑方法自己 push_undo 一次。
        // 多光标时一次按键要改 N 个位置，逐个调用就压 N 个撤销点 ——
        // 用户按一次撤销只撤掉一处，看起来就像「撤销坏了」。
        //
        // 所以给 push_undo 加一道闸：事务期间它什么都不做，
        // 整个事务由 commit() 统一压**一个**快照（事务开始时的内容）。
        int32_t tx_depth = 0;
        std::u32string tx_cp;
        int32_t tx_cursor = 0;
        int32_t tx_anchor = 0;

        /// 开事务。嵌套调用只有最外层真的快照。
        void tx_begin()
        {
            if (tx_depth++ == 0)
            {
                tx_cp = cp;
                tx_cursor = cursor;
                tx_anchor = anchor;
            }
        }

        /// 提交：把事务开始时的状态压成**一个**撤销点。
        ///
        /// 内容没变的话**不压** —— 「按了 Ctrl+C 但没选区」这种
        /// 空操作不该凭空多出一个撤销点（按撤销会「什么都没发生」，
        /// 用户以为撤销坏了）。
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

        /// 在**改动内容之前**调用
        void push_undo()
        {
            // 事务期间由 tx_commit 统一处理。
            if (tx_depth > 0) return;
            undo_stack.push_back({cp, cursor, anchor});
            if (undo_stack.size() > kUndoMax) undo_stack.erase(undo_stack.begin());
            // 新的编辑让重做栈失效 —— 这条不能省，
            // 否则「撤销 → 改点别的 → 重做」会把改过的东西也重做掉。
            redo_stack.clear();
        }

        /// 整份替换（打开文件）时清空历史：那不是一次「编辑」
        void clear_history()
        {
            undo_stack.clear();
            redo_stack.clear();
        }

        // ── 脏标记 ────────────────────────────────────────────────────
        //
        // **比内容，不比版本号。**
        //
        // 一开始用版本号（`version != saved_version`），看着更省，
        // 结果撤销一下就变成脏的：undo 会调 invalidate()、版本号往前跳，
        // 可内容其实已经回到载入时的样子了。症状是
        // 「撤销回原样了，文件名旁边的 ● 还在」—— 极易误判成
        // 「撤销根本没生效」，然后往完全错的方向查。
        //
        // 比内容就是老老实实比一次。界面每帧问一次，一个 300 行文件
        // 约 15 KB，1900fps 下 28 MB/s —— 对 memcmp 是白给的，
        // 换来的是「脏」和「内容」永远一致。
        std::u32string loaded_cp;
        bool has_loaded = false;
        bool valid() const { return has_loaded; }
        bool dirty() const { return has_loaded && cp != loaded_cp; }
        void mark_saved() { loaded_cp = cp; }
        /// 打开文件：基线 = 当前内容，所以一打开就是不脏的
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

        /// 直接读码点数组（查找用）。只读，改动必须走本类的方法。
        const std::u32string &raw() const { return cp; }

        /**
         * 用 add 替换当前选区。**一个**撤销点。
         *
         * ⚠ 不能靠 rvdl 侧「delete_selection + insert_str」凑：
         *   - delete_selection 压一个撤销位置，insert_str 又压一个
         *     → 一次替换要按两下撤销，看起来就像「撤销坏了」；
         *   - insert_str 在入参为空时**提前返回、来不及删选区**，
         *     所以「替换成空串」会什么都不发生。
         * 这两个坑都真实存在，所以这里必须是单独一个原语。
         */
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

        /// 整份替换（全部替换用）。**一个**撤销点。
        ///
        /// 不做成「循环调 insert_str」是因为那样会压 N 个撤销位置，
        /// 用户按一次撤销只撤掉一处 —— 看起来就像「撤销坏了」。
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

        // ── 操作 ──────────────────────────────────────────────────
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
            anchor = cursor;   // 追加会清掉选区
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
            // 整份替换不算「一次编辑」。不清历史的话，
            // 打开文件后第一次按撤销会回到一个空文档 —— 很难解释。
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

        /// 点击：anchor = cursor = pos，**会**清掉选区
        int32_t set_cursor(int32_t pos)
        {
            cursor = clamp(pos);
            anchor = cursor;
            return cursor;
        }

        /// 拖选 / Shift+方向：挪 cursor，**保住** anchor
        int32_t select_extend(int32_t pos)
        {
            cursor = clamp(pos);
            return has_sel() ? sel_hi() - sel_lo() : 0;
        }

        /// 只挪 anchor
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

        /// 插入一段字符串（换行、Tab 缩进、粘贴）。返回插入的码点数。
        ///
        /// 单独一个入口而不是让 rvdl 循环调 add()：那样一次 Tab 会压 4~8 个
        /// 撤销栈位置，用户按一次撤销只撤掉一个空格，
        /// 表现得像「撤销坏了」。
        int32_t insert_str(const char *u8)
        {
            if (!u8) return 0;
            const char *p = u8;
            const char *end = u8 + std::strlen(u8);
            std::u32string add;
            while (p < end) add.push_back(static_cast<char32_t>(decode_utf8(p, end)));
            return insert_raw(add);
        }

        /// insert_str 的内部版本：已经是码点序列。
        ///
        /// 单独拆出来是因为多光标那边拿到的是 u32string，
        /// 让它先转成 UTF-8 再解回来纯属浪费，而且会丢掉
        /// 「非法序列也照样插进去」这份宽容。
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

        /// 换行并**继承当前行的前导空白**。返回插入的码点数。
        ///
        /// 为什么必须在这里做而不能在 rvdl 里：rvdl 拼不出动态字符串
        /// （`"a" + "b"` 是指针加法），而「数出前导空白有几个再拼回去」
        /// 正好需要动态字符串。退而求其次用 insert_str 一个空格一次，
        /// 会把一次回车拆成 N 个撤销位置 —— 用户按一次撤销只退掉一个空格，
        /// 表现成「撤销坏了」。
        ///
        /// 只继承空白，不多给一级：多给一级（`{` 之后自动缩进）要判断
        /// 语法上下文，那是词法器的活，不该在缓冲层猜。
        int32_t newline()
        {
            if (has_sel()) delete_selection();
            const int32_t at = cursor;

            // 找当前行的行首。
            int32_t ls = at;
            while (ls > 0 && cp[ls - 1] != U'\n') ls -= 1;

            // 量的是**本行的前导空白**，不是「光标之前的空白」。
            //
            // ⚠ 这两个不是一回事。写成「从行首一路扫到光标、统计空白」的话，
            //   光标在行尾时扫到的就是**整行** —— 于是一行 `    abc`
            //   在行尾回车，扫出 7 个字符、里面混着 'a'，
            //   判成「不是纯空白」→ 缩进一个都不继承。
            //   症状是「回车继承缩进好像没生效」，而这段代码读起来完全合理。
            //
            // 上界取光标：光标停在缩进中间时只继承到那儿，
            // 不会凭空多给一级。
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

    StrBuf g_draft;   // 实例 0：输入区草稿
    StrBuf g_doc;     // 实例 1：编辑器里打开的文件
    // 实例 2：编辑器查找框。field 0 = 查找词，field 1 = 替换词。
    //
    // 为什么用两个实例而不是参数：g_draft 已经被聊天输入框占着，
    // 查找框共用它的话，用户打了半句话按 Ctrl+F 就没了 ——
    // 「输入框 A 抢走输入框 B 的缓冲」这个坑在 draft/doc 上已经栽过一次
    // （见本文件顶部的注释），这里不重复。
    //
    // 为什么是**数组**而不是再加两个 StrBuf：查找框要用到的操作只有 6 个，
    // 而 gh_str_* / gh_ed_* 那套是 26 个。照抄一遍等于把接口面积翻倍，
    // 而其中 insert/truncate/anchor/selection 这些查找框一个都不需要。
    StrBuf g_find[2];

    // ── 保存：路径与外部修改检测 ──────────────────────────────────────
    //
    // 放在这里而不是 shell.cpp，因为「打开文件时记下它的修改时间」
    // 和「写它」必须是同一个状态机。shell 那边只做槽位搬运。
    std::string g_doc_path;
    int64_t g_doc_mtime = 0;     // 载入时的 mtime（0 = 文件当时不存在）
    std::string g_save_error;

    std::string norm_path(const std::string &p)
    {
        return p.empty() ? p : std::filesystem::weakly_canonical(p).string();
    }

    /// 取磁盘上的 mtime；文件不存在返回 0
    int64_t file_mtime(const std::string &p)
    {
        std::error_code ec;
        auto t = std::filesystem::last_write_time(p, ec);
        if (ec) return 0;
        // 转成「秒」量级就够：只用来判断「载入之后有没有变过」，
        // 不需要纳秒精度，而跨平台取 filetime 的转换很啰嗦。
        return static_cast<int64_t>(
            std::chrono::duration_cast<std::chrono::seconds>(t.time_since_epoch()).count());
    }
} // namespace

extern "C"
{

// ═══════════════════════════════════════════════════════════════════════
// 实例 0 —— 输入区草稿（gh_str_*）
// ═══════════════════════════════════════════════════════════════════════

void gh_str_reset(void) { g_draft.reset(); }
int32_t gh_str_add(int32_t cp) { return g_draft.add(cp); }
int32_t gh_str_add_ascii(int32_t c) { return g_draft.add(c); }
int32_t gh_str_backspace(void) { return g_draft.backspace(); }
int32_t gh_str_set(const char *utf8) { return g_draft.set(utf8); }
int32_t gh_str_append(const char *utf8) { return g_draft.append(utf8); }
/// 在光标处插入一段文本（**一个**撤销点），有选区先删选区。
///
/// 为什么必须有：粘贴一段话如果用 add() 逐码点插入，
/// 会压 N 个撤销位置 —— 用户按一次撤销只撤掉一个字，
/// 表现为「粘贴之后撤销一次只少一个字」。
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

// ═══════════════════════════════════════════════════════════════════════
// 实例 1 —— 编辑器文档（gh_ed_*）
// ═══════════════════════════════════════════════════════════════════════
//
// 签名与 gh_str_* 完全一致，只是换了块缓冲。rvdl 侧两个视图各用一套，
// 不需要在每帧判断「当前该操作哪一块」—— 那种判断漏一次就是
// 用户正在打的草稿被文件内容冲掉。

void gh_ed_reset(void) { g_doc.reset(); }

int32_t gh_ed_set(const char *utf8, const char *path)
{
    const int32_t n = g_doc.set(utf8);
    g_doc.mark_loaded();
    if (path && *path)
    {
        g_doc_path = norm_path(path);
        // 记下「载入时的修改时间」。保存时拿它比对：
        // 用户在 VSCode 里改了同一个文件又切回来，
        // 我们的保存会把人家的改动直接盖掉 —— 而界面上看不出任何异常。
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

// ── 编辑 ────────────────────────────────────────────────────────────────

int32_t gh_ed_insert_str(const char *utf8) { return g_doc.insert_str(utf8); }

/// 回车 = 换行 + 继承前导空白。见 StrBuf::newline 的注释。
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

// ── 查找 / 替换 ─────────────────────────────────────────────────────────
//
// ⚠ 为什么搜索在 C++ 而不在 rvdl：rvdl 可以用 gh_ed_codepoint() 逐码点比较，
//   但一个 5000 行文件（10 万码点）配 3 字查找词就是 30 万次 JIT 循环。
//   更关键的是**测不了** —— 这段逻辑要构造缓冲区 + 循环 + 多个返回值，
//   在 rvdl 自检里只能靠「跑一遍看结果对不对」，而向前/向后/找不到/
//   空 needle/跨行匹配这五种情况各自都能静默地错。
//   放 C++ 之后每一种都是一行断言。

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

/// 在 cp 里找 needle，从 from 开始。找不到返回 -1（**不回绕**）。
///
/// 回绕留给 rvdl：它需要知道「一共几处」才能显示 `N/M`，
/// 而那个计数是界面的事，不该混进底层。
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

/// 从 from **往回**找。返回匹配**起点**。
int32_t find_bwd(const std::u32string &cp, const std::u32string &needle, int32_t from)
{
    if (needle.empty()) return -1;
    if (from > static_cast<int32_t>(cp.size())) from = static_cast<int32_t>(cp.size());
    // 起点不能超过 from，否则会把「正好在 from 之前的那个」漏掉
    const std::u32string::size_type n = needle.size();
    int32_t hi = from;
    if (static_cast<std::u32string::size_type>(hi) + n > cp.size())
        hi = static_cast<int32_t>(cp.size()) - static_cast<int32_t>(n);
    for (int32_t i = hi; i >= 0; --i)
        if (cp.compare(static_cast<std::u32string::size_type>(i), n, needle) == 0) return i;
    return -1;
}

} // namespace

int32_t gh_ed_find(const char *needle, int32_t from_cp)
{
    return find_fwd(g_doc.raw(), to_u32(needle), from_cp);
}

int32_t gh_ed_rfind(const char *needle, int32_t from_cp)
{
    return find_bwd(g_doc.raw(), to_u32(needle), from_cp);
}

/** 一共几处匹配。界面上要显示 `N/M`，所以这个数得单独给。 */
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
            i += n.size() - 1;   // 跳过已匹配的部分，避免「aa」在「aaaa」里数成 3
        }
    return total;
}

/**
 * cp 处那一处匹配是第几个（1 起）。不是匹配起点就返回 -1。
 *
 * 为什么单独给：界面上要显示「3/17」，得知道当前落在第几处。
 * 在 rvdl 里数的话只能循环调 gh_ed_find —— 那是 O(处数 × 全文)，
 * 一个 5000 行文件每按一次回车就白扫几十遍全文。
 */
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

/**
 * 全部替换。返回替换了几处。必须是一个操作、**一个撤销点**。
 */
/**
 * 替换**当前选区**。一个撤销点。
 *
 * ⚠ 必须在 C++ 做，不能让 rvdl 调 delete_selection + insert_str 凑：
 *   那样会压两个撤销位置（一次替换要按两下撤销），
 *   而且 insert_str 入参为空时会提前返回、**来不及删选区** ——
 *   「替换成空串」会静默地什么都不发生。
 */
int32_t gh_ed_replace_selection(const char *replacement)
{
    return g_doc.replace_selection(to_u32(replacement));
}

// ── 多光标 ─────────────────────────────────────────────────────────────
//
// 光标列表放在这里而不是 rvdl，有两个具体原因：
//   1. **rvdl 没有数组语法**（整个 rvdl 代码库里一个 `int32[]` 都没有）。
//      放 rvdl 就只能写 `UI_ED_MC_C0..C31` 这种一长串标量，
//      而且每加一个原语就得同步改一长串。
//   2. 一次按键要在 N 个位置做「一次编辑」，其中「只压一个撤销点」
//      这件事本来就得进 StrBuf（见 tx_begin / tx_commit）。
//   两件事都在同一侧，顺放一起比拆开省事。

namespace {

struct McCursor
{
    int32_t anchor;
    int32_t cursor;
};

/// 光标上限。防的是「Ctrl+D 按到手指抽筋」把每次编辑变成几百次全量快照。
constexpr size_t kMcMax = 32;
std::vector<McCursor> g_mc;

/// 从后往前改：前面的编辑不会影响后面光标的码点下标。
///
/// 顺序反了的话，第 i 个光标插入 N 个码点之后，
/// 后面那些光标的旧下标就全错位了 —— 表现为「后几个光标插到了奇怪的位置」。
McCursor apply_one(const McCursor &c, int32_t op, const std::u32string &text)
{
    g_doc.set_cursor(c.cursor);
    g_doc.select_extend(c.anchor);
    switch (op)
    {
    case 0: // 插入（替换选区）
        if (c.anchor != c.cursor) g_doc.replace_selection(text);
        else g_doc.insert_raw(text);
        return {g_doc.cursor, g_doc.cursor};
    case 1: // 退格：删光标前一个（有选区先删选区）
        if (c.anchor != c.cursor) g_doc.delete_selection();
        else g_doc.backspace();
        return {g_doc.cursor, g_doc.cursor};
    default: // Delete：删光标后一个（有选区先删选区）
        if (c.anchor != c.cursor) g_doc.delete_selection();
        else g_doc.del(g_doc.cursor);
        return {g_doc.cursor, g_doc.cursor};
    }
}

/// 在所有光标上做同一种编辑。**一个撤销点。**
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
    // 主光标（最后一个）同步给单光标状态，
    // 这样 Esc 收成一个、或者换文件时不会残留
    const McCursor &p = g_mc.back();
    g_doc.set_cursor(p.cursor);
}

/// 词：字母 / 数字 / 下划线 / 非 ASCII（CJK 也算「一个词」的一部分，
/// 但按字节展开会把一个汉字拆成三个 —— 所以非 ASCII 整体算一个字符）。
bool is_word_cp(char32_t c)
{
    if (c < 128)
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
    return true;
}

} // namespace

/// 收成一个光标（Esc / 单击 / 打开别的文件）
int32_t gh_ed_mc_clear()
{
    g_mc.clear();
    return 0;
}

/// 当前光标数。0 或 1 都表示「单光标模式」。
int32_t gh_ed_mc_count() { return static_cast<int32_t>(g_mc.size()); }

/// 在 pos 加一个光标。同一位置不重复加。
int32_t gh_ed_mc_add(int32_t pos)
{
    if (g_mc.size() >= kMcMax) return 0;
    const int32_t p = g_doc.clamp(pos);
    for (const McCursor &c : g_mc)
        if (c.anchor == p && c.cursor == p) return 0;
    // 第一个加入的光标就是主光标 —— Alt+点击之前那个单光标要保留
    if (g_mc.empty()) g_mc.push_back({g_doc.anchor, g_doc.cursor});
    g_mc.push_back({p, p});
    return static_cast<int32_t>(g_mc.size());
}

/// 第 i 个光标的位置（渲染用）
int32_t gh_ed_mc_pos(int32_t i)
{
    if (i < 0 || i >= static_cast<int32_t>(g_mc.size())) return -1;
    return g_mc[static_cast<size_t>(i)].cursor;
}

/// 第 i 个光标选区的起点；没有选区返回 -1（渲染时才知道要不要画高亮）
int32_t gh_ed_mc_sel(int32_t i)
{
    if (i < 0 || i >= static_cast<int32_t>(g_mc.size())) return -1;
    const McCursor &c = g_mc[static_cast<size_t>(i)];
    if (c.anchor == c.cursor) return -1;
    return c.anchor < c.cursor ? c.anchor : c.cursor;
}

/**
 * Ctrl+D：选中下一个相同处。
 *
 * 没有选区时先取光标所在的**词**；有选区就用选中的内容。
 * 返回加进去之后的光标数；到头了返回当前数（不重复加）。
 */
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
            // 没选区 → 取光标所在的词
            int32_t lo = g_doc.cursor;
            int32_t hi = g_doc.cursor;
            while (lo > 0 && is_word_cp(cp[static_cast<size_t>(lo - 1)])) --lo;
            while (hi < g_doc.size() && is_word_cp(cp[static_cast<size_t>(hi)])) ++hi;
            a = lo;
            b = hi;
            if (b <= a) return 0;   // 光标两侧都不是词字符（比如停在空格上）
            // ⚠ 第一次 Ctrl+D 是「把光标变成词的选区」，**不是**加光标。
            //   早先的写法是「保留原光标 + 再加一个词的选区」，
            //   而原光标恰好落在那个词里 —— 于是同一个词上叠了两个光标，
            //   打字时它被改两遍（"foo" 打成 "zzo" 之类）。
            //   光标数这时确实没变，所以如实返回 0。
            g_doc.set_cursor(a);
            g_doc.select_extend(b);
            return 0;
        }
        // 单光标 + 有选区 → 这条选区就是第一个多光标
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

    // 从**主光标之后**找下一个，且不能与已有的重叠
    const int32_t from = b;
    int32_t at = find_fwd(cp, want, from);
    if (at < 0) at = find_fwd(cp, want, 0);   // 回绕
    if (at < 0) return static_cast<int32_t>(g_mc.size());
    for (const McCursor &c : g_mc)
        if ((c.anchor < c.cursor ? c.anchor : c.cursor) == at) return static_cast<int32_t>(g_mc.size());
    g_mc.push_back({at, at + static_cast<int32_t>(want.size())});
    const McCursor &p = g_mc.back();
    g_doc.set_cursor(p.cursor);
    g_doc.set_anchor(p.anchor);
    return static_cast<int32_t>(g_mc.size());
}

/// 在所有光标处插入一个码点（打字）。一个撤销点。
int32_t gh_ed_mc_type(int32_t cp)
{
    const std::u32string one(1, static_cast<char32_t>(cp));
    apply_all(0, one);
    return 1;
}

/// 在所有光标处插入一段文本（粘贴）。一个撤销点。
int32_t gh_ed_mc_str(const char *utf8)
{
    const std::u32string t = to_u32(utf8);
    if (t.empty()) return 0;
    apply_all(0, t);
    return 1;
}

/// 所有光标退格。一个撤销点。
int32_t gh_ed_mc_backspace()
{
    apply_all(1, std::u32string());
    return 1;
}

/// 所有光标 Delete。一个撤销点。
int32_t gh_ed_mc_delete()
{
    apply_all(2, std::u32string());
    return 1;
}

/// 撤销栈深度（当前能连续撤多少步）。
///
/// 为什么需要它：`gh_ed_can_undo()` 只是「栈非空吗」这个布尔值。
/// 多光标一次改 3 处却压了 3 个撤销点的话，can_undo 仍然是 1 ——
/// 拿它断言「只压了一个点」是**测不出来的**（实测过：把事务闸门去掉，
/// can_undo 的断言照样全绿）。
/// 而闸门去掉的后果是：撤一次回到原文，**再撤一次会跳到中间某个
/// 不存在的状态**，用户连按 Ctrl+Z 看到代码乱跳。
int32_t gh_ed_undo_depth(void) { return static_cast<int32_t>(g_doc.undo_stack.size()); }

int32_t gh_ed_replace_all(const char *needle, const char *replacement)
{
    const std::u32string n = to_u32(needle);
    const std::u32string rep = to_u32(replacement);
    if (n.empty()) return 0;

    // 先在副本上做完，确认非空再落 —— 万一中途出错，原文还在。
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

// ── 查找框的输入缓冲 ─────────────────────────────────────────────────────
//
// field 0 = 查找词，field 1 = 替换词。只暴露查找框真正用得到的 6~7 个操作，
// 而不是把 gh_str_* 那 26 个抄一遍。

namespace {
StrBuf *find_buf(int32_t field) { return (field == 1) ? &g_find[1] : &g_find[0]; }
} // namespace

int32_t gh_find_set(int32_t field, const char *utf8) { return find_buf(field)->set(utf8); }
int32_t gh_find_add(int32_t field, int32_t cp) { return find_buf(field)->add(cp); }
int32_t gh_find_backspace(int32_t field) { return find_buf(field)->backspace(); }
int32_t gh_find_clear(int32_t field)
{
    StrBuf *b = find_buf(field);
    b->reset();
    return 0;   // 返回清空后的长度（0）
}
int32_t gh_find_len(int32_t field) { return find_buf(field)->size(); }
const char *gh_find_cstr(int32_t field) { return find_buf(field)->utf8_of().c_str(); }

/**
 * 第 i 个码点。越界返回 -1。
 *
 * 为什么需要：跳行要把查找框里那串数字读成整数，而 rvdl **拆不了字符串**
 * （gh_find_cstr 只能一次拿到整串，没有下标访问）。
 * 没有这个原语，跳行就只能靠逐字符拼接硬凑 —— 那正好是本文件
 * 顶部注释里点名要避开的那种做法。
 */
int32_t gh_find_codepoint(int32_t field, int32_t index)
{
    StrBuf *b = find_buf(field);
    if (index < 0 || index >= b->size()) return -1;
    return b->at(index);
}



// ── 保存 ────────────────────────────────────────────────────────────────

namespace {
    /// 原子写：先写临时文件，再替换。
    ///
    /// 直接覆写的话，写到一半崩溃 / 断电会留下一个**半截文件** ——
    /// 而这正好是用户最不能接受的结果：他让编辑器改一下，结果源码坏了。
    ///
    /// Windows 的 std::filesystem::rename 在目标已存在时会失败，
    /// 所以走 MoveFileExA 的 REPLACE_EXISTING。
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
} // namespace

int32_t gh_ed_save(const char *utf8_path)
{
    g_save_error.clear();

    if (!g_doc.valid())
    {
        g_save_error = "没有打开任何文件";
        return 4;
    }

    // 只允许写回**打开时那个路径**。
    //
    // 这是最紧的一条：即使 slots.tsv 被篡改，保存也只能落到
    // 已经打开过的那个文件上，不可能把内容写到别处去。
    // 比「限制在工作区内」还严 —— 工作区里可能有别的东西值得保护。
    //
    // ⚠ 空路径必须**拒绝**，不能当成「省略 = 用已载入的路径」。
    // 早先图省事写成「空就取 g_doc_path」，结果自检里一句
    // gh_ed_save("") 直接在仓库根目录写出了一个叫
    // selftest-fake-path 的文件 —— 而且返回 0，看着还挺成功。
    // 「省略参数就用默认值」这个约定在写文件的场景下太危险了。
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

    // 外部修改检测
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

    // 记下新的 mtime，否则紧接着再存一次会被「外部修改」挡住
    g_doc_mtime = file_mtime(want);
    g_doc.mark_saved();
    return 0;
}

const char *gh_save_error(void) { return g_save_error.c_str(); }

} // extern "C"
