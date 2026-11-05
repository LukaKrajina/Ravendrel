// Fox text/text_buffer.cpp - 编辑模型
//
// 为什么这是一个真正的对象，而不是挂在 char* 上的一堆辅助函数，见
// text_buffer.hpp。

#include "text_buffer.hpp"

#include <cstring>

namespace fox
{
    // ── UTF-8 边界 ───────────────────────────────────────────────────────
    //
    // 光标索引是字节偏移，但绝不能指向码点中间。所有公开的修改入口都经过
    // snap()。
    //
    // 注意这里有意容忍非法输入而不是抛异常：宿主和剪贴板会递来任意字节，
    // 文本框必须扛得住。utf8_decode() 对坏的首字节报告宽度 1，所以扫描总能
    // 前进，不会死循环。

    /// 从 `p` 开始的码点的字节长度，夹取到 `avail` 以内。
    static size_t utf8_step(const unsigned char *p, size_t avail)
    {
        const unsigned char c = p[0];
        size_t n;
        if (c < 0x80)
            n = 1;
        else if ((c & 0xE0) == 0xC0)
            n = 2;
        else if ((c & 0xF0) == 0xE0)
            n = 3;
        else if ((c & 0xF8) == 0xF0)
            n = 4;
        else
            return 1; ///< 游离的续接字节或非法首字节
        return n <= avail ? n : 1;
    }

    static bool is_continuation(unsigned char c) { return (c & 0xC0) == 0x80; }

    /// 以字节 `i` 结尾的码点的起始位置，适用于 0 < i <= s.size()。
    static size_t prev_boundary(const std::string &s, size_t i)
    {
        if (i == 0) return 0;
        // ⚠ 从 i-1 开始扫描，返回码点第一个字节的下标。早先的版本从 `i` 开始
        //   扫描，s[i-1] 不是续接字节时原样返回 `i`——而这是所有 ASCII 字符的
        //   常见情况。于是对 ASCII 它返回的是光标自己的位置：退格毫无反应，
        //   而 move_word 向左会永远死循环，因为它的内层步进是 `i = s`，s == i。
        //   两种情况都不会崩溃：一个吞掉按键，另一个卡死程序。
        size_t j = i - 1;
        while (j > 0 && is_continuation((unsigned char)s[j])) --j;
        return j;
    }

    size_t TextBuffer::snap(size_t byte, bool allow_past_end) const
    {
        if (!allow_past_end && byte > text_.size()) byte = text_.size();
        if (byte > text_.size()) byte = text_.size();

        // 回退到落点所在码点的开头。最多退 3 个字节，因为合法序列最长 4 字节。
        for (int k = 0; k < 4 && byte > 0 && byte < text_.size(); ++k)
        {
            if (!is_continuation((unsigned char)text_[byte])) return byte;
            --byte;
        }
        return byte;
    }

    // ── 内容 ────────────────────────────────────────────────────────

    void TextBuffer::set_text(std::string_view s)
    {
        text_.assign(s);
        caret_ = anchor_ = text_.size();
        clear_history();
        coalescing_ = false;
    }

    void TextBuffer::set_text(const char *utf8, size_t len)
    {
        set_text(std::string_view(utf8 ? utf8 : "", utf8 ? len : 0));
    }

    void TextBuffer::clear()
    {
        // 默认构造的 view，完全不涉及字面量。
        set_text(std::string_view());
    }

    void TextBuffer::select_all()
    {
        anchor_ = 0;
        caret_ = text_.size();
    }

    std::string_view TextBuffer::selected() const
    {
        const size_t a = sel_min(), b = sel_max();
        if (a >= b) return {};
        return std::string_view(text_).substr(a, b - a);
    }

    void TextBuffer::set_caret(size_t byte)
    {
        caret_ = snap(byte);
        // 单纯移动光标会收起选区。所以 select_to() 才作为单独的入口存在：
        // "移到这里"和"扩展到这里"是不同的操作，调用方必须说清楚是哪个。
        anchor_ = caret_;
        sticky_col_ = -1;
    }

    void TextBuffer::select_to(size_t byte)
    {
        composition_cancel();
        // 扩展：光标移动，锚点留在原处。
        caret_ = snap(byte);
    }

