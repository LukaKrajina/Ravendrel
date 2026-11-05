// ═══════════════════════════════════════════════════════════════════════
// Bro 可变字符串（rvdl 侧可写文本缓冲）
// ═══════════════════════════════════════════════════════════════════════
//
// rvdl 的 string 是 `i8*`，字面量进了常量池后不可写：没有 strlen，
// 并且 `"a" + "b"` **不会**拼字符串——它是指针加法，会算出个偏移量。
// 所以想在 rvdl 里做「就地编辑」，必须自带一块可写缓冲。
//
// 于是有了这个模块：一份全局的 codepoint 数组 + 光标/选区，
// 对外只暴露按码点操作的原语：
//
//   gh_str_add(0x4E2D)      加字符 —— 缓冲自己拼 UTF-8
//   gh_str_len()            取长度 —— 按码点，不是字节
//   gh_str_backspace()      删字符 —— 整字符删，不会劈开汉字
//   gh_str_codepoint(i)     取第 i 个字符的码点
//
// 全部按码点走，是为了避免 rvdl 在中文上把 3 字节切成两半——
// 按字节拼字符串在英文上看着没问题，中文直接出乱码方块。
//
// 实现在 rvdl_rt 里（不是宿主外壳），因为 UI 原语也在这里；
// 拆到两边会因为 inline static 的 DLL 副本问题拿不到同一份状态。

#pragma once
#include "RuntimeApi.h"
#include <cstdint>

#ifndef RVDL_HOST_EXPORT
#define RVDL_HOST_EXPORT extern "C" RVDL_RT_API
#endif

extern "C"
{
    // ── 内容 ──────────────────────────────────────────────────────────
    /// 清空
    RVDL_HOST_EXPORT void gh_str_reset(void);
    /// 加一个 Unicode 码点（<0 或 > 0x10FFFF 时忽略）。返回新长度。
    RVDL_HOST_EXPORT int32_t gh_str_add(int32_t cp);
    /// 加一个 ASCII 字符（等价 gh_str_add(c)，用于键盘输入）
    RVDL_HOST_EXPORT int32_t gh_str_add_ascii(int32_t c);
    /// 退格：有选区先删选区，否则删光标前一个。新光标位置。
    RVDL_HOST_EXPORT int32_t gh_str_backspace(void);
    /// 用 UTF-8 替换全部内容（宿主注入路径）。返回码点数。
    RVDL_HOST_EXPORT int32_t gh_str_set(const char *utf8);
    /// 追加 UTF-8。返回码点数。
    RVDL_HOST_EXPORT int32_t gh_str_append(const char *utf8);

    /**
     * 在光标处插入一段 UTF-8（**一个**撤销点），有选区先删选区。
     *
     * 为什么必须有：粘贴一段话如果用 gh_str_add 逐码点插入，
     * 会压 N 个撤销位置 —— 按一次撤销只撤掉一个字。
     */
    RVDL_HOST_EXPORT int32_t gh_str_insert_str(const char *utf8);

    // ── 查询 ──────────────────────────────────────────────────────────
    /// 码点数（不是字节数）
    RVDL_HOST_EXPORT int32_t gh_str_len(void);
    /// UTF-8 字节数
    RVDL_HOST_EXPORT int32_t gh_str_bytes(void);
    /// 内容版本号：每次**内容**变更递增（移动光标不递增）。
    ///
    /// 依赖方（行索引 / 词法分类）拿它当缓存键。
    /// 没有它就只能每帧重算整份文件 —— 5000 行的文件在 1900fps 下
    /// 就是每秒复制 380 MB。
    RVDL_HOST_EXPORT int32_t gh_str_version(void);
    /// 第 index 个码点，越界返回 -1
    RVDL_HOST_EXPORT int32_t gh_str_codepoint(int32_t index);
    /// UTF-8 C 字符串（可直接喂给 rvdl_ui_text*）。可能为 nullptr。

    RVDL_HOST_EXPORT const char *gh_str_cstr(void);

    // ── 位置编辑 ──────────────────────────────────────────────────────
    /// 在 index 前插入一个码点。返回新长度。
    RVDL_HOST_EXPORT int32_t gh_str_insert(int32_t index, int32_t cp);
    /// 删除 index 处的码点。0 表示成功。
    RVDL_HOST_EXPORT int32_t gh_str_delete(int32_t index);
    /// 只保留前 count 个码点
    RVDL_HOST_EXPORT int32_t gh_str_truncate(int32_t count);

    // ── 光标与选区（编辑器语义）───────────────────────────────────────
    //
    // 鼠标点一下 = anchor 和 cursor 都落在那儿，选区塌掉。
    // 拖选 = anchor 保持按下时的位置，cursor 跟着鼠标走。
    // 所以「移动光标」要分成两种调用，混用一个会让选区永远为空。
    //
    /// 点击：anchor = cursor = pos。会**清空**选区。
    RVDL_HOST_EXPORT int32_t gh_str_set_cursor(int32_t pos);
    /// 拖选/Shift+方向：挪动 cursor，**保住** anchor。
    /// 这才是 drag-select 该用的函数。
    RVDL_HOST_EXPORT int32_t gh_str_select_extend(int32_t pos);
    /// 只挪 anchor，不动 cursor
    RVDL_HOST_EXPORT int32_t gh_str_set_anchor(int32_t pos);
    RVDL_HOST_EXPORT int32_t gh_str_cursor(void);
    RVDL_HOST_EXPORT int32_t gh_str_anchor(void);
    /// 塌掉选区（anchor = cursor）
    RVDL_HOST_EXPORT int32_t gh_str_select_clear(void);
    /// 选区下界 / 上界（anchor 与 cursor 较小/较大者）
    RVDL_HOST_EXPORT int32_t gh_str_sel_start(void);
    RVDL_HOST_EXPORT int32_t gh_str_sel_end(void);
    RVDL_HOST_EXPORT int32_t gh_str_has_selection(void);
    /// 删掉选区，光标落到选区起点。返回是否删了东西。
    RVDL_HOST_EXPORT int32_t gh_str_delete_selection(void);
    /// 选区的 UTF-8（可能为空串）
    RVDL_HOST_EXPORT const char *gh_str_selection(void);

    /// 诊断：把当前状态打到 stderr
    RVDL_HOST_EXPORT void gh_str_dump(void);
}

