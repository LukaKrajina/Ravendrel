#pragma once
// Fox draw/font_ids.hpp - FontId，单独放这里以打破头文件循环
//
// FontId 放在这里而不是 text/font.hpp，这样 display_list.hpp 能直接用它，
// 不用把整个字体模块拉进来。起初是为了绕开真正的循环包含（font.hpp 需要
// DisplayList 来排版文本；display_list.hpp 需要 FontId 给批次作 key），
// 后来保留下来是因为它也标明了依赖方向：图元只知道*要画*文本，
// 不知道文本怎么光栅化。

#include "core/types.hpp"

namespace fox
{
    using FontId = u32;
    inline constexpr FontId FONT_INVALID = 0;

} // namespace fox