    // ── IME 输入组合 ──────────────────────────────────────────────────

    void TextBuffer::composition_begin(std::string_view utf8, size_t caret_bytes)
    {
        // 重新开始组合是正常情况，不是异常：已经在组合时再敲一个拼音字母会
        // 替换整个组合。所以 begin() 和 update() 有意做成同一个操作——这个
        // 区别只对平台有意义，它已经判定这是一次新组合还是对旧组合的编辑。
        preedit_.assign(utf8);
        preedit_caret_ = caret_bytes;
        if (preedit_caret_ > preedit_.size()) preedit_caret_ = preedit_.size();
    }

    void TextBuffer::composition_update(std::string_view utf8, size_t caret_bytes)
    {
        if (preedit_.empty() && utf8.empty()) return;
        preedit_.assign(utf8);
        preedit_caret_ = caret_bytes;
        if (preedit_caret_ > preedit_.size()) preedit_caret_ = preedit_.size();
    }

    void TextBuffer::set_preedit_caret(size_t byte)
    {
        preedit_caret_ = byte > preedit_.size() ? preedit_.size() : byte;
    }

    bool TextBuffer::composition_commit()
    {
        if (preedit_.empty()) return false;
        sticky_col_ = -1;

        const std::string committed = preedit_;
        preedit_.clear();
        preedit_caret_ = 0;

        // 在修改之前记录，而且只记录一次。上屏替换掉的选区是撤销时必须恢复
        // 的状态的一部分；删除若再记一条历史，一次用户操作就要按两次 Ctrl+Z。
        record(EditKind::Discrete, time_);
        delete_selection_no_history();
        text_.insert(caret_, committed);
        caret_ = snap(caret_ + committed.size());
        anchor_ = caret_;
        return true;
    }

    void TextBuffer::composition_cancel()
    {
        preedit_.clear();
        preedit_caret_ = 0;
    }

    // ── 历史 ────────────────────────────────────────────────────────

    void TextBuffer::clear_history()
    {
        undo_.clear();
        redo_.clear();
        coalescing_ = false;
        last_kind_ = EditKind::Discrete;
        last_edit_time_ = -1e30f;
    }

    void TextBuffer::record(EditKind kind, f32 now)
    {
        // 这次编辑要不要并入上一条撤销记录？
        //
        // 只有输入和退格会合并，只合并同类，只在光标没有跳走时合并，且只在
        // 时间窗口内合并。任何新的编辑都会清空重做栈——这就是"重做"的含义。
        const bool merge = coalescing_ && kind != EditKind::Discrete && kind != EditKind::Delete &&
                           kind == last_kind_ && (now - last_edit_time_) <= coalesce_seconds_;

        if (!merge)
        {
            undo_.push_back(TextEditState{text_, caret_, anchor_});
            if (undo_.size() > kMaxHistory) undo_.erase(undo_.begin());
        }

        redo_.clear();
        last_kind_ = kind;
        coalescing_ = true;
        last_edit_time_ = now;
    }

    void TextBuffer::undo()
    {
        composition_cancel();
        if (undo_.empty()) return;
        // 回退之前把当前状态压入 redo 栈，这样重做能回到这里。
        redo_.push_back(TextEditState{text_, caret_, anchor_});
        if (redo_.size() > kMaxHistory) redo_.erase(redo_.begin());

        const TextEditState &s = undo_.back();
        text_ = s.text;
        caret_ = snap(s.caret);
        anchor_ = snap(s.anchor);

        undo_.pop_back();
        // 下一次编辑必须另起一步，不能并入刚撤销的那一步。
        coalescing_ = false;
    }

    void TextBuffer::redo()
    {
        composition_cancel();
        if (redo_.empty()) return;
        undo_.push_back(TextEditState{text_, caret_, anchor_});
        if (undo_.size() > kMaxHistory) undo_.erase(undo_.begin());

        const TextEditState &s = redo_.back();
        text_ = s.text;
        caret_ = snap(s.caret);
        anchor_ = snap(s.anchor);

        redo_.pop_back();
        coalescing_ = false;
    }

    // ── 编辑 ────────────────────────────────────────────────────────

