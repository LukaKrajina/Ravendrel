// Fox text/text_buffer 测试——纯逻辑，无窗口、无 GPU、无驱动。
//
// 为什么单独一个二进制：TextBuffer 是选区、撤销、剪贴板共同的地基，而它
// 的难点与渲染无关——让光标不落在多字节码点内部，以及把连续打字合并成
// 一步撤销而不是一字一步。这类 bug 在屏幕上看着没问题（"光标位置有点
// 怪"），趁你不注意把文本弄坏。
//
// 和 fox_headless 一样，它只需要 C++ 运行时，所以能在 CI、容器里跑，
// 将来也能在 QuarkOS 上跑。

#include "text/text_buffer.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

using namespace fox;

namespace
{
    int g_fail = 0;
    int g_checks = 0;

    /// `detail` 是 std::string，不是 const char*。
    ///
    /// 它以前是 const char*，调用方用 `(std::string(...) + ...).c_str()`
    /// 构造消息。那个临时对象在完整表达式结束时销毁，整个调用期间 `detail`
    /// 都指向已释放的内存——而普通构建里 68 项检查照样全过，因为释放的
    /// 内存碰巧还留着正确的字节。ASan 把那次读报成 global-buffer-overflow。
    ///
    /// 会无意中读到已释放内存的测试框架不是测试框架，只是让人感觉更安全
    /// 而已。
    void check(bool ok, const std::string &what, const std::string &detail = std::string())
    {
        ++g_checks;
        if (!ok)
        {
            ++g_fail;
            std::printf("  FAIL %s%s%s\n", what.c_str(), detail.empty() ? "" : " -- ",
                        detail.c_str());
        }
    }

    void eq_str(const TextBuffer &b, const char *want, const char *what)
    {
        const bool ok = b.text() == want;
        std::string detail;
        if (!ok) detail = std::string("got \"") + b.text() + "\" want \"" + want + "\"";
        check(ok, what, detail);
    }

    /// 把"缓冲为空"单独立为断言。
    ///
    /// 它存在是为了让测试绝不传入零长度字符串字面量。MSVC 生成的 `""`
    /// 可寻址字节数为零（其 ASan 影子区是全局 redzone），对它读任何一个
    /// 字节都会出错——包括本文件开头那个完全合理的 `want[0] == '\0'`
    /// 守卫。读不了的字面量不该在比较里绕过，而应该停止使用。
    void eq_empty(const TextBuffer &b, const char *what)
    {
        const bool ok = b.text().empty();
        std::string detail;
        if (!ok) detail = std::string("got \"") + b.text() + "\", expected empty";
        check(ok, what, detail);
    }

    void eq_size(size_t got, size_t want, const char *what)
    {
        const bool ok = got == want;
        char buf[128];
        std::snprintf(buf, sizeof buf, "got %zu want %zu", got, want);
        check(ok, what, ok ? std::string() : std::string(buf));
    }

    void eq_bool(bool got, bool want, const char *what)
    {
        const bool ok = got == want;
        check(ok, what,
              ok ? std::string() : std::string(want ? "expected true" : "expected false"));
    }

    // ── UTF-8 ──────────────────────────────────────────────────────────
    //
    // "a你b" 是 1 + 3 + 1 = 5 字节。下面每个测试真正针对的都是中间那个
    // 三字节字符。
    const char *kMixed = "a\xe4\xbd\xa0" "b"; // a + U+4F60 + b
    const size_t kMixedLen = 5;

    void test_caret_never_splits_a_codepoint()
    {
        TextBuffer b;
        b.set_text(kMixed, kMixedLen);
        eq_str(b, kMixed, "set_text stores bytes verbatim");

        // 字节 2 和 3 在三字节码点内部。光标落在那里意味着下一次编辑会改掉
        // 半个字符，很久之后才在用户文本里表现为 U+FFFD。
        b.set_caret(2);
        eq_size(b.caret(), 1, "caret at byte 2 snaps back to 1");
        b.set_caret(3);
        eq_size(b.caret(), 1, "caret at byte 3 snaps back to 1");
        b.set_caret(4);
        eq_size(b.caret(), 4, "caret at byte 4 (codepoint boundary) stays");

        b.set_caret(99);
        eq_size(b.caret(), kMixedLen, "caret past the end clamps");
    }

