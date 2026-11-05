// Fox 无头渲染测试——无窗口、无 Vulkan、无驱动。
//
// 为什么存在
// ---------------
// 1. QuarkOS（runtime/qcos）是独立内核：没有 GPU、没有显示驱动、没有
//    libc。它只能调 CPU 光栅化器。这个二进制就是整套 UI 栈能在那种环境
//    下运行的证明——而且它在桌面这里就能跑，意味着 QuarkOS 路径不用启动
//    内核也能测试。
//
// 2. 窗口截图不算证据。截图可能被遮挡、在缩放中途截取，或显示过期的
//    合成器表面，这种事发生过一次就烧掉了一整轮调试——界面看着空白，
//    而这个说法无法证伪。从自己的缓冲读像素没有这种失效模式。
//
// 3. CPU 光栅化器是 shaders.frag 的参考实现。两者消费同一份 InstanceData，
//    用两者渲染同一个 UI 再对比，就是对 GLSL 真正的正确性检查。

#include "core/context.hpp"
#include "widget/primitives.hpp"
#include "window/window.hpp"
#include "backend/cpu_backend.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace fox;

namespace
{
    constexpr int kCell = 32;
    constexpr int kIconCount = 4;

    Rect icon_cell(int i)
    {
        const float w = 1.0f / (float)kIconCount;
        return Rect{(float)i * w, 0.0f, w, 1.0f};
    }

    /// 与窗口 demo 画的四个图标相同，构造方式一致（RGBA，每像素 4 字节），
    /// 这样两条路径可比。
    std::vector<u8> build_icon_sheet(int w, int h)
    {
        std::vector<u8> px((size_t)w * h * 4, 0);
        for (int cell = 0; cell < kIconCount; ++cell)
        {
            const int ox = cell * kCell;
            for (int y = 0; y < kCell; ++y)
                for (int x = 0; x < kCell; ++x)
                {
                    const float fx = (float)x + 0.5f, fy = (float)y + 0.5f;
                    float cov = 0.0f;
                    switch (cell)
                    {
                    case 0: cov = std::sqrt((fx - 16) * (fx - 16) + (fy - 16) * (fy - 16)) <= 12.0f; break;
                    case 1: { const float d = std::sqrt((fx - 16) * (fx - 16) + (fy - 16) * (fy - 16)); cov = d <= 12.0f && d >= 7.0f; break; }
                    case 2: { const float ty = fy / 26.0f; cov = ty <= 1.0f && std::fabs(fx - 16.0f) <= ty * 12.0f; break; }
                    default: { const float dx = std::fabs(fx - 16.0f), dy = std::fabs(fy - 16.0f); cov = dx <= 13.0f && dy <= 13.0f && !(dx <= 9.0f && dy <= 9.0f); break; }
                    }
                    if (!cov) continue;
                    const size_t o = ((size_t)y * w + (size_t)(ox + x)) * 4;
                    px[o + 0] = 255; px[o + 1] = 255; px[o + 2] = 255; px[o + 3] = 255;
                }
        }
        return px;
    }
} // namespace

