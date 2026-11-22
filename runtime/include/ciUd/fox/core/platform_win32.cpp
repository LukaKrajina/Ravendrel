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

                HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes);
                if (h)
                {
                    void *p = GlobalLock(h);
                    if (p)
                    {
                        memcpy(p, w.c_str(), bytes);
                        GlobalUnlock(h);
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

                const int len = (int)lstrlenW(w);
                if (len <= 0) return {};

                const int n = WideCharToMultiByte(CP_UTF8, 0, w, len, nullptr, 0, nullptr, nullptr);
                if (n <= 0) return {};

                std::string out((size_t)n, '\0');
                WideCharToMultiByte(CP_UTF8, 0, w, len, out.data(), n, nullptr, nullptr);
                return out;
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
    }

    Platform *make_default_platform() { return new Win32Platform(); }

}

#else

namespace fox
{
    Platform *make_default_platform() { return nullptr; }

}

#endif