    void test_movement_is_by_codepoint()
    {
        TextBuffer b;
        b.set_text(kMixed, kMixedLen);
        b.set_caret(0);

        b.move_codepoint(1, false);
        eq_size(b.caret(), 1, "right 1 lands on byte 1");
        b.move_codepoint(1, false);
        eq_size(b.caret(), 4, "right 2 SKIPS the whole 3-byte codepoint");
        b.move_codepoint(1, false);
        eq_size(b.caret(), 5, "right 3 reaches the end");
        b.move_codepoint(1, false);
        eq_size(b.caret(), 5, "right at the end is a no-op");

        b.move_codepoint(-1, false);
        eq_size(b.caret(), 4, "left from end lands on byte 4");
        b.move_codepoint(-1, false);
        eq_size(b.caret(), 1, "left again SKIPS the whole codepoint");
        b.move_codepoint(-1, false);
        eq_size(b.caret(), 0, "left reaches the start");
        b.move_codepoint(-1, false);
        eq_size(b.caret(), 0, "left at the start is a no-op");
    }

    void test_backspace_removes_a_whole_codepoint()
    {
        TextBuffer b;
        b.set_text(kMixed, kMixedLen);
        b.set_caret(4); // 紧跟 U+4F60 之后
        b.backspace();
        eq_str(b, "ab", "backspace removed the whole CJK char, not 1 byte");
        eq_size(b.caret(), 1, "caret moved to the char's start");

        // 防止的失败：从三字节序列里删掉一个字节会留下孤立的续字节，
        // 文本就这么坏了。
    }

    void test_delete_is_by_codepoint()
    {
        TextBuffer b;
        b.set_text(kMixed, kMixedLen);
        b.set_caret(1); // 紧挨 U+4F60 之前
        b.del();
        eq_str(b, "ab", "delete removed the whole CJK char");
    }

    void test_malformed_bytes_terminate()
    {
        // 孤立的续字节和被截断的序列，正是宿主或剪贴板会送来的那种。
        // 解码必须始终有进展：后退一步却没动就是死循环，这种输入最容易
        // 触发它。
        const char junk[] = "\x80\x80\xBF\xE4\xBD"; // 5 个坏字节
        TextBuffer b;
        b.set_text(junk, 5);

        // 断言的是会终止，不是每个字节都消失。从末尾退格会整段剥掉码点
        // 形状的片段；剩下的卡在光标处，要用 Delete 清，因为光标退不到
        // 位置 0 之前。
        for (int i = 0; i < 50; ++i)
        {
            b.move_codepoint(-1, false);
            b.backspace();
        }
        check(b.size() < 5, "backspacing through malformed bytes made progress");
        check(b.caret() <= b.size(), "caret never exceeds the buffer");

        for (int i = 0; i < 50; ++i) b.del();
        eq_size(b.size(), 0, "Delete finishes the job and terminates");
    }

    // ── 选区 ──────────────────────────────────────────────────────────

    void test_selection()
    {
        TextBuffer b;
        b.set_text("hello world", 11);

        b.set_caret(0);
        b.select_to(5);
        eq_bool(b.has_selection(), true, "select_to creates a selection");
        eq_size(b.sel_min(), 0, "sel_min");
        eq_size(b.sel_max(), 5, "sel_max");
        eq_str(b, "hello world", "selecting does not mutate");
        check(b.selected() == "hello", "selected() returns the range");

        // 普通光标移动会收起选区；扩展移动不会。
        b.move_codepoint(1, false);
        eq_bool(b.has_selection(), false, "plain move collapses the selection");
        b.set_caret(0);
        b.select_to(5);
        b.move_codepoint(1, true);
        eq_bool(b.has_selection(), true, "extend keeps the selection");
        eq_size(b.sel_min(), 0, "extend keeps the anchor put");

        // 反向选区：锚点在光标之后。
        b.set_anchor(5);
        b.set_caret(0);
        b.select_to(0);
        b.set_anchor(5);
        b.move_codepoint(-1, true);
        eq_size(b.sel_min(), 0, "backwards selection still reports min");
        eq_size(b.sel_max(), 5, "backwards selection still reports max");
    }

