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
    constexpr int kCell = 32;
    constexpr int kIconCount = 4;

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
                    case 0:
                    {
                        const float dx = fx - 16.0f, dy = fy - 16.0f;
                        cov = std::sqrt(dx * dx + dy * dy) <= 12.0f ? 1.0f : 0.0f;
                        break;
                    }
                    case 1:
                    {
                        const float dx = fx - 16.0f, dy = fy - 16.0f;
                        const float d = std::sqrt(dx * dx + dy * dy);
                        cov = (d <= 12.0f && d >= 7.0f) ? 1.0f : 0.0f;
                        break;
                    }
                    case 2:
                    {
                        const float ty = fy / 26.0f;
                        if (ty <= 1.0f && std::fabs(fx - 16.0f) <= ty * 12.0f) cov = 1.0f;
                        break;
                    }
                    default:
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

    Rect icon_cell(int i)
    {
        const float w = 1.0f / (float)kIconCount;
        return Rect{(float)i * w, 0.0f, w, 1.0f};
    }

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
}

static void draw_card(Context &c, const Rect &r, const char *title, const char *body)
{
    Theme &th = c.theme();
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
    c.init("Fox", 1000, 680);

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

    std::vector<u8> pulse_px((size_t)kCell * kCell * 4, 0);
    TexId g_pulse = c.create_texture((u32)kCell, (u32)kCell, pulse_px.data());
    double next_pulse = 0.0;
    int pulse_step = 0;

    while (!backend.window_close_requested())
    {
        backend.poll_events(c);
        if (getenv("FOX_BEAT"))
            std::fprintf(stderr, "[beat] frame %d begin\n", c.frame_index());

        c.frame_begin();

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

        Rect cr = w.content();
        f32 pad = th.spacing_lg;
        Rect inner{cr.x + pad, cr.y + pad, cr.w - pad * 2.0f, cr.h - pad * 2.0f};

        label(c, Rect{inner.x, inner.y, inner.w, 28.0f}, "Golden Gate Overview", th.text,
              Align::Start);
        Rect rule{inner.x, inner.y + 32.0f, inner.w, th.hairline};
        c.dl().push(rule, th.separator, 0.2f);

        f32 y = inner.y + 48.0f;

        {
            const f32 s = 28.0f;
            f32 ix = inner.x;
            for (int i = 0; i < kIconCount; ++i)
            {
                image_region(c, Rect{ix, y, s, s}, g_icons, icon_cell(i), th.accent_color);
                ix += s + 8.0f;
            }
            image_region(c, Rect{ix, y + 4.0f, 20.0f, 20.0f}, g_icons, icon_cell(1),
                         Color{th.accent_color.r, th.accent_color.g, th.accent_color.b, 102});
            image_region(c, Rect{ix + 28.0f, y, 64.0f, 40.0f}, g_icons, icon_cell(3), th.text);
            ix += 100.0f;

            image_region(c, Rect{ix, y, s, s}, g_pulse, Rect{0, 0, 1, 1}, IMAGE_TINT);
            ix += s + 8.0f;

            label(c, Rect{ix, y, 200.0f, s}, "icons", th.text_tertiary, Align::Start);
            y += s + th.spacing_md;
        }

        draw_card(c, Rect{inner.x, y, inner.w, 92.0f}, "Liquid Glass",
                  "Frosted material blurs its backdrop, boosts saturation and adds a specular rim.");
        y += 92.0f + th.spacing_md;

        Rect b1{inner.x, y, 130.0f, th.control_height_large};
        Rect b2{b1.right() + th.spacing_sm, y, 130.0f, th.control_height_large};
        Rect b3{b2.right() + th.spacing_sm, y, 130.0f, th.control_height_large};
        if (button(c, b1, "Primary", ButtonStyle::Filled)) g_clicks++;
        if (button(c, b2, "Secondary", ButtonStyle::Tinted)) g_clicks++;
        button(c, b3, "Disabled", ButtonStyle::Bezel, false);
        y += th.control_height_large + th.spacing_md;

        f32 cy = y;
        toggle_switch(c, Rect{inner.x, cy + 2.0f, 42.0f, 24.0f}, g_switch);
        label(c, Rect{inner.x + 52.0f, cy, 160.0f, 28.0f}, "Enable physics", th.text, Align::Start);

        checkbox(c, Rect{inner.x + 230.0f, cy, 200.0f, 28.0f}, "Auto-layout", g_checkbox);
        checkbox(c, Rect{inner.x + 230.0f, cy + 28.0f, 200.0f, 28.0f}, "Glass materials", g_checkbox2);
        y += 66.0f;

        label(c, Rect{inner.x, y - 2.0f, 120.0f, 16.0f}, "Amplitude", th.text_secondary,
              Align::Start);
        slider(c, Rect{inner.x + 120.0f, y - 4.0f, inner.w - 120.0f, 22.0f}, g_slider, 0.0f, 1.0f);
        y += 30.0f;
        label(c, Rect{inner.x, y - 2.0f, 120.0f, 16.0f}, "Damping", th.text_secondary, Align::Start);
        slider(c, Rect{inner.x + 120.0f, y - 4.0f, inner.w - 120.0f, 22.0f}, g_slider2, 0.0f, 1.0f);
        y += 34.0f;

        text_field(c, Rect{inner.x, y, fx_min(inner.w, 420.0f), th.control_height_large}, g_buffer,
                   (int)sizeof(g_buffer), "Type here (supports 中文输入)");
        y += th.control_height_large + th.spacing_md;

        char stats[220];
        std::snprintf(stats, sizeof(stats),
                      "%.0f fps   instances/frame: %u   glass path: %s   clicks: %d",
                      g_fps, backend.last_instance_count(),
                      backend.used_blur_path() ? "on" : "off", g_clicks);
        draw_card(c, Rect{inner.x, y, inner.w, 74.0f}, "Diagnostics", stats);

        if (c.input().keys_pressed[(int)'t'])
        {
            c.set_theme(th.appearance == Appearance::Light ? Appearance::Dark : Appearance::Light,
                        th.accent);
        }

        c.frame_end();
        backend.render(c.dl(), c);
        if (getenv("FOX_BEAT"))
            std::fprintf(stderr, "[beat] frame %d rendered\n", c.frame_index());

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

        if (const char *ea = getenv("FOX_EXIT_AFTER"))
        {
            if (++exit_frames >= atoi(ea)) break;
        }

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