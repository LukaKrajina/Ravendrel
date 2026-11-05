#pragma once
// ============================================================================
// font_search_paths.hpp —— 字体绝对路径的**唯一知识来源**
//
// 为什么要有这个文件
// ------------------
// 硬编码字体路径曾出现在两处、各写各的：qgui 的 `font_loader.cpp`
// （Nuklear 经典 GUI 的字号档位）与 fox 的 `text/font.cpp`（fox 文本引擎的
// 主字体 + fallback 链）。两处清单**内容与优先级都不同**（不是疏漏，是两边
// 各自演化），直接合并成一张表会改变多字体机器上实际选中的字体 —— 那是
// 渲染度量的变化，不是纯重构，不能悄悄做。
//
// 于是这里把**路径字符串**收敛为具名常量（单一维护点），使用方按自己的
// 优先级引用名字：
//   · 新增一个平台的字体 → 只在这里加一行 constexpr；
//   · 调整某套 UI 的优先级 → 只改那一处的引用顺序，不影响另一套。
// 这样「加路径漏一处」的隐患（症状是某平台缺字 = 豆腐块，而缺字**不报错**，
// 只是画面不对）从结构上消失。
// ============================================================================

#include <cstddef>

namespace qgui
{
namespace fontpath
{
    // ── Linux：CJK ────────────────────────────────────────────────────────
    constexpr const char *NOTO_CJK_SC_OTF = "/usr/share/fonts/opentype/noto/NotoSansCJKsc-Regular.otf";
    constexpr const char *NOTO_CJK_TTC = "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc";
    constexpr const char *NOTO_CJK_PKG_TTC = "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc";
    constexpr const char *NOTO_CJK_FLAT_TTC = "/usr/share/fonts/noto/NotoSansCJK-Regular.ttc";
    constexpr const char *NOTO_SANS_REGULAR = "/usr/share/fonts/noto/NotoSans-Regular.ttf";
    constexpr const char *WQY_MICROHEI = "/usr/share/fonts/truetype/wqy/wqy-microhei.ttc";
    constexpr const char *WQY_ZENHEI = "/usr/share/fonts/truetype/wqy/wqy-zenhei.ttc";
    constexpr const char *DROID_FALLBACK_FULL = "/usr/share/fonts/truetype/droid/DroidSansFallbackFull.ttf";
    // ── Linux：Latin / Cyrillic ───────────────────────────────────────────
    constexpr const char *DEJAUVU_SANS = "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf";
    constexpr const char *DEJAUVU_SANS_FLAT = "/usr/share/fonts/dejavu/DejaVuSans.ttf";
    constexpr const char *LIBERATION_SANS = "/usr/share/fonts/liberation/LiberationSans-Regular.ttf";
    // ── Android ───────────────────────────────────────────────────────────
    constexpr const char *ANDROID_NOTO_CJK = "/system/fonts/NotoSansCJK-Regular.ttc";
    constexpr const char *ANDROID_ROBOTO = "/system/fonts/Roboto-Regular.ttf";
    constexpr const char *ANDROID_DROID_FALLBACK = "/system/fonts/DroidSansFallback.ttf";
    // ── Windows ───────────────────────────────────────────────────────────
    constexpr const char *WIN_MSYH = "C:/Windows/Fonts/msyh.ttc";
    constexpr const char *WIN_SIMHEI = "C:/Windows/Fonts/simhei.ttf";
    constexpr const char *WIN_SIMSUN = "C:/Windows/Fonts/simsun.ttc";
    constexpr const char *WIN_SEGOEUI = "C:/Windows/Fonts/segoeui.ttf";
    constexpr const char *WIN_SEGUIEE = "C:/Windows/Fonts/seguiee.ttf";
    constexpr const char *WIN_SEGUISYM = "C:/Windows/Fonts/seguisym.ttf";
    constexpr const char *WIN_ARIAL = "C:/Windows/Fonts/arial.ttf";
    // ── macOS ─────────────────────────────────────────────────────────────
    constexpr const char *MAC_PINGFANG = "/System/Library/Fonts/PingFang.ttc";
    constexpr const char *MAC_STHEITI = "/System/Library/Fonts/STHeiti Light.ttc";
    constexpr const char *MAC_HELVETICA = "/System/Library/Fonts/Helvetica.ttc";
    constexpr const char *MAC_SFNS = "/System/Library/Fonts/SFNS.ttf";
    constexpr const char *MAC_APPLE_SYMBOLS = "/System/Library/Fonts/Apple Symbols.ttf";
    constexpr const char *MAC_ARIAL_UNICODE = "/Library/Fonts/Arial Unicode.ttf";

} // namespace fontpath
} // namespace qgui
