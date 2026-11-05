#pragma once
// Fox text/text_buffer.hpp - 编辑模型
//
// 为什么要有这个对象，而不是"直接改调用方的 char*"
// -----------------------------------------------------------------
// 选区、撤销、剪贴板不是三个独立功能，而是同一样东西的三个侧面：一个带
// 光标、锚点和历史记录的缓冲区。想把任何一个硬塞到裸 char* 上，到头来还
// 是得发明这个结构，只是散落在控件各处，历史记录塞在控件状态的空闲字段
// 里。
//
// 所以就做成了这个对象，text_field 是它的一个视图。
//
// UTF-8 是全部难点所在
// -----------------------------
// 这里所有索引都是字节偏移，因为调用方持有的就是字节，但光标绝不能落在
// 多字节序列中间。弄错不会直接报错——它会把码点切开，下一次编辑把后半截
// 替换成 U+FFFD。对 CJK 字符按退格是最容易踩到的方式。所以每次修改前都
// 先对齐到码点边界。
//
// 撤销基于快照，不基于差量
// -------------------------------------
// 差量更省内存，但一个文本框也就几十字节，历史也有上限；快照就几行数据，
// 而且不存在"差量应用错了"这类 bug。这里要的就是每一步都正确可靠。

#include "core/types.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace fox
{
    /// 撤销历史中的一条记录：一次编辑前的完整状态。
    struct TextEditState
    {
        std::string text;
        size_t caret = 0;
        size_t anchor = 0;
    };

    class TextBuffer
    {
    public:
        // ── 内容 ───────────────────────────────────────────────────

        /// 替换全部文本，并清空历史：这是"换了一个值"，不是"用户输入的"，
        /// 所以不能进撤销历史。
        ///
        /// 用于宿主绕过 Fox 直接改缓冲区的场合——这正是它不算编辑的原因。
        void set_text(std::string_view s);
        void set_text(const char *utf8, size_t len);

        /// 清空缓冲区。
        ///
        /// 语义上等同 set_text("")，但不携带任何字符串字面量。这在这个工具链
        /// 上很要紧：零长度字面量实际占一个字节（结尾 NUL），MSVC 带边界检查
        /// 的 char_traits::length 会越过它多读一个字节，于是 ASan 对一个完全
        /// 合法的字面量报 global-buffer-overflow。空字符串场景不用字面量，
        /// sanitizer 就不会被噪音打扰，真出问题时报出来才可信。
        void clear();

        const std::string &text() const { return text_; }
        size_t size() const { return text_.size(); }
        bool empty() const { return text_.empty(); }

        /// 借出以 NUL 结尾的 C 字符串。任何修改都会使该指针失效，这和调用方
        /// 对 `char* buffer` 的既有约定一致。
        const char *c_str() const { return text_.c_str(); }

        // ── 光标与选区 ───────────────────────────────────────────────

        size_t caret() const { return caret_; }
        size_t anchor() const { return anchor_; }
        bool has_selection() const { return caret_ != anchor_; }

        size_t sel_min() const { return caret_ < anchor_ ? caret_ : anchor_; }
        size_t sel_max() const { return caret_ < anchor_ ? anchor_ : caret_; }

        /// 设置光标，对齐到码点边界。不移动锚点——移动光标用它，扩展选区用
        /// `select_to`，让调用方明确区分"移动光标"和"扩展选区"。
        void set_caret(size_t byte);

        /// 移动光标，锚点跟着一起移动。
        void select_to(size_t byte);

        void set_anchor(size_t byte) { anchor_ = snap(byte); }
        void clear_selection() { anchor_ = caret_; }
        void select_all();

        /// 选中的字节；没有选区时为空。
        std::string_view selected() const;

        // ── 编辑 ───────────────────────────────────────────────────

        /// 在光标处插入，替换选区。记入撤销历史。
        void insert(std::string_view utf8);

        /// 删除选区（如果有）。删掉了东西返回 true。
        bool delete_selection();

        /// 删除光标前一个码点，或删除选区。按码点处理：对 CJK 字符退格删掉
        /// 整个字符，而不是它的一个字节。
        void backspace();

        /// 删除光标后一个码点，或删除选区。
        void del();

        // ── 导航 ────────────────────────────────────────────────────

        /// 按码点移动，不按字节——按一次键必须移动一个可见字符，不管它占
        /// 几个字节。
        void move_codepoint(int delta, bool extend);
        void move_home(bool extend);
        void move_end(bool extend);
        void move_word(int direction, bool extend);

        // ── 行 ────────────────────────────────────────────────────
        //
        // 这里"行"只按显式的 '\n' 分隔。有意不建模软换行：如果一条逻辑行可以
        // 占多行显示，"光标所在行"和"光标所在显示行"就不再相等，下面每个查询
        // 都得靠排版引擎回答。保持二者相等，文本区就能报行号、滚到光标、上下
        // 移动光标，而不依赖显示行怎么断——软换行于是成为排版的属性，而不是
        // 编辑模型的属性。

        /// 行数。末尾的 '\n' 会开启最后一行空行，因为用户看到的就是这样：
        /// 光标停在那行上。
        size_t line_count() const;

        /// 第 `line` 行（从 0 开始）的起始字节。超出末尾返回 size()。
        size_t line_start(size_t line) const;

        /// 第 `line` 行最后一个字节之后的位置，不含 '\n'。
        size_t line_end(size_t line) const;

        /// `byte` 所在的行，已夹到有效范围内。
        size_t line_of(size_t byte) const;

        /// 光标上下移动一行，保持期望的列位置。
        ///
        /// 粘滞列就是这个功能的全部意义。没有它，从长行下移到短行再移回来就
        /// 丢了原来的位置——这是没有粘滞列时的标准行为，也是编辑器都保留粘滞
        /// 列的原因。
        void move_line(int delta, bool extend);

        // ── IME 输入组合 ───────────────────────────────────────────────
        //
        // preedit（"组合串"/"预编辑文本"）是 IME 还在组合中的文本——拼音字母、
        // 假名读音、正在预览的候选词。它绝对不是文档的一部分：用户还没确认它，
        // 文档里不能有它，在确认前也不能产生任何撤销记录。
        //
        // 所以它作为独立状态放在这里，有自己的光标，由控件拼进显示，而不是拼
        // 进文本本身。把它挡在 `text_` 之外，"Ctrl+Z 只撤销已上屏的候选词、
        // 别的都不动"才是严格成立的，而不只是大致成立。

        /// IME 开始一次组合。会替换正在进行的组合——用户再敲一个字符时 IME
        /// 重新开始组合是正常行为。
        ///
        /// `caret_bytes` 是 `utf8` 内部的偏移。这里夹取而不直接信任它：宿主是
        /// 按它自己理解的组合串算的，越界值会索引到我们存储内容之外。只有拥有
        /// 该字符串的这一层才知道正确的边界在哪，所以在这里夹取。
        void composition_begin(std::string_view utf8, size_t caret_bytes = (size_t)-1);

        /// 组合文本变了。没有进行中的组合时什么也不做——宿主可能发来一个它已
        /// 经开始取消的更新，这是正常的。
        void composition_update(std::string_view utf8, size_t caret_bytes = (size_t)-1);

        /// 设置组合文本内的光标，夹取到其长度内。跟踪组合内光标的宿主会调用
        /// 它。
        void set_preedit_caret(size_t byte);

        /// 用户选定候选词：preedit 变成正式文本。
        ///
        /// 整个上屏只记一条撤销记录，包括它替换掉的选区。把已上屏的候选词一个
        /// 字一个字地撤销，不符合任何人对"撤销"的理解。
        bool composition_commit();

        /// 组合被放弃（按了 Escape、失去焦点、别的窗口抢走键盘）。丢弃 preedit，
        /// 不动其他任何东西。
        void composition_cancel();

        bool has_composition() const { return !preedit_.empty(); }
        const std::string &preedit() const { return preedit_; }

        /// preedit 内部的光标位置，单位字节。IME 还在"打字"、不让用户在其组合
        /// 文本内移动时，等于 preedit().size()。
        size_t preedit_caret() const { return preedit_caret_; }

        /// 放弃任何进行中的组合。导航、撤销、重做都会调用——它们都会打断 IME
        /// 正在做的事。
        ///
        /// 每个会破坏组合的修改入口都调用它，而不是各自判断——否则总会有某个
        /// 入口悄悄吞掉半个已上屏的候选词。
        void cancel_composition_if_any() { composition_cancel(); }

        // ── 历史 ───────────────────────────────────────────────────

        bool can_undo() const { return !undo_.empty(); }
        bool can_redo() const { return !redo_.empty(); }
        void undo();
        void redo();

        /// 清空全部历史。值被外部整体替换时调用。
        void clear_history();

        /// 合并窗口。间隔小于它的连续单字符编辑合并成一条撤销记录，这就是
        /// Ctrl+Z 撤销起来像"撤销那个词"而不是"撤销一个字母"的原因。
        void set_coalesce_seconds(f32 s) { coalesce_seconds_ = s; }

        /// 此缓冲区最后一次活动的时间，供 Context::prune_state_cache() 使用。
        f32 last_time() const { return last_time_; }

        /// 通知缓冲区时间已推进，用于打断合并、刷新清理水位。
        ///
        /// 每帧对每个有焦点或可见的输入框调用一次。合并本身并不严格依赖它
        /// （edit() 会盖上给定的时间戳），但它能防止一次打字跨越宿主帧循环的
        /// 长暂停后仍被合并，而且它也是向 Context::prune_state_cache() 标记
        /// 缓冲区仍然存活的依据。
        void advance_time(f32 now)
        {
            time_ = now;
            last_time_ = now;
        }

        /// 标记一次独立编辑的开始，它不能与之前的编辑合并——粘贴、按删除键、
        /// 程序性修改。紧接着的 edit() 一定会新开一条历史记录。
        void break_coalescing() { coalescing_ = false; }

    private:
        /// 夹到 [0, size]，并对齐到码点边界。
        size_t snap(size_t byte, bool allow_past_end = true) const;

        /// 删除选区，不碰历史记录。
        ///
        /// insert() 先记录编辑前的状态——包括选区，因为撤销一次吞掉选区的插入
        /// 必须把选区也恢复——然后再删选区。这里若调用公开的 delete_selection()，
        /// 一次用户操作会多出一条多余的历史记录。
        bool delete_selection_no_history();

        /// 把当前状态记为一条撤销记录，除非 `kind` 要并入上一条。
        enum class EditKind
        {
            Insert,    ///< 与相邻的 Insert 合并（连续输入）
            Backspace, ///< 与相邻的 Backspace 合并
            Delete,    ///< 从不合并——每次按删除键都是独立一步
            Discrete   ///< 从不合并
        };
        void record(EditKind kind, f32 now);

        void move_to_line_edge(bool extend, bool forward);

        /// 一个输入框也就几十字节，所以历史上限按条数算，不按字节。vector 上
        /// 删最老一条是 O(n)，这个规模下比任何环形缓冲都省心，也不会写错。
        static constexpr size_t kMaxHistory = 200;

        std::string text_;
        size_t caret_ = 0;
        size_t anchor_ = 0;

        /// IME 正在组合的文本。不属于 `text_`；为什么必须分开，见上面的输入
        /// 组合一节。
        std::string preedit_;
        size_t preedit_caret_ = 0;

        // 两者都当普通栈用：push_back / pop_back。undo_ 存的是要退回的状态，
        // 栈顶即"最近一次编辑之前的文本"。
        std::vector<TextEditState> undo_;
        std::vector<TextEditState> redo_;

        EditKind last_kind_ = EditKind::Discrete;
        bool coalescing_ = false;
        f32 last_edit_time_ = -1e30f;
        f32 coalesce_seconds_ = 0.6f;
        f32 time_ = 0.0f;
        f32 last_time_ = -1e30f; ///< GC 水位，由 advance_time() 刷新

        /// 垂直移动时的期望列位置，没有时为 -1。
        ///
        /// 每次水平移动和每次编辑都会重置它。要是在这些操作后还保留，用户按
        /// Home 后光标还会"粘"在第 40 列，这不是想要的行为。
        int sticky_col_ = -1;
    };

} // namespace fox