// ═══════════════════════════════════════════════════════════════════════
// 实例 1 —— 编辑器文档（gh_ed_*）
// ═══════════════════════════════════════════════════════════════════════
//
// 与 gh_str_* 签名完全一致，只是操作另一块缓冲。两块必须分开：
// 草稿要跨「打开文件 → 关掉文件」保留，文件内容要随切换重新载入。
// 挤在一块的话得每帧判断「现在该动哪块」，漏判一次就是
// 用户正在输入的草稿被文件内容覆盖 —— 而且这个 bug 只在
// 「先打字再开文件」时出现，测不出来。
//
// 少了 delete / truncate / insert / append：编辑器不需要逐字符增删
// 这些口子，写文件是整份覆盖（gh_ed_set）。少几个导出少几个错用点。

extern "C"
{
    RVDL_HOST_EXPORT void gh_ed_reset(void);
    /**
     * 载入一份文件。
     *
     * `path` 是**相对路径**（与槽位里的一致），用来：
     *   · 记住保存时要写回哪儿
     *   · 记下载入时的 mtime，保存时检测「文件被外部改过」
     *
     * 两个参数必须一起给：路径和内容一旦分两次调用就可能对不上，
     * 而「内容是 A、路径是 B」正是保存覆盖错文件的那种错。
     */
    RVDL_HOST_EXPORT int32_t gh_ed_set(const char *utf8, const char *path);
    RVDL_HOST_EXPORT int32_t gh_ed_append(const char *utf8);
    RVDL_HOST_EXPORT int32_t gh_ed_add(int32_t cp);
    RVDL_HOST_EXPORT int32_t gh_ed_backspace(void);

    RVDL_HOST_EXPORT int32_t gh_ed_len(void);
    RVDL_HOST_EXPORT int32_t gh_ed_bytes(void);
    RVDL_HOST_EXPORT int32_t gh_ed_version(void);
    RVDL_HOST_EXPORT int32_t gh_ed_codepoint(int32_t index);
    RVDL_HOST_EXPORT const char *gh_ed_cstr(void);

    RVDL_HOST_EXPORT int32_t gh_ed_set_cursor(int32_t pos);
    RVDL_HOST_EXPORT int32_t gh_ed_cursor(void);
    RVDL_HOST_EXPORT int32_t gh_ed_select_extend(int32_t pos);
    RVDL_HOST_EXPORT int32_t gh_ed_set_anchor(int32_t pos);
    RVDL_HOST_EXPORT int32_t gh_ed_anchor(void);
    RVDL_HOST_EXPORT int32_t gh_ed_select_clear(void);
    RVDL_HOST_EXPORT int32_t gh_ed_sel_start(void);
    RVDL_HOST_EXPORT int32_t gh_ed_sel_end(void);
    RVDL_HOST_EXPORT int32_t gh_ed_has_selection(void);
    RVDL_HOST_EXPORT int32_t gh_ed_delete_selection(void);
    RVDL_HOST_EXPORT const char *gh_ed_selection(void);

    // ── 编辑 ────────────────────────────────────────────────────────────

    /// 插入一段字符串（换行 / Tab 缩进 / 粘贴）。有选区先替换掉。
    /// 返回插入的码点数。
    ///
    /// 单独一个入口而不是让 rvdl 循环调 gh_ed_add()：那样一次 Tab 会压 4~8 个
    /// 撤销位置，用户按一次撤销只撤掉一个空格，表现得像「撤销坏了」。
    RVDL_HOST_EXPORT int32_t gh_ed_insert_str(const char *utf8);

    /// 回车：换行 + **继承当前行的前导空白**。返回插入的码点数（含那个 \n）。
    ///
    /// 为什么不让 rvdl 自己拼：rvdl 拼不出动态字符串，而「数出前导空白有几个
    /// 再拼回去」正好需要。用 insert_str 一个空格一次会把一次回车拆成
    /// N 个撤销位置 —— 按一次撤销只退掉一个空格，看起来像撤销坏了。
    RVDL_HOST_EXPORT int32_t gh_ed_newline(void);

    /// 删除第 index 个码点（Delete 键：删光标**后面**一个）。
    /// 退格删前面那个，方向相反，两件事不能共用一个函数。
    RVDL_HOST_EXPORT int32_t gh_ed_delete_at(int32_t index);

    /// 单个码点 → UTF-8 字符串。
    ///
    /// rvdl 造不出动态字符串，所以「把刚敲进来的那个汉字存进去」这件事
    /// 只能在这儿做。返回值指向内部缓冲，下一次调用会被覆盖 ——
    /// rvdl 侧拿到后要立刻用掉。
    ///
    /// 少了这个，中文注释就存不进去：界面上看得见，光标也动得了，
    /// 但按保存之后汉字全没了。这是最难自己发现的一类 bug。
    RVDL_HOST_EXPORT const char *gh_utf8_cp(int32_t cp);

    /// 撤销 / 重做。返回是否真的动了（0 = 没有可撤销的东西）。
    ///
    /// 实现是**整份快照**而不是「反向操作」—— 见 BroStr.cpp 的注释。
    /// 移动光标不记快照（没有内容变化，撤销它对用户没有意义）。
    RVDL_HOST_EXPORT int32_t gh_ed_undo(void);
    RVDL_HOST_EXPORT int32_t gh_ed_redo(void);
    /// 当前有没有可撤销 / 可重做（用来灰掉按钮）
    RVDL_HOST_EXPORT int32_t gh_ed_can_undo(void);
    RVDL_HOST_EXPORT int32_t gh_ed_can_redo(void);
    /**
     * 撤销栈深度（能连续撤多少步）。
     *
     * 为什么需要它：gh_ed_can_undo() 只是「栈非空吗」这个布尔值。
     * 一次操作压了 3 个撤销点的话 can_undo 仍然是 1 ——
     * 拿它断言「只压了一个点」是**测不出来的**。
     */
    RVDL_HOST_EXPORT int32_t gh_ed_undo_depth(void);

    /// 相对载入时的内容有没有被改过（界面上的「●」脏标记）
    RVDL_HOST_EXPORT int32_t gh_ed_dirty(void);

    // ── 保存 ────────────────────────────────────────────────────────────
    //
    // 走宿主而不是自己写文件：rvdl 没有文件 I/O，而且保存是**唯一**一个
    // 「界面动作直接改磁盘」的地方，必须过宿主这一道 ——
    // 路径校验、外部修改检测、原子写都在那儿。

    /// 把文档写到 utf8_path。
    ///   0 = 成功
    ///   1 = 路径越界（不允许写到工作区之外）
    ///   2 = 文件在载入之后被外部改过，拒绝覆盖
    ///   3 = 打不开 / 写失败
    ///   4 = 没有载入任何文档
    /// 失败原因另见 gh_save_error()。
    RVDL_HOST_EXPORT int32_t gh_ed_save(const char *utf8_path);

    /// 上一次 gh_ed_save 失败的可读原因（中文）
    RVDL_HOST_EXPORT const char *gh_save_error(void);
}