    void test_delete_selection()
    {
        TextBuffer b;
        b.set_text("hello world", 11);
        b.set_caret(0);
        b.select_to(6); // "hello "
        check(b.delete_selection(), "delete_selection reports it did something");
        eq_str(b, "world", "selection removed");
        eq_size(b.caret(), 0, "caret at the deletion point");

        b.set_caret(2);
        eq_bool(b.delete_selection(), false, "delete with no selection is a no-op");
    }

    void test_insert_replaces_selection_in_one_step()
    {
        TextBuffer b;
        b.set_text("hello world", 11);
        b.set_caret(0);
        b.select_to(6);
        b.insert("goodbye ");

        eq_str(b, "goodbye world", "insert replaced the selection");

        // 一次用户操作、一步撤销——而且选区必须一起回来，不只是文本。
        // 这就是 insert() 在删除之前先记录的原因。
        b.undo();
        eq_str(b, "hello world", "one undo restores the pre-insert text");
        b.redo();
        eq_str(b, "goodbye world", "redo reapplies it");
    }

    // ── 撤销 ───────────────────────────────────────────────────────────

    void test_typing_coalesces_into_one_step()
    {
        TextBuffer b;
        b.clear();
        b.advance_time(0.0f);
        b.set_coalesce_seconds(0.6f);

        for (int i = 0; i < 5; ++i)
        {
            b.advance_time(0.05f * (float)i); // 相隔 50 毫秒：一段连续打字
            b.insert("x");
        }
        eq_str(b, "xxxxx", "five inserts produced five chars");

        b.undo();
        eq_empty(b, "a typing burst collapses to ONE undo step");

        // 这正是重点："撤销那个词"，而不是"撤销一个字母"。
        check(!b.can_undo(), "and there is nothing left to undo");
    }

    void test_typing_slowly_is_separate_steps()
    {
        TextBuffer b;
        b.clear();
        b.advance_time(0.0f);
        b.set_coalesce_seconds(0.6f);

        for (int i = 0; i < 3; ++i)
        {
            b.advance_time(2.0f * (float)i); // 相隔 2 秒：各自独立的编辑
            b.insert("x");
        }
        eq_str(b, "xxx", "three inserts");

        b.undo();
        eq_str(b, "xx", "undo 1 of 3");
        b.undo();
        eq_str(b, "x", "undo 2 of 3");
        b.undo();
        eq_empty(b, "undo 3 of 3");
        check(!b.can_undo(), "history exhausted");
    }

    void test_backspace_coalesces_but_delete_does_not()
    {
        TextBuffer b;
        b.set_text("abcdef", 6);
        b.advance_time(0.0f);
        b.set_caret(6);

        b.advance_time(0.05f);
        b.backspace();
        b.advance_time(0.10f);
        b.backspace();
        b.advance_time(0.15f);
        b.backspace();
        eq_str(b, "abc", "three backspaces");
        b.undo();
        eq_str(b, "abcdef", "a backspace run is one step");

        TextBuffer d;
        d.set_text("abcdef", 6);
        d.advance_time(0.0f);
        d.set_caret(0);
        d.advance_time(0.05f);
        d.del();
        d.advance_time(0.10f);
        d.del();
        eq_str(d, "cdef", "two deletes");
        d.undo();
        eq_str(d, "bcdef", "Delete does NOT coalesce - each press is its own step");
    }

