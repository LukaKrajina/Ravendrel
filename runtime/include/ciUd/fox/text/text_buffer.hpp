#pragma once

#include "core/types.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace fox
{
    struct TextEditState
    {
        std::string text;
        size_t caret = 0;
        size_t anchor = 0;
    };

    class TextBuffer
    {
    public:

        void set_text(std::string_view s);
        void set_text(const char *utf8, size_t len);

        void clear();

        const std::string &text() const { return text_; }
        size_t size() const { return text_.size(); }
        bool empty() const { return text_.empty(); }

        const char *c_str() const { return text_.c_str(); }


        size_t caret() const { return caret_; }
        size_t anchor() const { return anchor_; }
        bool has_selection() const { return caret_ != anchor_; }

        size_t sel_min() const { return caret_ < anchor_ ? caret_ : anchor_; }
        size_t sel_max() const { return caret_ < anchor_ ? anchor_ : caret_; }

        void set_caret(size_t byte);

        void select_to(size_t byte);

        void set_anchor(size_t byte) { anchor_ = snap(byte); }
        void clear_selection() { anchor_ = caret_; }
        void select_all();

        std::string_view selected() const;


        void insert(std::string_view utf8);

        bool delete_selection();

        void backspace();

        void del();


        void move_codepoint(int delta, bool extend);
        void move_home(bool extend);
        void move_end(bool extend);
        void move_word(int direction, bool extend);


        size_t line_count() const;

        size_t line_start(size_t line) const;

        size_t line_end(size_t line) const;

        size_t line_of(size_t byte) const;

        void move_line(int delta, bool extend);


        void composition_begin(std::string_view utf8, size_t caret_bytes = (size_t)-1);

        void composition_update(std::string_view utf8, size_t caret_bytes = (size_t)-1);

        void set_preedit_caret(size_t byte);

        bool composition_commit();

        void composition_cancel();

        bool has_composition() const { return !preedit_.empty(); }
        const std::string &preedit() const { return preedit_; }

        size_t preedit_caret() const { return preedit_caret_; }

        void cancel_composition_if_any() { composition_cancel(); }


        bool can_undo() const { return !undo_.empty(); }
        bool can_redo() const { return !redo_.empty(); }
        void undo();
        void redo();

        void clear_history();

        void set_coalesce_seconds(f32 s) { coalesce_seconds_ = s; }

        f32 last_time() const { return last_time_; }

        void advance_time(f32 now)
        {
            time_ = now;
            last_time_ = now;
        }

        void break_coalescing() { coalescing_ = false; }

    private:
        size_t snap(size_t byte, bool allow_past_end = true) const;

        bool delete_selection_no_history();

        enum class EditKind
        {
            Insert,
            Backspace,
            Delete,
            Discrete
        };
        void record(EditKind kind, f32 now);

        void move_to_line_edge(bool extend, bool forward);

        static constexpr size_t kMaxHistory = 200;

        std::string text_;
        size_t caret_ = 0;
        size_t anchor_ = 0;

        std::string preedit_;
        size_t preedit_caret_ = 0;

        std::vector<TextEditState> undo_;
        std::vector<TextEditState> redo_;

        EditKind last_kind_ = EditKind::Discrete;
        bool coalescing_ = false;
        f32 last_edit_time_ = -1e30f;
        f32 coalesce_seconds_ = 0.6f;
        f32 time_ = 0.0f;
        f32 last_time_ = -1e30f;

        int sticky_col_ = -1;
    };

}