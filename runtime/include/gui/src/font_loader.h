#pragma once
#include "nuklear_config.h"
#include <string>

namespace qgui
{

/// 字号档位。与 BroUi.hpp 里的 QK_FONT_* 一一对应。
///
/// 为什么是「档位」而不是直接给像素：
/// 字体字号是在 atlas 烘焙时定死的，运行时改不了。给一个连续的 px 参数
/// 只会让人以为能任意缩放，然后拿到一张没烘焙过的尺寸（豆腐块）。
/// 档位把这个约束显式化 —— 要新尺寸就往 kFontSpecs 里加一行重新烘焙。
enum FontSize
{
    FONT_TINY = 0,    ///< 12px：状态栏、图例、极弱标签
    FONT_BODY = 1,    ///< 16px：正文（默认）
    FONT_TITLE = 2,   ///< 24px：区块标题
    FONT_DISPLAY = 3, ///< 34px：项目名、关键数字
    FONT_COUNT = 4
};

bool find_i18n_font(std::string &out_cjk, std::string &out_cyrillic);

/// 把所有字号烘焙进 atlas。必须在 nk_font_atlas_begin 之后、
/// nk_font_atlas_end 之前调用。
void load_i18n_fonts(struct nk_font_atlas *atlas);

/// 取第 index 个字号的用户字体。越界返回 nullptr。
const struct nk_user_font *font_at(int index);

/// 字号档位数（失败回退时会少于 FONT_COUNT）
int font_count();

/// 第 index 档的实际像素高度（0 = 该档不可用）
float font_px(int index);

/// 烘焙结果诊断：返回图集里烘进去的**字形数**。0 = 还没烘焙。
///
/// 为什么是字形数而不是图集宽高：
/// 多字号把图集从 ~6 MB 抬到 ~20 MB，撑爆 maxImageDimension2D 时
/// 表现是「启动失败」这种毫无线索的错，所以确实需要一个能核对的值。
///
/// ⚠ 早先这里返回图集宽高（低 16 位宽 / 高 16 位高），但**回填函数
///   从来没被调用过** —— 于是它恒为 0。比没有更糟：
///   0 看起来像「图集是空的」，而那是一个完全不同的故障。
///   （nuklear 的 nk_font_atlas_bake 确实会写 atlas->tex_width，
///   但 GLFW3 的 font_stash_end 之后拿不到它。）
///   所以改成回填真正拿得到的那个数：glyph_count。
int atlas_info();

/// 回填字形数。由 RenderContext 在 font_stash_end 之后调。
void set_atlas_glyphs(int n);

} // namespace qgui
