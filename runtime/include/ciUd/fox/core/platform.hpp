#pragma once
// Fox core/platform.hpp - Fox 自己提供不了的宿主服务
//
// 为什么不用 GLFW 的剪贴板？
// --------------------------------
// `glfwGetClipboardString` / `glfwSetClipboardString` 看起来是现成答案，
// 其实不对。它们要求 glfwInit() 成功，某些平台上还要求有活跃的窗口
// 上下文——CPU 路径刻意两者都不要求。Fox 必须在完全没有窗口的环境下
// 工作：无头渲染、CI 容器、QuarkOS（没有 libc、没有显示驱动的独立内核）。
// 只在恰好有窗口时才管用的剪贴板，在最需要它的平台上恰恰用不了。
//
// 所以这是一个真正的接缝，平台代码是它背后每 OS 一个小的编译单元。
//
// 范围：这个接口刻意只做一件事。每个"以防万一"加的虚函数，QuarkOS 构建
// 都得先实现才能链接——正是这种压力让接口保持克制。有需求再加下一个能力。

#include "core/types.hpp"

#include <string>
#include <string_view>

namespace fox
{
    class Platform
    {
    public:
        virtual ~Platform() = default;

        /// 当前剪贴板内容，UTF-8。不可用时为空。
        ///
        /// 空字符串同时也是"剪贴板本来就是空的"的正常答案，调用方无法
        /// 区分两者。对文本框来说可以接受——两种情况都粘贴不出来就是对的
        /// 反应——这也是它不是 `bool read(std::string&)` 的原因。
        virtual std::string clipboard_text() = 0;

        /// 用 UTF-8 文本替换剪贴板内容。不可用时什么都不做：剪贴板出错
        /// 不值得让应用崩掉。
        virtual void set_clipboard_text(std::string_view utf8) = 0;

        /// 剪贴板文本的 NUL 结尾副本，截断到 `cap - 1`。
        /// 为 C ABI 提供，它没法交出 std::string。
        ///
        /// 刻意不标 const，跟这里其他读接口不同：读剪贴板会打开再关闭
        /// 系统剪贴板，是对全局状态的副作用。标成 const 是撒谎，只会把
        /// const_cast 推到实现里去。
        void clipboard_text_into(char *out, size_t cap);
    };

    // ── IME ────────────────────────────────────────────────────────────
    //
    // 三个调用、不带状态，因为状态全在平台侧。Fox 从不问"IME 在干什么"，
    // 由宿主告知。这样平台层不进入 Fox 的控制流，宿主也可以完全不靠系统
    // 支持就合成一次输入——测试、远程 UI、回放都用得上。
    //
    // 这些是自由函数而不是 Platform 虚函数，因为输入组合属于拥有焦点的
    // 那个字段，该是哪个字段不由平台来选。Fox 来路由。
    //
    //   · begin/update：IME 打开或更新了输入组合。`caret` 是合成中文本
    //     内部的字节偏移，不是文档里的偏移。
    //   · commit：用户选定了候选词。成为一步撤销。
    //   · cancel：放弃。不插入任何内容。
    //
    // ⚠ Windows 上这需要通过窗口的 HWND 驱动 TSF（或旧式 IMM32）。
    //   两者都还没实现。所以 Fox 的输入组合支持是完整的，只是没有驱动：
    //   模型、渲染、撤销语义都能用，宿主可以通过 fox_ime_* 推入合成。
    //   缺的是监听真实 IME 的那部分。
    //
    //   这部分刻意留白，不盲写。本环境里没法验证——没有活跃的 IME，也没法
    //   忠实模拟——从外部看，从没跑过的代码和不能用的代码没有区别。

    /// 把 IME 输入组合路由给拥有焦点的字段。没有焦点时什么都不做，这是
    /// 正确结果而非错误：无处可去的输入组合就是用户已经放弃的输入。
    void ime_composition_begin(const char *utf8, size_t caret_bytes);
    void ime_composition_update(const char *utf8, size_t caret_bytes);
    void ime_composition_commit();
    void ime_composition_cancel();

    /// IME 是否正在向拥有焦点的字段输入？由拥有焦点的字段回答，不是平台。
    bool ime_is_composing();

    /// 当前平台对象，宿主未安装时为 null。
    ///
    /// null 表示"这里没有剪贴板"，所有调用方都必须容忍——QuarkOS 在系统
    /// 调用出现之前就是这个状态。
    Platform *platform();

    /// 安装平台对象。传 null 清除。
    ///
    /// 想用自己的剪贴板（远程 UI、内嵌引擎、测试）的宿主，必须在
    /// Context::init 之前安装，因为 init 在没有时会装一个默认的。
    void set_platform(Platform *p);

    /// 当前构建的平台：有 Win32 就是 Win32，其他地方为 null。刻意返回
    /// null 而不是调用时才失败的桩，这样"不支持"是调用方可检测的值。
    Platform *make_default_platform();

} // namespace fox