    void test_paste_does_not_coalesce_with_typing()
    {
        // 防止的 bug：200 字符的粘贴紧跟在一个敲入的字母后面，Ctrl+Z 会把
        // 粘贴和那个字母一起删掉——因为它们并成了同一条历史记录。
        TextBuffer b;
        b.clear();
        b.advance_time(0.0f);

        b.advance_time(0.01f);
        b.insert("a");
        b.advance_time(0.02f);
        b.insert("pasted text");

        eq_str(b, "apasted text", "typed then pasted");
        b.undo();
        eq_str(b, "a", "undo removed ONLY the paste, not the typed letter");
        b.undo();
        eq_empty(b, "second undo removed the letter");
    }

    void test_navigation_does_not_create_history()
    {
        TextBuffer b;
        b.set_text("hello world", 11);
        b.advance_time(0.0f);
        b.set_caret(0);
        for (int i = 0; i < 5; ++i) b.move_codepoint(1, false);
        check(!b.can_undo(), "moving the caret is not an edit");
    }

    void test_undo_restores_selection()
    {
        TextBuffer b;
        b.set_text("hello world", 11);
        b.advance_time(0.0f);

        b.set_caret(0);
        b.select_to(5);
        b.advance_time(0.05f);
        b.insert("X");

        b.undo();
        eq_str(b, "hello world", "text restored");
        eq_size(b.anchor(), 0, "anchor restored");
        eq_size(b.caret(), 5, "caret restored");
        eq_bool(b.has_selection(), true, "the selection came back with it");
    }

    void test_set_text_clears_history()
    {
        // 宿主写 buffer 是一个新值，不是用户敲出来的。把它做成可撤销
        // 毫无道理。
        TextBuffer b;
        b.set_text("abc", 3);
        b.advance_time(0.0f);
        b.set_caret(3);
        b.insert("d");
        check(b.can_undo(), "there is an edit to undo");

        b.set_text("zzz", 3);
        check(!b.can_undo(), "set_text dropped the history");
        check(!b.can_redo(), "set_text dropped the redo stack too");
    }

    void test_home_end_and_words()
    {
        TextBuffer b;
        b.set_text("alpha beta\ngamma delta", 22);

        // "alpha beta\ngamma delta" -- indices: a0 l1 p2 h3 a4 ' '5 b6 e7 t8 a9
        // '\n'10 g11 ...
        b.set_caret(0);
        b.move_end(false);
        eq_size(b.caret(), 10, "End goes to the newline, i.e. end of line 1");

        b.move_codepoint(-1, false);
        b.move_home(false);
        eq_size(b.caret(), 0, "Home goes to the start of the line");

        // 约定，双向对称：按词右移先跨空白再跨词，落在下一个词的末尾；
        // 按词左移落在前一个词的开头。
        b.set_caret(0);
        b.move_word(1, false);
        eq_size(b.caret(), 5, "word-right from 0 lands on the end of 'alpha'");
        b.move_word(1, false);
        eq_size(b.caret(), 10, "word-right again lands on the end of 'beta'");
        b.move_word(-1, false);
        eq_size(b.caret(), 6, "word-left lands on the start of 'beta'");
        b.move_word(-1, false);
        eq_size(b.caret(), 0, "word-left again reaches the start of 'alpha'");
        b.move_word(-1, false);
        eq_size(b.caret(), 0, "word-left at the start is a no-op (used to hang)");

        // 跨越换行不能失控。
        b.set_caret(10);
        b.move_word(1, false);
        eq_size(b.caret(), 16, "word-right crosses the newline into 'gamma'");
    }

    void test_history_is_bounded()
    {
        TextBuffer b;
        b.clear();
        b.advance_time(0.0f);
        b.set_coalesce_seconds(0.0f); // 每次插入各占一步

        for (int i = 0; i < 500; ++i)
        {
            b.advance_time((float)i); // 每次相隔一秒
            b.insert("x");
        }
        // kMaxHistory 是 200。全部撤销必须能终止，且不能无上限增长。
        int steps = 0;
        while (b.can_undo() && steps < 1000)
        {
            b.undo();
            ++steps;
        }
        check(steps <= 200, "history is capped", "steps");
        std::printf("  history cap: %d undo steps available from 500 edits\n", steps);
    }
    // ── IME 输入组合 ────────────────────────────────────────────────
    //
    // 用户提交之前，预编辑不是文档的一部分。这里每一项都在断言这种分离，
    // 因为失效方式看不出来：文本看着是对的，Ctrl+Z 却撤销了错的东西。

