#pragma once

#include "core/types.hpp"

#include <string>
#include <string_view>

namespace fox
{
    class Platform
    {
    public:
        virtual ~Platform() = default;

        virtual std::string clipboard_text() = 0;

        virtual void set_clipboard_text(std::string_view utf8) = 0;

        void clipboard_text_into(char *out, size_t cap);
    };


    void ime_composition_begin(const char *utf8, size_t caret_bytes);
    void ime_composition_update(const char *utf8, size_t caret_bytes);
    void ime_composition_commit();
    void ime_composition_cancel();

    bool ime_is_composing();

    Platform *platform();

    void set_platform(Platform *p);

    Platform *make_default_platform();

}