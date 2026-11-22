#include "text_buffer.hpp"

#include <cstring>

namespace fox
{

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
            return 1;
        return n <= avail ? n : 1;
    }

    static bool is_continuation(unsigned char c) { return (c & 0xC0) == 0x80; }

    static size_t prev_boundary(const std::string &s, size_t i)
    {
        if (i == 0) return 0;
        size_t j = i - 1;
        while (j > 0 && is_continuation((unsigned char)s[j])) --j;
        return j;
    }

    size_t TextBuffer::snap(size_t byte, bool allow_past_end) const
    {
        if (!allow_past_end && byte > text_.size()) byte = text_.size();
        if (byte > text_.size()) byte = text_.size();

        for (int k = 0; k < 4 && byte > 0 && byte < text_.size(); ++k)
        {
            if (!is_continuation((unsigned char)text_[byte])) return byte;
            --byte;
        }
        return byte;
    }


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
        anchor_ = caret_;
        sticky_col_ = -1;
    }

    void TextBuffer::select_to(size_t byte)
    {
        composition_cancel();
        caret_ = snap(byte);
    }


    void TextBuffer::composition_begin(std::string_view utf8, size_t caret_bytes)
    {
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
        redo_.push_back(TextEditState{text_, caret_, anchor_});
        if (redo_.size() > kMaxHistory) redo_.erase(redo_.begin());

        const TextEditState &s = undo_.back();
        text_ = s.text;
        caret_ = snap(s.caret);
        anchor_ = snap(s.anchor);

        undo_.pop_back();
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
        composition_cancel();
        sticky_col_ = -1;

        const EditKind kind = utf8.size() == 1 ? EditKind::Insert : EditKind::Discrete;

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

        if (sticky_col_ < 0) sticky_col_ = (int)(caret_ - line_start(here));

        const size_t lines = line_count();
        size_t target = here + (size_t)std::max(delta, 0);
        if (delta < 0)
            target = (size_t)std::max((long long)here + delta, 0LL);
        if (target >= lines) target = lines - 1;

        const size_t start = line_start(target);
        const size_t end = line_end(target);
        size_t want = start + (size_t)sticky_col_;

        if (want > end) want = end;
        caret_ = snap(want);
        if (caret_ > end) caret_ = end;

        if (!extend) anchor_ = caret_;
    }


    void TextBuffer::move_codepoint(int delta, bool extend)
    {
        composition_cancel();
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

}