    void test_composition_is_not_in_the_document()
    {
        TextBuffer b;
        b.set_text("hello", 5);
        b.set_caret(5);

        b.composition_begin("ni", 2);
        eq_bool(b.has_composition(), true, "composition is open");
        eq_str(b, "hello", "the DOCUMENT is untouched while composing");
        eq_str(b, "hello", "text() still has no pinyin in it");
        check(b.preedit() == "ni", "the composing text is held separately");

        b.composition_update("nihao", 5);
        eq_str(b, "hello", "still untouched after an update");
        check(b.preedit() == "nihao", "preedit tracks the update");
    }

    void test_commit_inserts_and_is_one_undo_step()
    {
        TextBuffer b;
        b.set_text("hello", 5);
        b.set_caret(5);
        b.advance_time(0.0f);

        b.composition_begin("nihao", 5);
        b.composition_commit();
        eq_str(b, "hellonihao", "the committed candidate is inserted");
        eq_bool(b.has_composition(), false, "composition is closed after commit");

        // 重点就在这：一次 Ctrl+Z 去掉整个候选词。逐字撤销不是任何人
        // 理解的撤销。
        b.undo();
        eq_str(b, "hello", "ONE undo removed the entire committed candidate");
        b.redo();
        eq_str(b, "hellonihao", "redo brings the whole candidate back");
    }

    void test_commit_replaces_selection_as_one_step()
    {
        TextBuffer b;
        b.set_text("hello world", 11);
        b.advance_time(0.0f);

        b.set_caret(0);
        b.select_to(6); // "hello "
        b.composition_begin("\xE4\xBD\xA0\xE5\xA5\xBD", 6); // 你好
        b.composition_commit();

        eq_str(b, "\xE4\xBD\xA0\xE5\xA5\xBD" "world", "commit replaced the selection");

        b.undo();
        eq_str(b, "hello world", "one undo restores BOTH the text and the selection");
        eq_bool(b.has_selection(), true, "the selection came back too");
    }

    void test_cancel_inserts_nothing()
    {
        TextBuffer b;
        b.set_text("hello", 5);
        b.set_caret(5);
        b.advance_time(0.0f);

        b.composition_begin("nihao", 5);
        b.composition_cancel();

        eq_str(b, "hello", "cancelled composition inserted nothing");
        eq_bool(b.has_composition(), false, "composition is closed");
        check(!b.can_undo(), "a cancelled composition leaves NO undo entry - "
                             "there is nothing for the user to undo, because "
                             "nothing ever happened");
    }

    void test_other_edits_supersede_a_composition()
    {
        // IME 合成期间打字、移动或撤销，说明用户已经做别的了。留着预编辑
        // 不放，它会锚在一个已经不存在的光标上。
        {
            TextBuffer b;
            b.composition_begin("ni", 2);
            b.insert("x");
            eq_bool(b.has_composition(), false, "insert cancelled the composition");
        }
        {
            TextBuffer b;
            b.set_text("abc", 3);
            b.composition_begin("ni", 2);
            b.move_codepoint(1, false);
            eq_bool(b.has_composition(), false, "navigation cancelled the composition");
        }
        {
            TextBuffer b;
            b.set_text("abc", 3);
            b.set_caret(3);
            b.insert("d");
            b.composition_begin("ni", 2);
            b.undo();
            eq_bool(b.has_composition(), false, "undo cancelled the composition");
        }
        {
            TextBuffer b;
            b.set_text("abc", 3);
            b.set_caret(3);
            b.composition_begin("ni", 2);
            b.backspace();
            eq_bool(b.has_composition(), false, "backspace cancelled the composition");
            eq_str(b, "ab", "and the backspace still did its job");
        }
    }