    bool TextBuffer::delete_selection()
    {
        if (!has_selection()) return false;
        record(EditKind::Discrete, time_);
        return delete_selection_no_history();
    }

    bool TextBuffer::delete_selection_no_history()
    {
        if (!has_selection()) return false;
        const size_t a = sel_min(), b = sel_max();
        text_.erase(a, b - a);
        caret_ = anchor_ = snap(a);
        return true;
    }

    void TextBuffer::insert(std::string_view utf8)
    {
        if (utf8.empty()) return;
        // 已确认的插入会取代 IME 正在组合的内容。
        composition_cancel();
        sticky_col_ = -1;

        // 插入单个字符是打字按键，可以和上一次合并。更长的内容（粘贴、IME
        // 上屏、程序性设置）独立成步，否则撤销会把整段粘贴的文字并进前面刚
        // 输入的字母一起吞掉。
        const EditKind kind = utf8.size() == 1 ? EditKind::Insert : EditKind::Discrete;

        // 在修改之前记录：撤销必须把选区也恢复。
        record(kind, time_);
        delete_selection_no_history();
        text_.insert(caret_, utf8);
        caret_ = snap(caret_ + utf8.size());
        anchor_ = caret_;
    }

    void TextBuffer::backspace()
    {
        composition_cancel();
        sticky_col_ = -1;
        if (has_selection())
        {
            record(EditKind::Discrete, time_);
            delete_selection_no_history();
            return;
        }
        if (caret_ == 0) return;

        record(EditKind::Backspace, time_);
        // 整个码点一起删，不是只删一个字节。
        const size_t start = prev_boundary(text_, caret_);
        text_.erase(start, caret_ - start);
        caret_ = anchor_ = start;
    }

    void TextBuffer::del()
    {
        composition_cancel();
        sticky_col_ = -1;
        if (has_selection())
        {
            record(EditKind::Discrete, time_);
            delete_selection_no_history();
            return;
        }
        if (caret_ >= text_.size()) return;
        record(EditKind::Delete, time_);
        const size_t n = utf8_step((const unsigned char *)text_.data() + caret_,
                                   text_.size() - caret_);
        text_.erase(caret_, n);
        anchor_ = caret_;
    }

    // ── 行 ────────────────────────────────────────────────────────

    size_t TextBuffer::line_count() const
    {
        size_t n = 1;
        for (size_t i = 0; i < text_.size(); ++i)
            if (text_[i] == '\n') ++n;
        return n;
    }

    size_t TextBuffer::line_start(size_t line) const
    {
        if (line == 0) return 0;
        size_t seen = 0;
        for (size_t i = 0; i < text_.size(); ++i)
        {
            if (text_[i] != '\n') continue;
            if (++seen == line) return i + 1;
        }
        // 超过最后一行。line_start(line_count()-1) 是合法的，等于 size()；
        // 再多要一行是调用方的错误，夹到 size() 就无害了。
        return text_.size();
    }

    size_t TextBuffer::line_end(size_t line) const
    {
        const size_t start = line_start(line);
        for (size_t i = start; i < text_.size(); ++i)
        {
            if (text_[i] == '\n') return i;
        }
        return text_.size();
    }

    size_t TextBuffer::line_of(size_t byte) const
    {
        if (byte > text_.size()) byte = text_.size();
        size_t line = 0;
        for (size_t i = 0; i < byte && i < text_.size(); ++i)
            if (text_[i] == '\n') ++line;
        return line;
    }

    void TextBuffer::move_line(int delta, bool extend)
    {
        composition_cancel();

        const size_t here = line_of(caret_);

        // 第一次垂直移动时确定期望列，连续垂直移动时保持它。`caret_ - line_start`
        // 是字节偏移，只有纯 ASCII 行才等于列号——所以这里存字节偏移，目标行
        // 再对它重新对齐。
        if (sticky_col_ < 0) sticky_col_ = (int)(caret_ - line_start(here));

        const size_t lines = line_count();
        // 夹取而不是回绕。从最后一行绕到第一行不是任何文本框的行为，而且按
        // 一次键光标会跳过整个文档。
        size_t target = here + (size_t)std::max(delta, 0);
        if (delta < 0)
            target = (size_t)std::max((long long)here + delta, 0LL);
        if (target >= lines) target = lines - 1;

        const size_t start = line_start(target);
        const size_t end = line_end(target);
        size_t want = start + (size_t)sticky_col_;

        if (want > end) want = end;
        caret_ = snap(want);
        // snap() 可能把光标拉回码点边界，这是正确的；再夹一次是因为 snap 只会
        // 往左移。
        if (caret_ > end) caret_ = end;

        if (!extend) anchor_ = caret_;
    }