int main(int argc, char **argv)
{
    const char *out = argc > 1 ? argv[1] : "fox_headless.ppm";
    // "select" 模式把文本框驱动到选中状态，这样能在图片里检查高亮。
    // 选区渲染属于"编译通过、测试通过"说明不了的东西——它要么画在字形
    // 下面，要么没有，只有像素能给出答案。
    const bool want_selection = argc > 2 && argv[2][0] == 's';
    // 'i' 通过真实的平台入口推入一个 IME 输入组合。
    const bool want_ime = argc > 2 && argv[2][0] == 'i';
    const bool latin_primary = argc > 3 && argv[3][0] == 'l';
    const int W = 1000, H = 680;

    Context &c = ctx();
    c.init("Fox headless", W, H);

    // "latin" 模式：强制用只含拉丁字母的主字体（Segoe UI，本机上最接近
    // 项目目标 macOS 27 UI 字体的东西）。
    //
    // 默认的平台搜索优先选支持 CJK 的字面，主字体自己就覆盖 CJK，回退链
    // 从不触发。这让"字体回退可用"成了一句没被测试过的话——一个从没被
    // 观察到运行的机制。抽掉覆盖是让它显形的唯一办法，而这也是真实应用
    // 会发布的配置。
    if (latin_primary)
    {
        c.set_font_file("C:/Windows/Fonts/segoeui.ttf");
        std::fprintf(stderr, "[headless] primary forced to a Latin-only face\n");
    }

    // 固定时间。紧循环里的 frame_begin() 测出的 dt 只有微秒级，光栅化时
    // 所有动画都还停在起始值——参考图里 anim = 0（开关旋钮在左边、没有
    // 按压状态），跟真实状态无关。参考图不依赖机器速度，正是这个二进制
    // 的全部意义。
    c.set_fixed_dt(1.0f / 60.0f);

    std::vector<u8> sheet = build_icon_sheet(kCell * kIconCount, kCell);
    TexId icons = c.create_texture((u32)(kCell * kIconCount), (u32)kCell, sheet.data());

    CpuBackend cpu;
    if (!cpu.init(W, H))
    {
        std::fprintf(stderr, "[headless] buffer allocation failed\n");
        return 1;
    }

    bool g_switch = true, g_check = true;
    float g_slider = 0.62f, g_slider2 = 0.30f;
    char g_buf[128] = "Fox 图形框架";

    // 跑好几帧，多到动画能稳定下来：第一帧把字形光栅化进图集，所以之后
    // 那一帧的文字也必须正确。只渲染一次会漏掉"文字晚一帧出现"这一整类
    // bug。
    for (int f = 0; f < 60; ++f)
    {
        // 字体覆盖诊断，只在第 0 帧。
        //
        // 不知道主字面来自哪个文件、它是否声称覆盖该码点，"CJK 文字是豆腐
        // 块"就没法回答——而 load_system_ui 的候选列表有十几个路径，前面
        // 几个在本系统上根本不存在。这就是"回退坏了"和"主字体本来就有"
        // 的区别。
        if (f == 0)
        {
            const FontId primary = c.font(c.theme().font_body);
            std::fprintf(stderr, "[headless] primary=%s  A=%d  CJK(4F60)=%d  chain=%zu\n",
                         fonts().face_source(primary), fonts().has_glyph(primary, 'A') ? 1 : 0,
                         fonts().has_glyph(primary, 0x4F60) ? 1 : 0, fonts().fallback_chain().size());
            for (FontId fb : fonts().fallback_chain())
                std::fprintf(stderr, "[headless]   fallback=%s  CJK(4F60)=%d\n",
                             fonts().face_source(fb), fonts().has_glyph(fb, 0x4F60) ? 1 : 0);
        }

        c.frame_begin();

        Theme &th = c.theme();
        Rect win_rect{0.0f, 0.0f, (float)W, (float)H};
        Window w;
        w.begin(c, win_rect, "Fox", true);

        const char *labels[] = {"Overview", "Circuit", "Operators", "Backends", "Settings"};
        int selected = 1;
        SidebarItem items[5];
        for (int i = 0; i < 5; ++i)
        {
            items[i].label = labels[i];
            items[i].icon_tex = icons;
            items[i].icon_src = icon_cell(i % kIconCount);
            items[i].icon = nullptr;
        }
        int scroll = 0;
        sidebar(c, w.sidebar(), items, 5, selected, scroll);

        Rect cr = w.content();
        f32 pad = th.spacing_lg;
        Rect inner{cr.x + pad, cr.y + pad, cr.w - pad * 2.0f, cr.h - pad * 2.0f};

        label(c, Rect{inner.x, inner.y, inner.w, 28}, "Golden Gate Overview", th.text,
              Align::Start);
        c.dl().push(Rect{inner.x, inner.y + 32.0f, inner.w, th.hairline}, th.separator, 0.2f);

        f32 y = inner.y + 48.0f;

        // 图标行：着色、alpha 和拉伸，与窗口 demo 相同。
        {
            const f32 s = 28.0f;
            f32 ix = inner.x;
            for (int i = 0; i < kIconCount; ++i)
            {
                image_region(c, Rect{ix, y, s, s}, icons, icon_cell(i), th.accent_color);
                ix += s + 8.0f;
            }
            image_region(c, Rect{ix, y + 4.0f, 20.0f, 20.0f}, icons, icon_cell(1),
                         Color{th.accent_color.r, th.accent_color.g, th.accent_color.b, 102});
            image_region(c, Rect{ix + 28.0f, y, 64.0f, 40.0f}, icons, icon_cell(3), th.text);
            label(c, Rect{ix + 100.0f, y, 200.0f, s}, "icons", th.text_tertiary, Align::Start);
            y += s + th.spacing_md;
        }

        // 卡片，用与 draw_card 相同的阴影/圆角/细线配方。
        {
            const Rect r{inner.x, y, inner.w, 92.0f};
            const CornerRadii rad(10.0f);
            Shadow sh;
            sh.offset_y = 2.0f;
            sh.blur = 10.0f;
            sh.color = Color{0, 0, 0, 0x24};
            shadow_rect(c, r, rad, sh, 0.5f);
            c.dl().push_rounded(r, th.control_bg, rad, 1.0f);
            c.dl().push_border(r, th.separator, th.hairline, rad, 1.1f);
            f32 cx2 = r.x + th.spacing_lg;
            label(c, Rect{cx2, r.y + th.spacing_md, r.w - th.spacing_lg * 2, 20}, "Liquid Glass",
                  th.text, Align::Start);
            label(c, Rect{cx2, r.y + th.spacing_md + 24.0f, r.w - th.spacing_lg * 2, 40},
                  "Frosted material blurs its backdrop and adds a specular rim.",
                  th.text_secondary, Align::Start);
            y += r.h + th.spacing_md;
        }

        button(c, Rect{inner.x, y, 130, 32}, "Primary", ButtonStyle::Filled);
        button(c, Rect{inner.x + 140, y, 130, 32}, "Secondary");
        button(c, Rect{inner.x + 280, y, 130, 32}, "Disabled", ButtonStyle::Tinted, false);
        y += 44;

        toggle_switch(c, Rect{inner.x, y + 4, 42, 24}, g_switch);
        checkbox(c, Rect{inner.x + 80, y, 200, 24}, "Enable Physics", g_check);
        y += 36;
        slider(c, Rect{inner.x, y, inner.w - 200, 24}, g_slider, 0.f, 1.f);
        y += 36;
        slider(c, Rect{inner.x, y, inner.w - 200, 24}, g_slider2, 0.f, 1.f);
        y += 40;
        const Rect field_r{inner.x, y, 320, 28};
        text_field(c, field_r, g_buf, (int)sizeof(g_buf), "hint", 0);

        // 多行文本区，刻意装了比放得下更多的行，这样滚动条和滚动到光标
        // 的行为才真正被练到。内容放得下的文本区两者都没有，只测那种情况
        // 证明不了难的部分。
        y += 44;
        char area_buf[512] =
            "Fox \xE5\xA4\x9A\xE8\xA1\x8C\xE6\x96\x87\xE6\x9C\xAC\xE5\x9F\x9F\n"
            "second line\n"
            "third line\n"
            "fourth line\n"
            "fifth line\n"
            "sixth line\n"
            "seventh line\n"
            "eighth line\n"
            "ninth line\n"
            "tenth line - the caret starts BELOW the fold";
        const Rect area_r{inner.x, y, 320, 120};
        text_area(c, area_r, area_buf, (int)sizeof(area_buf), "notes", 0);

        // 把光标停在最后一行、屏幕外。这正是滚动到光标的意义：没有它，
        // 光标画在框外，用户等于对着空气打字。
        {
            const Id aid = c.make_id(0xB0000006u, (Id)id_hash_rect(area_r));
            c.focus().set_focus(aid);
            TextBuffer &atb = c.text_edit(aid);
            c.sync_text_from_host(atb, area_buf, strlen(area_buf));
            atb.set_caret(atb.size());
            if (f == 0)
            {
                const WidgetState &ws = c.state(aid);
                std::fprintf(stderr,
                             "[headless] text_area lines=%zu caret_line=%zu box(y=%.1f h=%.1f)\n",
                             atb.line_count(), atb.line_of(atb.caret()), area_r.y, area_r.h);
                std::fprintf(stderr, "[headless]   lh=%.2f pad_y=%.2f scroll=%.2f scroll_x=%.2f\n",
                             fonts().line_height(c.font(th.font_body)), th.spacing_md, ws.scroll,
                             ws.scroll_x);
            }
        }

        // 选区和 IME 从这里驱动，而不是合成输入事件——本二进制里没有事件
        // 泵，伪造一个等于测伪造的东西。两者都从真实平台 IME 会用的同一组
        // 入口进去，所以被测代码就是真实代码。
        //
        // 这对预编辑尤其重要：它走的绘制路径在这里没有别的办法执行，而从没
        // 跑过的渲染路径和不能用的渲染路径没有区别。
        if (want_selection || want_ime)
        {
            const Id fid = c.make_id(0xB0000005u, (Id)id_hash_rect(field_r));
            c.focus().set_focus(fid);
            TextBuffer &tb = c.text_edit(fid);
            if (want_selection)
            {
                // 选中 "Fox "——四个字节，其中一个是空格，逐字形的实现会
                // 在这里留出缝隙。
                tb.set_caret(0);
                tb.set_anchor(4);
            }
            if (want_ime)
            {
                c.sync_text_from_host(tb, g_buf, strlen(g_buf));
                c.ime_begin("nihao", 3);
                // 只在第 0 帧：这是状态断言，打六十遍会把真正要紧的诊断
                // 埋掉。
                if (f == 0)
                    std::fprintf(stderr, "[headless] ime composing=%d preedit=\"%s\"\n",
                                 c.ime_composing() ? 1 : 0, tb.preedit().c_str());
            }
        }

        c.frame_end();
        cpu.render(c.dl(), c);
    }

    const size_t painted = cpu.non_background_pixels();
    std::fprintf(stderr, "[headless] %dx%d instances=%u painted=%zu (%.1f%%)\n", W, H,
                 cpu.last_instance_count(), painted,
                 100.0 * (double)painted / (double)((size_t)W * H));

    if (!cpu.write_ppm(out))
    {
        std::fprintf(stderr, "[headless] write_ppm failed\n");
        return 1;
    }
    std::fprintf(stderr, "[headless] wrote %s\n", out);

    // 什么都没画出来的 UI 正是这个二进制存在要检测的失败，所以显式以非零
    // 退出，而不是只留一张没有任何提示的空图。
    if (painted < 1000)
    {
        std::fprintf(stderr, "[headless] FAIL: essentially nothing drawn\n");
        return 1;
    }
    std::fprintf(stderr, "[headless] OK\n");
    return 0;
}