    void test_restarting_a_composition_replaces_it()
    {
        // 合成中再敲一个拼音字母会替换整个输入组合。这是正常行为，不是
        // 异常。
        TextBuffer b;
        b.composition_begin("n", 1);
        b.composition_begin("ni", 2);
        b.composition_begin("nih", 3);
        check(b.preedit() == "nih", "the newest composition wins");
        b.composition_commit();
        eq_str(b, "nih", "only the final composition is committed");
    }

    void test_preedit_caret_is_clamped()
    {
        // 这个偏移是宿主按它自己理解的合成文本算的。盲目信任会索引到我们
        // 存的内容之外，画光标的地方就会读到不属于它的内存。
        TextBuffer b;
        b.composition_begin("ni", 9999);
        eq_size(b.preedit_caret(), 2, "an out-of-range preedit caret clamps to the length");

        b.set_preedit_caret(1);
        eq_size(b.preedit_caret(), 1, "an in-range preedit caret is kept");

        // 再来一个多字节合成，那才是真实情形。
        b.composition_begin("\xE4\xBD\xA0\xE5\xA5\xBD", 99);
        eq_size(b.preedit_caret(), 6, "clamps against BYTES, not codepoints");
    }

    // ── 行 ─────────────────────────────────────────────────────────

    void test_line_queries()
    {
        // "aa\nbbbb\ncc" -> 3 行
        TextBuffer b;
        b.set_text("aa\nbbbb\ncc", 9);
        eq_size(b.line_count(), 3, "three lines");
        eq_size(b.line_start(0), 0, "line 0 starts at 0");
        eq_size(b.line_start(1), 3, "line 1 starts after the first newline");
        eq_size(b.line_start(2), 8, "line 2 starts after the second");
        eq_size(b.line_end(0), 2, "line 0 ends BEFORE its newline");
        eq_size(b.line_end(1), 7, "line 1 ends before its newline");
        eq_size(b.line_end(2), 9, "the last line ends at size()");

        eq_size(b.line_of(0), 0, "byte 0 is line 0");
        eq_size(b.line_of(2), 0, "the newline itself belongs to the line it ends");
        eq_size(b.line_of(3), 1, "byte 3 is line 1");
        eq_size(b.line_of(9999), 2, "an out-of-range byte clamps to the last line");

        // 末尾换行会开启最后一个空行，因为用户按回车后光标就停在那。
        // 把它算进去，"行数"才和画出来的一致。
        b.set_text("aa\n", 3);
        eq_size(b.line_count(), 2, "a trailing newline starts an empty final line");
        eq_size(b.line_start(1), 3, "that empty line starts at size()");
        eq_size(b.line_end(1), 3, "and is empty");

        b.clear();
        eq_size(b.line_count(), 1, "an empty buffer is one (empty) line, not zero");
    }

    void test_move_line_preserves_column()
    {
        // 粘性列就是这个特性。从长行下移到短行再移回来，必须回到起点——
        // 所有编辑器都这么干，竖向移动这才好用。
        //
        //   "abcdefgh\nxy\nabcdefgh"
        //    0.......7 8 9 10 11 12........19
        //   line0 = 0..8   line1 = 9..11 ("xy")   line2 = 12..20
        TextBuffer b;
        b.set_text("abcdefgh\nxy\nabcdefgh", 20);
        b.set_caret(5); // 第 0 行第 5 列

        b.move_line(1, false);
        eq_size(b.caret(), 11, "down onto the SHORT line clamps to its end (9+5=14 -> 11)");
        b.move_line(1, false);
        eq_size(b.caret(), 17, "down again restores column 5 on the long line (12+5)");
        b.move_line(-1, false);
        eq_size(b.caret(), 11, "up onto the short line clamps to its end again");
        b.move_line(-1, false);
        eq_size(b.caret(), 5, "up again restores column 5 - the round trip works");
    }