    // ── 导航 ─────────────────────────────────────────────────────────

    void TextBuffer::move_codepoint(int delta, bool extend)
    {
        composition_cancel();
        // 水平移动会重新确定后续垂直移动的列位置。保留旧值的话，用户按 Home
        // 后光标还会粘在第 40 列。
        sticky_col_ = -1;
        if (delta < 0)
        {
            if (caret_ == 0) return;
            caret_ = prev_boundary(text_, caret_);
        }
        else if (delta > 0)
        {
            if (caret_ >= text_.size()) return;
            caret_ += utf8_step((const unsigned char *)text_.data() + caret_,
                                text_.size() - caret_);
        }
        // extend -> 锚点不动，选区变大；否则选区收起。所有导航入口统一如此。
        if (!extend) anchor_ = caret_;
    }

    void TextBuffer::move_to_line_edge(bool extend, bool forward)
    {
        composition_cancel();
        sticky_col_ = -1;
        const size_t n = text_.size();
        if (forward)
        {
            while (caret_ < n && text_[caret_] != '\n') ++caret_;
        }
        else
        {
            if (caret_ == 0) return;
            size_t i = caret_ - 1;
            // 先从换行符上退开，这样在行首按 Home 是无操作，而不是跳到上一行
            // 的行尾。
            if (text_[i] == '\n') --i;
            while (i > 0 && text_[i - 1] != '\n') --i;
            caret_ = i;
        }
        if (!extend) anchor_ = caret_;
    }

    void TextBuffer::move_home(bool extend)
    {
        move_to_line_edge(extend, false);
    }

    void TextBuffer::move_end(bool extend)
    {
        move_to_line_edge(extend, true);
    }

    void TextBuffer::move_word(int direction, bool extend)
    {
        composition_cancel();
        sticky_col_ = -1;
        const size_t n = text_.size();
        auto cls = [&](unsigned char c) -> int {
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') return 0;
            if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))
                return 1;
            return 2;
        };
        auto step_fwd = [&](size_t i) {
            return i + utf8_step((const unsigned char *)text_.data() + i, n - i);
        };

        // 两个阶段：先跨过空白串，再跨过同一类字符的串。两个方向对称，向前
        // 落在下一个词的末尾，向后落在上一个词的开头——和所有原生文本框一致。
        size_t i = caret_;

        if (direction > 0)
        {
            while (i < n && cls((unsigned char)text_[i]) == 0) i = step_fwd(i);
            if (i < n)
            {
                const int c = cls((unsigned char)text_[i]);
                while (i < n && cls((unsigned char)text_[i]) == c) i = step_fwd(i);
            }
        }
        else
        {
            // ⚠ 反向扫描必须每次迭代都重新算 `s`，并无条件执行 i = s。早先的
            //   版本在第一次迭代时确定类别，之后只在"同类"分支上步进——于是
            //   对单字节字符 s == i，`i` 永远不减小，循环停不下来。在 ASCII
            //   词上按词左移会卡死程序，这是最糟糕的发现方式：它需要用户去按
            //   那个键，测试碰不到。
            while (i > 0)
            {
                const size_t s = prev_boundary(text_, i);
                if (cls((unsigned char)text_[s]) != 0) break;
                i = s;
            }
            if (i > 0)
            {
                const int c = cls((unsigned char)text_[prev_boundary(text_, i)]);
                while (i > 0)
                {
                    const size_t s = prev_boundary(text_, i);
                    if (cls((unsigned char)text_[s]) != c) break;
                    i = s;
                }
            }
        }

        caret_ = snap(i);
        if (!extend) anchor_ = caret_;
    }

} // namespace fox
