// Fox demo —— 框架的 macOS 27（Golden Gate）风格展示。
//
//   fox_demo        启动展示程序
//
// 演示：玻璃标题栏、红绿灯按钮、鲜艳侧边栏、按钮、开关、勾选框、滑块、文本框、
// 分区卡片和实时诊断条。按 'T' 键或工具栏控件切换外观。

#include "core/context.hpp"
#include "widget/primitives.hpp"
#include "window/window.hpp"
#include "backend/vk_backend.hpp"
#include <cstdio>
#include <cstring>
#include <cmath>
#include <vector>

using namespace fox;

namespace
{
    // ── 图标图集 ──────────────────────────────────────────────────────────
    //
    // 一张 128x32 RGBA8 纹理里放 4 个 32x32 单元，画成透明底白色图形。因为是
    // 白色，着色只需相乘——与 macOS 强调色用的是同一机制。
    //
    // 它取代了侧边栏过去画的 "◆ ◇" 文字占位符。那些不是图标，只是碰巧像图形
    // 的字符，所以没法正确着色或缩放。
    constexpr int kCell = 32;
    constexpr int kIconCount = 4;

    /// 构建图集。图标用解析式画出而不是打包成 PNG，这样 demo 就没有自己的
    /// 资源加载路径，也就没得错。
    ///
    /// ⚠ RGBA = 每像素 4 字节。分配 w*h 却让 create_texture() 读 w*h*4，是 4 倍
    ///   堆越界读；demo 恰好犯过这个错，只表现为进程退出时偶发堆损坏，没有校验
    ///   消息，渲染循环也不崩溃。
    std::vector<u8> build_icon_sheet(int w, int h)
    {
        std::vector<u8> px((size_t)w * h * 4, 0);

        for (int cell = 0; cell < kIconCount; ++cell)
        {
            const int ox = cell * kCell;
            for (int y = 0; y < kCell; ++y)
            {
                for (int x = 0; x < kCell; ++x)
                {
                    const float fx = (float)x + 0.5f, fy = (float)y + 0.5f;
                    float cov = 0.0f;
                    switch (cell)
                    {
                    case 0: // 实心圆盘
                    {
                        const float dx = fx - 16.0f, dy = fy - 16.0f;
                        cov = std::sqrt(dx * dx + dy * dy) <= 12.0f ? 1.0f : 0.0f;
                        break;
                    }
                    case 1: // 圆环
                    {
                        const float dx = fx - 16.0f, dy = fy - 16.0f;
                        const float d = std::sqrt(dx * dx + dy * dy);
                        cov = (d <= 12.0f && d >= 7.0f) ? 1.0f : 0.0f;
                        break;
                    }
                    case 2: // 朝上三角
                    {
                        const float ty = fy / 26.0f;
                        if (ty <= 1.0f && std::fabs(fx - 16.0f) <= ty * 12.0f) cov = 1.0f;
                        break;
                    }
                    default: // 圆角方框轮廓
                    {
                        const float dx = std::fabs(fx - 16.0f), dy = std::fabs(fy - 16.0f);
                        const float m = 13.0f;
                        const bool inOuter = dx <= m && dy <= m;
                        const bool inInner = dx <= m - 4.0f && dy <= m - 4.0f;
                        cov = (inOuter && !inInner) ? 1.0f : 0.0f;
                        break;
                    }
                    }
                    if (cov <= 0.0f) continue;
                    // 透明底白色：单色图标才能被强调色着色，macOS 图标就是这么做的。
                    const size_t o = ((size_t)(y) * w + (size_t)(ox + x)) * 4;
                    px[o + 0] = 255;
                    px[o + 1] = 255;
                    px[o + 2] = 255;
                    px[o + 3] = (u8)(cov * 255.0f);
                }
            }
        }
        return px;
    }

    /// 上面图集中图标 i 的归一化单元矩形。
    Rect icon_cell(int i)
    {
        const float w = 1.0f / (float)kIconCount;
        return Rect{(float)i * w, 0.0f, w, 1.0f};
    }

    // 侧边栏条目。所有图标来自同一张共享图集，整张列表只花一张纹理和一个
    // 描述符集。
    const int kItemCount = 5;

    int g_selected = 0;
    bool g_switch = true;
    bool g_checkbox = false;
    bool g_checkbox2 = true;
    float g_slider = 0.62f;
    float g_slider2 = 0.30f;
    static float g_fps = 0.0f;
    char g_buffer[128] = "Fox 图形框架";
    int g_clicks = 0;
} // namespace