    void test_move_line_clamps_at_document_edges()
    {
        TextBuffer b;
        b.set_text("aa\nbb", 5);
        b.set_caret(0);
        b.move_line(-1, false);
        eq_size(b.caret(), 0, "up on the first line is a no-op, NOT a wrap");
        b.set_caret(4);
        b.move_line(1, false);
        eq_size(b.caret(), 4, "down on the last line is a no-op, NOT a wrap");
    }

    void test_home_and_end_are_line_relative()
    {
        TextBuffer b;
        b.set_text("alpha\nbeta\ngamma", 16);
        b.set_caret(8); // inside "beta"
        b.move_home(false);
        eq_size(b.caret(), 6, "Home goes to the start of THIS line");
        b.move_end(false);
        eq_size(b.caret(), 10, "End goes to the end of THIS line");
    }

    void test_backspace_at_line_start_joins_lines()
    {
        TextBuffer b;
        b.set_text("ab\ncd", 5);
        b.set_caret(3); // start of line 1
        b.backspace();
        eq_str(b, "abcd", "backspace at a line start JOINS the lines");
        eq_size(b.caret(), 2, "the caret lands where the newline was");
    }

    void test_vertical_movement_resets_on_horizontal()
    {
        // 粘性列不能在水平移动后还留着。留着的话，用户按 Home 后光标还会
        // 粘在第 5 列——没人想要这种行为，`sticky_col_` 被重置就是为此。
        TextBuffer b;
        b.set_text("abcdefgh\nabcdefgh\nabcdefgh", 26);
        b.set_caret(5); // 第 0 行第 5 列

        b.move_line(1, false);
        eq_size(b.caret(), 14, "down preserved column 5");

        b.move_home(false);
        eq_size(b.caret(), 9, "Home moved to the start of line 1");

        // 列现在是 0。如果旧的第 5 列被带过来，这里会是 23；落在 18 才是
        // 要点。
        b.move_line(1, false);
        eq_size(b.caret(), 18, "vertical movement uses the NEW column, not the stale one");
    }

} // namespace

int main()
{
    // stdout 重定向后是块缓冲的，卡住时会吞掉所有分区标题，毫无线索知道
    // 卡在哪。每节后刷新，才能把"卡住了"变成"卡在按词导航里"。
    auto section = [](const char *name) {
        std::printf("[text_test] %s\n", name);
        std::fflush(stdout);
    };

    section("UTF-8 caret safety");
test_caret_never_splits_a_codepoint();
    test_movement_is_by_codepoint();
    test_backspace_removes_a_whole_codepoint();
    test_delete_is_by_codepoint();
    test_malformed_bytes_terminate();

    section("selection");
    test_selection();
    test_delete_selection();
    test_insert_replaces_selection_in_one_step();

    section("undo");
test_typing_coalesces_into_one_step();
test_typing_slowly_is_separate_steps();
test_backspace_coalesces_but_delete_does_not();
test_paste_does_not_coalesce_with_typing();
test_navigation_does_not_create_history();
test_undo_restores_selection();
test_set_text_clears_history();
test_history_is_bounded();

    section("navigation");
    test_home_end_and_words();

    section("lines");
    test_line_queries();
    test_move_line_preserves_column();
    test_move_line_clamps_at_document_edges();
    test_home_and_end_are_line_relative();
    test_backspace_at_line_start_joins_lines();
    test_vertical_movement_resets_on_horizontal();

    section("IME composition");
    test_composition_is_not_in_the_document();
    test_commit_inserts_and_is_one_undo_step();
    test_commit_replaces_selection_as_one_step();
    test_cancel_inserts_nothing();
    test_other_edits_supersede_a_composition();
    test_restarting_a_composition_replaces_it();
    test_preedit_caret_is_clamped();

    std::printf("\n[text_test] %d checks, %d failures\n", g_checks, g_fail);
    std::fflush(stdout);
    if (g_fail)
    {
        std::printf("[text_test] FAIL\n");
        return 1;
    }
    std::printf("[text_test] OK\n");
    return 0;
}
