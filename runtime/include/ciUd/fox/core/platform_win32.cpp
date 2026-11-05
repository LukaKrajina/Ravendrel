// Fox core/platform_win32.cpp - Win32 剪贴板
//
// 为什么有这个文件而不用两个 GLFW 调用，见 platform.hpp：GLFW 剪贴板
// 需要 glfwInit() 和窗口上下文，而 Fox 的要点就是不需要窗口。

#include "platform.hpp"

#if defined(_WIN32)

#include <windows.h>

#include <string>

namespace fox
{
    namespace
    {
        class Win32Platform final : public Platform
        {
        public:
            std::string clipboard_text() override
            {
                if (!open_clipboard()) return {};

                std::string out;
                if (HANDLE h = GetClipboardData(CF_UNICODETEXT))
                {
                    const wchar_t *w = (const wchar_t *)GlobalLock(h);
                    if (w)
                    {
                        out = narrow(w);
                        GlobalUnlock(h);
                    }
                }
                CloseClipboard();
                return out;
            }

            void set_clipboard_text(std::string_view utf8) override
            {
                if (!open_clipboard()) return;
                EmptyClipboard();

                const std::wstring w = widen(utf8);
                const SIZE_T bytes = (SIZE_T)(w.size() + 1) * sizeof(wchar_t);

                // 必须用 GMEM_MOVEABLE：SetClipboardData 会接管句柄所有权，
                // 其他进程粘贴时剪贴板要能移动这块内存。
                HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes);
                if (h)
                {
                    void *p = GlobalLock(h);
                    if (p)
                    {
                        memcpy(p, w.c_str(), bytes);
                        GlobalUnlock(h);
                        // 成功后 OS 接管句柄；失败时句柄还是我们的，必须
                        // 释放。跳过失败分支的话，每次写剪贴板失败都泄漏。
                        if (!SetClipboardData(CF_UNICODETEXT, h)) GlobalFree(h);
                    }
                    else
                    {
                        GlobalFree(h);
                    }
                }
                CloseClipboard();
            }

        private:
            /// 别的进程正好开着剪贴板时 OpenClipboard 会失败，这很常见、
            /// 也是暂时的——文件管理器正在复制、同一应用的另一个实例。
            /// 第一次失败就当致命错误，会让粘贴间歇性失败，这是最糟的
            /// 失败方式：看起来像 Fox 坏了，而不是系统正忙。
            static bool open_clipboard()
            {
                for (int attempt = 0; attempt < 8; ++attempt)
                {
                    if (OpenClipboard(nullptr)) return true;
                    Sleep(5);
                }
                return false;
            }

            static std::string narrow(const wchar_t *w)
            {
                if (!w || !*w) return {};

                // 计算长度时不带 -1。传 -1 会让返回的计数包含结尾 NUL，
                // 于是分配 n-1 字节、却要求写 n 字节，堆字符串会越界一字节
                // ——这种缺陷只在剪贴板条目长度恰好凑上时才出现。
                const int len = (int)lstrlenW(w);
                if (len <= 0) return {};

                const int n = WideCharToMultiByte(CP_UTF8, 0, w, len, nullptr, 0, nullptr, nullptr);
                if (n <= 0) return {};

                std::string out((size_t)n, '\0');
                WideCharToMultiByte(CP_UTF8, 0, w, len, out.data(), n, nullptr, nullptr);
                return out; // 正好 n 字节 UTF-8，无 NUL
            }

            static std::wstring widen(std::string_view utf8)
            {
                if (utf8.empty()) return {};
                const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(),
                                                  (int)utf8.size(), nullptr, 0);
                if (n <= 0) return {};
                std::wstring out((size_t)n, L'\0');
                MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int)utf8.size(), out.data(), n);
                return out;
            }
        };
    } // namespace

    Platform *make_default_platform() { return new Win32Platform(); }

} // namespace fox

#else // !_WIN32

namespace fox
{
    // 这个目标平台上没有剪贴板。返回 null 而不是失败的桩，调用方可以
    // 检测"不支持"——qcos 有剪贴板系统调用之前 QuarkOS 就是这个状态。
    Platform *make_default_platform() { return nullptr; }

} // namespace fox

#endif
