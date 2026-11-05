#pragma once
#include <string>

namespace qgui
{

    enum class Lang
    {
        EN,
        ZH,
        RU,
        FR,
        DE
    };

    Lang current_lang();

    void set_lang(Lang l);

    Lang detect_lang();

    const char *tr(const char *key);

    struct LangEntry
    {
        Lang lang;
        const char *label;
    };

    const LangEntry *available_langs();

    int available_lang_count();

    const char *lang_code(Lang l);
}