// ═══════════════════════════════════════════════════════════════════════
// 实例 2 —— 编辑器查找框（gh_find_*）
// ═══════════════════════════════════════════════════════════════════════
//
// 为什么单独一块缓冲：g_draft（聊天输入框）已经被占用了。
// 共用的话，用户在输入框打了半句话、按 Ctrl+F，那半句话就没了 ——
// 「输入框 A 抢走输入框 B 的缓冲」这个坑在 draft/doc 上已经栽过一次。
//
// 为什么是 **field 参数**而不是两个实例各自一套：查找框只用得到 7 个操作，
// 而 gh_str_* 是 26 个。照抄一遍等于把接口面积翻倍，
// 其中 insert / truncate / anchor / selection 这些查找框一个都不需要。
//
// field：0 = 查找词，1 = 替换词。

extern "C"
{
    /// 用 UTF-8 替换某个输入框的内容（宿主注入：预填选中的文本等）
    RVDL_HOST_EXPORT int32_t gh_find_set(int32_t field, const char *utf8);
    /// 敲一个码点进去。返回新长度。
    RVDL_HOST_EXPORT int32_t gh_find_add(int32_t field, int32_t cp);
    /// 退格：有选区先删选区，否则删前一个
    RVDL_HOST_EXPORT int32_t gh_find_backspace(int32_t field);
    /// 清空
    RVDL_HOST_EXPORT int32_t gh_find_clear(int32_t field);
    /// 码点数
    RVDL_HOST_EXPORT int32_t gh_find_len(int32_t field);
    /// UTF-8 C 字符串（可直接喂给 rvdl_ui_text*）
    RVDL_HOST_EXPORT const char *gh_find_cstr(int32_t field);
    /// 第 index 个码点；越界 -1
    ///
    /// 为什么需要：跳行要把框里的数字读成整数，而 rvdl 拆不了字符串
    ///（gh_find_cstr 只能一次拿到整串，没有下标访问）。
    RVDL_HOST_EXPORT int32_t gh_find_codepoint(int32_t field, int32_t index);
}

