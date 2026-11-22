#pragma once
#include "nuklear_config.h"
#include <string>

namespace qgui
{

enum FontSize
{
    FONT_TINY = 0,
    FONT_BODY = 1,
    FONT_TITLE = 2,
    FONT_DISPLAY = 3,
    FONT_COUNT = 4
};

bool find_i18n_font(std::string &out_cjk, std::string &out_cyrillic);

void load_i18n_fonts(struct nk_font_atlas *atlas);

const struct nk_user_font *font_at(int index);

int font_count();

float font_px(int index);

int atlas_info();

void set_atlas_glyphs(int n);

}