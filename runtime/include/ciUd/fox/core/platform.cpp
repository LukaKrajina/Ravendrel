#include "platform.hpp"

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

        const std::string s = clipboard_text();
        size_t cut = s.size() < (cap - 1) ? s.size() : (cap - 1);

        if (cut < s.size())
        {
            while (cut > 0 && ((unsigned char)s[cut] & 0xC0) == 0x80) --cut;
        }

        memcpy(out, s.data(), cut);
        out[cut] = 0;
    }

    Platform *platform() { return g_platform; }
    void set_platform(Platform *p) { g_platform = p; }

}