// ═══════════════════════════════════════════════════════════════════════
// 查找 / 替换（作用在编辑器文档上）
// ═══════════════════════════════════════════════════════════════════════
//
// 放在 C++ 而不是 rvdl 的理由不只是快：rvdl 可以用 gh_ed_codepoint 逐码点比较，
// 但一个 5000 行文件配 3 字查找词就是 30 万次 JIT 循环；更关键的是**测不了**
// —— 向前找 / 向后找 / 找不到 / 空 needle / 跨行匹配这五种情况
// 各自都能静默地错，而在这里每一种都是一行断言。

extern "C"
{
    /// 从 from_cp 往后找 needle，返回匹配的**起点**码点下标；找不到 -1。
    /// **不回绕** —— 回绕与「一共几处」的计数交给 rvdl，那是界面的事。
    RVDL_HOST_EXPORT int32_t gh_ed_find(const char *needle, int32_t from_cp);

    /// 从 from_cp 往回找（Shift+Enter）。返回匹配起点；找不到 -1。
    RVDL_HOST_EXPORT int32_t gh_ed_rfind(const char *needle, int32_t from_cp);

    /// 一共几处匹配。界面上要显示 `N/M`。
    /// 重叠的不重复计数（「aa」在「aaaa」里算 2 处，不是 3 处）。
    RVDL_HOST_EXPORT int32_t gh_ed_find_count(const char *needle);

    /// cp 处那一处是第几个匹配（1 起）；cp 不是匹配起点则 -1。
    ///
    /// 为什么单独给：界面上要显示「3/17」。在 rvdl 里数只能循环调
    /// gh_ed_find，那是 O(处数 × 全文)，每按一次回车白扫几十遍全文。
    RVDL_HOST_EXPORT int32_t gh_ed_ordinal(const char *needle, int32_t cp);

    /**
     * 替换**当前选区**。一个撤销点。
     *
     * ⚠ 不能让 rvdl 侧用 delete_selection + insert_str 凑：
     *   那样会压两个撤销位置（一次替换要按两下撤销）；
     *   而且 insert_str 入参为空时**提前返回、来不及删选区**，
     *   「替换成空串」会静默地什么都不发生。
     */
    RVDL_HOST_EXPORT int32_t gh_ed_replace_selection(const char *replacement);

    /**
     * 全部替换。返回替换了几处。
     *
     * ⚠ 这必须是**一个操作、一个撤销点**。在 rvdl 里循环调 insert_str
     *   会压 N 个撤销位置，用户按一次 Ctrl+Z 只撤掉一处 ——
     *   看起来就像「撤销坏了」。回车继承缩进栽过同一个坑。
     */
    RVDL_HOST_EXPORT int32_t gh_ed_replace_all(const char *needle, const char *replacement);

    // ── 多光标 ────────────────────────────────────────────────────────────
    //
    // 光标列表放在 C++ 而不是 rvdl，两个原因：
    //   1. rvdl **没有数组语法**（整个代码库里一个 `int32[]` 都没有），
    //      放 rvdl 只能写成一长串标量，每加一个原语就要同步改一长串。
    //   2. 「一次按键在 N 处编辑、但只压一个撤销点」这件事
    //      本来就得进 StrBuf（事务），两边都在同一侧。
    //
    // 所有 *_type / *_backspace / *_delete 都是**一个撤销点**：
    // 在 rvdl 里逐个调用单光标原语会压 N 个撤销点，
    // 用户按一次撤销只撤掉一处 —— 看起来就像「撤销坏了」。

    /// 收成一个光标（Esc / 单击 / 打开别的文件）
    RVDL_HOST_EXPORT int32_t gh_ed_mc_clear(void);
    /// 当前光标数。0 或 1 都是「单光标模式」。
    RVDL_HOST_EXPORT int32_t gh_ed_mc_count(void);
    /// 在 pos 加一个光标；同一位置不重复加。加满了返回 0。
    RVDL_HOST_EXPORT int32_t gh_ed_mc_add(int32_t pos);
    /// 第 i 个光标位置（渲染用）；越界 -1
    RVDL_HOST_EXPORT int32_t gh_ed_mc_pos(int32_t i);
    /// 第 i 个光标选区起点；没有选区 -1（渲染才知道要不要画高亮）
    RVDL_HOST_EXPORT int32_t gh_ed_mc_sel(int32_t i);

    /**
     * Ctrl+D：选中下一个相同处。
     *
     * 没有选区时先取光标所在的**词**；有选区就用选中的内容。
     * 返回加完之后的光标数；到头了返回当前数（不重复加）。
     */
    RVDL_HOST_EXPORT int32_t gh_ed_mc_add_next(void);

    /// 在所有光标处插入一个码点（打字）。一个撤销点。
    RVDL_HOST_EXPORT int32_t gh_ed_mc_type(int32_t cp);
    /// 在所有光标处插入一段文本（粘贴）。一个撤销点。
    RVDL_HOST_EXPORT int32_t gh_ed_mc_str(const char *utf8);
    /// 所有光标退格。一个撤销点。
    RVDL_HOST_EXPORT int32_t gh_ed_mc_backspace(void);
    /// 所有光标 Delete。一个撤销点。
    RVDL_HOST_EXPORT int32_t gh_ed_mc_delete(void);
}