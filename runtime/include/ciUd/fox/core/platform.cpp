// Fox core/platform.cpp - 接缝本身
//
// Win32 实现在 platform_win32.cpp，这样本文件不含任何 OS 头文件，可在
// 任何地方编译，包括独立内核。

#include "platform.hpp"

// IME 入口在 platform.hpp 声明、在这里实现，因为路由输入组合要找到拥有
// 焦点的字段，那是 Context 的问题而不是平台的问题。没有头文件循环：
// context.hpp 包含 platform.hpp，而这里是 .cpp。
#include "core/context.hpp"

#include <cstring>

namespace fox
{
    namespace
    {
        Platform *g_platform = nullptr;
    }

    void Platform::clipboard_text_into(char *out, size_t cap)
    {
        if (!out || cap == 0) return;
        out[0] = 0;

        // 截断而不是溢出。C ABI 调用方给一个固定 buffer，不知道剪贴板有
        // 多长，越界写过去永远是错的。
        //
        // 简单按字节切还可能切进多字节码点中间，调用方会拿到孤立的首字节
        // 或续字节，没法和真正的数据损坏区分。所以如果第一个没复制的字节
        // 是续字节，就回退到首字节，把整个码点丢掉。
        const std::string s = clipboard_text();
        size_t cut = s.size() < (cap - 1) ? s.size() : (cap - 1);

        if (cut < s.size())
        {
            while (cut > 0 && ((unsigned char)s[cut] & 0xC0) == 0x80) --cut;
            // 现在 `s[cut]` 是首字节（或 cut == 0），复制 [0, cut) 恰好
            // 丢掉被切断的整个码点，不多不少。
        }

        memcpy(out, s.data(), cut);
        out[cut] = 0;
    }

    Platform *platform() { return g_platform; }
    void set_platform(Platform *p) { g_platform = p; }

} // namespace fox