static void draw_card(Context &c, const Rect &r, const char *title, const char *body)
{
    Theme &th = c.theme();
    // 卡片：不透明填充 + 柔和阴影 + 发丝线，macOS 分组内容的观感。
    CornerRadii rad(10.0f);

    Cmd sh;
    sh.type = CmdType::Rect;
    sh.rect = Rect{r.x, r.y + 2.0f, r.w, r.h};
    sh.radii = rad;
    sh.shadow.blur = 10.0f;
    sh.shadow.color = Color{0, 0, 0,
                            (u8)(th.appearance == Appearance::Dark ? 0x66 : 0x24)};
    sh.color = sh.shadow.color;
    sh.z = 0.5f;
    c.dl().push(sh);

    c.dl().push_rounded(r, th.control_bg, rad, 1.0f);
    c.dl().push_border(r, th.separator, th.hairline, rad, 1.1f);

    f32 x = r.x + th.spacing_lg;
    f32 y = r.y + th.spacing_md;
    label(c, Rect{x, y, r.w - th.spacing_lg * 2, 20}, title, th.text, Align::Start);
    y += 24.0f;
    label(c, Rect{x, y, r.w - th.spacing_lg * 2, r.h - 40}, body, th.text_secondary, Align::Start);
}

int main()
{
    if (!glfwInit())
    {
        std::fprintf(stderr, "glfwInit failed\n");
        return 1;
    }
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);

    // FOX_SCALE 在没有 HiDPI 屏的机器上模拟 HiDPI。窗口必须随它一起变大，就像
    // 在真 2x 屏上一样：scale 2 时 1000x680 逻辑窗口对应 2000x1360 帧缓冲。只设
    // scale 不设窗口大小的话，逻辑窗口会缩一半，整个 UI 会"正确地"——但毫无用处
    // 地——看起来大一倍。
    float forced_scale = 0.0f;
    if (const char *fs = getenv("FOX_SCALE"))
    {
        const float v = (float)atof(fs);
        if (v > 0.0f) forced_scale = v;
    }
    const int win_w = (int)(1000.0f * forced_scale + 0.5f);
    const int win_h = (int)(680.0f * forced_scale + 0.5f);

    GLFWwindow *win = glfwCreateWindow(forced_scale > 0.0f ? win_w : 1000,
                                       forced_scale > 0.0f ? win_h : 680, "Fox — 狐", nullptr,
                                       nullptr);
    if (!win)
    {
        std::fprintf(stderr, "glfwCreateWindow failed\n");
        glfwTerminate();
        return 1;
    }

    Context &c = ctx();
    c.init("Fox", 1000, 680); // 始终是逻辑尺寸

    // UI 里所有图标共用一张纹理。首帧上传一次后不再改动，这才是应有的稳态：
    // 一张纹理的代价是一次上传加一个描述符集，不是每次绘制一次。
    const int sheet_w = kCell * kIconCount, sheet_h = kCell;
    std::vector<u8> sheet_px = build_icon_sheet(sheet_w, sheet_h);
    TexId g_icons = c.create_texture((u32)sheet_w, (u32)sheet_h, sheet_px.data());

    VkBackend backend;
    if (!backend.init(win, c))
    {
        std::fprintf(stderr, "Fox backend init failed\n");
        glfwDestroyWindow(win);
        glfwTerminate();
        return 1;
    }

    int frames = 0;
    float fps_acc = 0.0f;
    int dump_frames = 0;
    int atlas_frames = 0;
    int exit_frames = 0;

    // 一张 32x32 纹理，内容每秒变几次，让重传路径保持被锻炼。刻意按定时器而不是
    // 每帧更新：每帧重写虽然合法，但会把静态资源变成每帧上传——这个坑不该由
    // demo 教给人。
    std::vector<u8> pulse_px((size_t)kCell * kCell * 4, 0);
    TexId g_pulse = c.create_texture((u32)kCell, (u32)kCell, pulse_px.data());
    double next_pulse = 0.0;
    int pulse_step = 0;

    // 窗口自己请求关闭（旧的 backend.should_close() 把「没有窗口」也算成
    // 「想关闭」，已拆成 has_window() 与 window_close_requested()）。
    while (!backend.window_close_requested())
    {
        backend.poll_events(c);
        // FOX_BEAT 每帧打印一行。进程死掉前打印的最后一行就是崩溃位置，这比反复
        // 读拆卸路径然后瞎猜有用得多。
        if (getenv("FOX_BEAT"))
            std::fprintf(stderr, "[beat] frame %d begin\n", c.frame_index());

        c.frame_begin();

        // 每秒重写脉冲纹理几次：扫过的位置让重传看得见。
        if ((double)c.time() >= next_pulse && g_pulse != TEX_NONE)
        {
            next_pulse = (double)c.time() + 0.18;
            const float phase = (float)(pulse_step % 24) / 24.0f;
            ++pulse_step;
            for (int y = 0; y < kCell; ++y)
            {
                for (int x = 0; x < kCell; ++x)
                {
                    const float fx = (float)x + 0.5f, fy = (float)y + 0.5f;
                    const float dx = fx - 16.0f, dy = fy - 16.0f;
                    const float d = std::sqrt(dx * dx + dy * dy);
                    // 圆环，带一条绕行的亮弧。
                    float v = (d <= 13.0f && d >= 9.0f) ? 0.35f : 0.0f;
                    const float ang = std::atan2(dy, dx) / 6.2831853f + 0.5f;
                    float dist = ang - phase;
                    while (dist > 0.5f) dist -= 1.0f;
                    while (dist < -0.5f) dist += 1.0f;
                    if (v > 0.0f && std::fabs(dist) < 0.09f) v = 1.0f;
                    const u8 a = (u8)(v * 255.0f);
                    const size_t o = ((size_t)y * kCell + (size_t)x) * 4;
                    pulse_px[o + 0] = a;
                    pulse_px[o + 1] = a;
                    pulse_px[o + 2] = a;
                    pulse_px[o + 3] = a;
                }
            }
            c.update_texture(g_pulse, (u32)kCell, (u32)kCell, pulse_px.data());
            c.request_redraw();
        }

        Theme &th = c.theme();
        const Rect win_rect{0.0f, 0.0f, (float)c.width(), (float)c.height()};

        Window w;
        w.begin(c, win_rect, "Fox", true);

        // ── 侧边栏 ────────────────────────────────────────────────────
        // 每帧构建，这样图集 TexId（启动时创建一次）才可用；数组本身只是五个小
        // 结构体，相对它喂给的绘制调用开销可以忽略。
        static const char *kLabels[kItemCount] = {"Overview", "Circuit", "Operators", "Backends",
                                                  "Settings"};
        SidebarItem items[kItemCount];
        for (int i = 0; i < kItemCount; ++i)
        {
            items[i].label = kLabels[i];
            items[i].icon_tex = g_icons;
            items[i].icon_src = icon_cell(i % kIconCount);
            items[i].icon = nullptr;
        }

        int scroll = 0;
        int clicked = sidebar(c, w.sidebar(), items, kItemCount, g_selected, scroll);
        if (clicked >= 0) g_selected = clicked;

        // ── 内容区 ────────────────────────────────────────────────────
        Rect cr = w.content();
        f32 pad = th.spacing_lg;
        Rect inner{cr.x + pad, cr.y + pad, cr.w - pad * 2.0f, cr.h - pad * 2.0f};

        // 区块标题
        label(c, Rect{inner.x, inner.y, inner.w, 28.0f}, "Golden Gate Overview", th.text,
              Align::Start);
        Rect rule{inner.x, inner.y + 32.0f, inner.w, th.hairline};
        c.dl().push(rule, th.separator, 0.2f);

        f32 y = inner.y + 48.0f;

        // ── 图标 ──────────────────────────────────────────────────────
        {
            const f32 s = 28.0f;
            f32 ix = inner.x;
            for (int i = 0; i < kIconCount; ++i)
            {
                image_region(c, Rect{ix, y, s, s}, g_icons, icon_cell(i), th.accent_color);
                ix += s + 8.0f;
            }
            // 强调色 40% 透明度：验证 alpha 是相乘而不是替换，禁用行需要的正是
            // 这种效果。
            image_region(c, Rect{ix, y + 4.0f, 20.0f, 20.0f}, g_icons, icon_cell(1),
                         Color{th.accent_color.r, th.accent_color.g, th.accent_color.b, 102});
            // 拉伸：把 32x32 单元画进 64x40 矩形，验证四边形把源矩形映满整个
            // 目标。
            image_region(c, Rect{ix + 28.0f, y, 64.0f, 40.0f}, g_icons, icon_cell(3), th.text);
            ix += 100.0f;

            // 第二张纹理，像素按定时器变化。它锻炼 Context::update_texture 和
            // 后端的重传路径（静态图集碰不到这些）。实时图表、视频缩略图、加载
            // 动画都依赖它，而悄悄过期的重传看起来和静态图一模一样——所以把它
            // 放上屏幕，而不是只写在注释里。
            image_region(c, Rect{ix, y, s, s}, g_pulse, Rect{0, 0, 1, 1}, IMAGE_TINT);
            ix += s + 8.0f;

            label(c, Rect{ix, y, 200.0f, s}, "icons", th.text_tertiary, Align::Start);
            y += s + th.spacing_md;
        }

        // 卡片：说明
        draw_card(c, Rect{inner.x, y, inner.w, 92.0f}, "Liquid Glass",
                  "Frosted material blurs its backdrop, boosts saturation and adds a specular rim.");
        y += 92.0f + th.spacing_md;

        // 一排按钮
        Rect b1{inner.x, y, 130.0f, th.control_height_large};
        Rect b2{b1.right() + th.spacing_sm, y, 130.0f, th.control_height_large};
        Rect b3{b2.right() + th.spacing_sm, y, 130.0f, th.control_height_large};
        if (button(c, b1, "Primary", ButtonStyle::Filled)) g_clicks++;
        if (button(c, b2, "Secondary", ButtonStyle::Tinted)) g_clicks++;
        button(c, b3, "Disabled", ButtonStyle::Bezel, false);
        y += th.control_height_large + th.spacing_md;

        // 控件行：开关 + 勾选框
        f32 cy = y;
        toggle_switch(c, Rect{inner.x, cy + 2.0f, 42.0f, 24.0f}, g_switch);
        label(c, Rect{inner.x + 52.0f, cy, 160.0f, 28.0f}, "Enable physics", th.text, Align::Start);

        checkbox(c, Rect{inner.x + 230.0f, cy, 200.0f, 28.0f}, "Auto-layout", g_checkbox);
        checkbox(c, Rect{inner.x + 230.0f, cy + 28.0f, 200.0f, 28.0f}, "Glass materials", g_checkbox2);
        y += 66.0f;

        // 滑块
        label(c, Rect{inner.x, y - 2.0f, 120.0f, 16.0f}, "Amplitude", th.text_secondary,
              Align::Start);
        slider(c, Rect{inner.x + 120.0f, y - 4.0f, inner.w - 120.0f, 22.0f}, g_slider, 0.0f, 1.0f);
        y += 30.0f;
        label(c, Rect{inner.x, y - 2.0f, 120.0f, 16.0f}, "Damping", th.text_secondary, Align::Start);
        slider(c, Rect{inner.x + 120.0f, y - 4.0f, inner.w - 120.0f, 22.0f}, g_slider2, 0.0f, 1.0f);
        y += 34.0f;

        // 文本框（支持 CJK）
        text_field(c, Rect{inner.x, y, fx_min(inner.w, 420.0f), th.control_height_large}, g_buffer,
                   (int)sizeof(g_buffer), "Type here (supports 中文输入)");
        y += th.control_height_large + th.spacing_md;

        // 诊断卡片
        char stats[220];
        std::snprintf(stats, sizeof(stats),
                      "%.0f fps   instances/frame: %u   glass path: %s   clicks: %d",
                      g_fps, backend.last_instance_count(),
                      backend.used_blur_path() ? "on" : "off", g_clicks);
        draw_card(c, Rect{inner.x, y, inner.w, 74.0f}, "Diagnostics", stats);

        // 快捷键：'t' 切换外观。可打印键直接用 ASCII 码（见 input.hpp），
        // 所以没有 Key::T。
        if (c.input().keys_pressed[(int)'t'])
        {
            // 走 Context 而不是自由函数 set_theme()：Context 之后会重新应用
            // DPI 缩放，自由函数会把所有长度记号悄悄重置回 1x。
            c.set_theme(th.appearance == Appearance::Light ? Appearance::Dark : Appearance::Light,
                        th.accent);
        }

        c.frame_end();
        backend.render(c.dl(), c);
        if (getenv("FOX_BEAT"))
            std::fprintf(stderr, "[beat] frame %d rendered\n", c.frame_index());

        // FOX_ATLAS=<path.ppm> 把正文字体的 CPU 端字形图集放大到单像素可见后写出，
        // 然后退出。
        //
        // 这能把文字出错的两种情况分开——它们在屏幕上看起来一模一样："图集里存的
        // 是豆腐块"对"UV 指错单元"。只输出左上角：货架装箱从左往右填，用到的字形
        // 都落在前几百像素内。
        if (const char *adump = getenv("FOX_ATLAS"))
        {
            if (++atlas_frames >= 4)
            {
                const GlyphAtlas *a = fonts().atlas(c.default_font());
                if (a)
                {
                    const int aw = a->width(), ah = a->height();
                    const std::vector<u8> &px = a->pixels();
                    const int kZoom = 4, kSpan = 320;
                    FILE *f = nullptr;
                    if (fopen_s(&f, adump, "wb") == 0 && f)
                    {
                        fprintf(f, "P6\n%d %d\n255\n", kSpan * kZoom, kSpan * kZoom);
                        for (int y = 0; y < kSpan * kZoom; ++y)
                            for (int x = 0; x < kSpan * kZoom; ++x)
                            {
                                const int sx = x / kZoom, sy = y / kZoom;
                                // 黑底白字，覆盖率一目了然。
                                const u8 v =
                                    (sx < aw && sy < ah) ? px[(size_t)sy * aw + sx] : 0;
                                fputc(v, f); fputc(v, f); fputc(v, f);
                            }
                        fclose(f);
                    }
                    fprintf(stderr, "[Fox] atlas %dx%d, %zu glyphs -> %s\n", aw, ah,
                            a->glyph_count(), adump);
                }
                break;
            }
        }

        // FOX_EXIT_AFTER=N 在 N 帧后跳出循环，给出一条不经过 dump_ppm 的干净退出
        // 路径。与 FOX_DUMP 对比即可区分"回读工具把设备拆坏了"和"图像路径的某处
        // 把设备弄不稳定了"。
        if (const char *ea = getenv("FOX_EXIT_AFTER"))
        {
            if (++exit_frames >= atoi(ea)) break;
        }

        // FOX_DUMP=<path.ppm> 写出真实交换链的 GPU 端回读并退出。这是真值检验：
        // 窗口截图可能被遮挡或在调整大小中途截取，那样"UI 是空白的"就成了无法
        // 证明的说法。改为从显存回读。
        //
        // FOX_DUMP_FRAMES 覆盖先等待的帧数——要抓定时器驱动的东西（比如本 demo
        // 的动画纹理）时需要它。
        if (const char *dump = getenv("FOX_DUMP"))
        {
            int want = 4;
            if (const char *df = getenv("FOX_DUMP_FRAMES")) want = atoi(df);
            if (++dump_frames >= want)
            {
                backend.dump_ppm(dump);
                break;
            }
        }

        // FPS 统计，显示在下面的诊断卡片里。
        fps_acc += c.dt();
        ++frames;
        if (fps_acc >= 0.5f)
        {
            g_fps = (float)frames / fps_acc;
            frames = 0;
            fps_acc = 0.0f;
        }

        if (w.close_requested()) break;
    }

    const bool beat = getenv("FOX_BEAT") != nullptr;
    // ? 顺序很重要：先拆后端。
    //
    //   VkBackend::shutdown() 销毁 VkSurfaceKHR 和窗口创建时所用的 VkInstance，
    //   Vulkan 要求这两者都先于 instance 消失。所以窗口必须在那之后销毁，不能
    //   在之前：正序会泄漏 surface（VUID-vkDestroyInstance-instance-00629，每次
    //   关机报一次），倒序则让窗口引用已经不存在的对象。
    if (beat) std::fprintf(stderr, "[beat] backend.shutdown\n");
    backend.shutdown();
    if (beat) std::fprintf(stderr, "[beat] glfwDestroyWindow\n");
    glfwDestroyWindow(win);
    if (beat) std::fprintf(stderr, "[beat] glfwTerminate\n");
    glfwTerminate();
    if (beat) std::fprintf(stderr, "[beat] fonts.shutdown\n");
    fonts().shutdown();
    if (beat) std::fprintf(stderr, "[beat] teardown complete\n");
    return 0;
}
