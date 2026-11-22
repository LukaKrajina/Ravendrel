#include "fox.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>


#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include "glfw3.h"










static int clipboard_roundtrip(void)
{

    static char saved[65536];
    const size_t saved_len = fox_clipboard_text(saved, sizeof saved);









    if (saved_len + 1 >= sizeof saved)
    {
        fprintf(stderr, "[ctest] clipboard exceeds the save buffer; skipping "
                        "(a restore would truncate it)\n");
        return 0;
    }



    if (saved_len == 0)
    {
        fox_clipboard_set_text("probe");
        char probe[64];
        if (fox_clipboard_text(probe, sizeof probe) == 0)
        {
            fprintf(stderr, "[ctest] no clipboard on this target, skipping\n");
            return 0;
        }
    }


    static const char kText[] = "Fox clipboard \xE5\x89\xAA\xE8\xB4\xB4\xE6\x9D\xBF";
    if (!fox_clipboard_set_text(kText))
    {
        fprintf(stderr, "[ctest] FAIL clipboard write refused\n");
        return 1;
    }

    char got[256];
    const size_t n = fox_clipboard_text(got, sizeof got);
    if (n != strlen(kText) || strcmp(got, kText) != 0)
    {
        fprintf(stderr, "[ctest] FAIL round-trip: got \"%s\" (%zu) want \"%s\" (%zu)\n", got, n,
                kText, strlen(kText));
        fox_clipboard_set_text(saved);
        return 1;
    }



    static const char kSplit[] = "abc\xE4\xBD\xA0" "def";
    fox_clipboard_set_text(kSplit);
    char small[6];
    const size_t sn = fox_clipboard_text(small, sizeof small);
    if (sn != 3 || strcmp(small, "abc") != 0)
    {
        fprintf(stderr, "[ctest] FAIL truncation split a codepoint: got \"%s\" (%zu)\n",
                small, sn);
        fox_clipboard_set_text(saved);
        return 1;
    }

    if (fox_clipboard_text(NULL, 0) != 0)
    {
        fprintf(stderr, "[ctest] FAIL cap==0 should report 0\n");
        fox_clipboard_set_text(saved);
        return 1;
    }

    fox_clipboard_set_text(saved);
    fprintf(stderr, "[ctest] clipboard round-trip OK (utf-8, truncation, cap=0)\n");
    return 0;
}








static int ime_roundtrip(fox_ctx *ctx)
{
    if (fox_ime_is_composing(ctx))
    {
        fprintf(stderr, "[ctest] FAIL a composition was already open\n");
        return 1;
    }


    const fox_id fid = fox_make_id_i(0xB0000005u, 1);
    fox_focus(ctx, fid);
    if (!fox_has_focus(ctx, fid))
    {
        fprintf(stderr, "[ctest] FAIL fox_focus did not take\n");
        return 1;
    }

    fox_ime_begin(ctx, "nihao", 3);
    if (!fox_ime_is_composing(ctx))
    {
        fprintf(stderr, "[ctest] FAIL nothing is composing after fox_ime_begin "
                        "(is the field focused and is the id right?)\n");
        return 1;
    }

    fox_ime_update(ctx, "nihaox", 6);
    fox_ime_cancel(ctx);
    if (fox_ime_is_composing(ctx))
    {
        fprintf(stderr, "[ctest] FAIL fox_ime_cancel did not cancel\n");
        return 1;
    }



    fox_focus(ctx, 0);
    fox_ime_begin(ctx, "abc", 3);
    if (fox_ime_is_composing(ctx))
    {
        fprintf(stderr, "[ctest] FAIL a composition opened with nothing focused\n");
        return 1;
    }
    fox_ime_commit(ctx);

    fprintf(stderr, "[ctest] ime begin/update/cancel + unfocused no-op OK\n");
    return 0;
}

int main(void)
{
    fox_config cfg;
    cfg.title = "Fox C ABI";
    cfg.width = 900;
    cfg.height = 600;

    fox_ctx *ctx = fox_create(&cfg);
    if (!ctx)
    {
        fprintf(stderr, "fox_create failed: %s\n", "no context");
        return 1;
    }

    if (!glfwInit())
    {
        fprintf(stderr, "glfwInit failed\n");
        fox_destroy(ctx);
        return 1;
    }
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    GLFWwindow *win = glfwCreateWindow(900, 600, "Fox C ABI", NULL, NULL);
    if (!win)
    {
        fprintf(stderr, "glfwCreateWindow failed\n");
        glfwTerminate();
        fox_destroy(ctx);
        return 1;
    }

    if (!fox_init_backend_vulkan(ctx, win))
    {
        fprintf(stderr, "fox_init_backend_vulkan failed: %s\n", fox_error(ctx));
        glfwDestroyWindow(win);
        glfwTerminate();
        fox_destroy(ctx);
        return 1;
    }

    char text[128];
    strcpy(text, "typed through the C ABI");
    int checked = 1, toggled = 0;
    float amount = 0.5f;

    int frames = 0;
    while (!fox_should_close(ctx) && frames < 5)
    {
        fox_poll_events(ctx);
        fox_begin_frame(ctx);

        {
            fox_rect win_rect;
            win_rect.x = 0.0f;
            win_rect.y = 0.0f;
            win_rect.w = (float)fox_width(ctx);
            win_rect.h = (float)fox_height(ctx);

            fox_window_begin(ctx, win_rect, "Fox C ABI", 1);

            fox_rect content = fox_window_content(ctx);
            fox_color accent = fox_theme_color(ctx, FOX_COLOR_ACCENT);
            float pad = fox_theme_metric(ctx, FOX_METRIC_SPACING_LG);

            fox_label(ctx,
                      (fox_rect){content.x + pad, content.y + pad, content.w - pad * 2, 28},
                      "C ABI smoke test", accent, FOX_ALIGN_START);

            float y = content.y + pad + 40.0f;

            (void)fox_button(ctx, (fox_rect){content.x + pad, y, 140, 30}, "Button",
                             FOX_BUTTON_FILLED, 1);
            (void)fox_button(ctx, (fox_rect){content.x + pad + 150, y, 140, 30}, "Disabled",
                             FOX_BUTTON_TINTED, 0);
            y += 42.0f;

            (void)fox_toggle_switch(ctx, (fox_rect){content.x + pad, y, 44, 24}, &toggled);
            (void)fox_checkbox(ctx, (fox_rect){content.x + pad + 80, y, 200, 24}, "Checkbox",
                               &checked);
            y += 36.0f;

            (void)fox_slider(ctx, (fox_rect){content.x + pad, y, 320, 24}, &amount, 0.0f, 1.0f,
                             FOX_SLIDER_CONTINUOUS, 0.0f);
            y += 36.0f;

            (void)fox_text_field(ctx, (fox_rect){content.x + pad, y, 320, 28}, text,
                                 (int)sizeof(text), "type here",
                                 fox_make_id_i(0xB0000005u, 1));
            y += 40.0f;

            fox_separator(ctx, (fox_rect){content.x + pad, y, content.w - pad * 2, 1}, 1);
            y += 14.0f;

            fox_panel(ctx, (fox_rect){content.x + pad, y, 240, 60},
                      fox_theme_color(ctx, FOX_COLOR_CONTROL_BG),
                      fox_theme_metric(ctx, FOX_METRIC_CORNER_PANEL), 0);
            fox_glass_bar(ctx,
                          (fox_rect){content.x + pad + 250, y, 240, 60},
                          fox_theme_metric(ctx, FOX_METRIC_CORNER_PANEL));


            {
                static unsigned char px[16 * 16 * 4];
                int i, j;
                fox_tex t;
                for (j = 0; j < 16; ++j)
                    for (i = 0; i < 16; ++i)
                    {
                        unsigned char v = (unsigned char)(((i / 4 + j / 4) & 1) ? 235 : 90);
                        unsigned char *p = &px[(j * 16 + i) * 4];
                        p[0] = v; p[1] = v; p[2] = v; p[3] = 255;
                    }
                t = fox_texture_create(ctx, 16, 16, px);
                if (t != 0)
                {
                    fox_image(ctx, (fox_rect){content.x + pad + 500, y, 32, 32}, t,
                              (fox_color){255, 255, 255, 255});

                    px[0] = px[1] = px[2] = 255;
                    fox_texture_update(ctx, t, 16, 16, px);
                }
            }


            {
                static const float shape[6] = {0.0f, 2.0f, 10.0f, 14.0f, 30.0f, -12.0f};
                const float bx = content.x + pad + 420.0f, by = y + 30.0f;
                float pts[6];
                int i;
                for (i = 0; i < 3; ++i)
                {
                    pts[i * 2] = bx + shape[i * 2];
                    pts[i * 2 + 1] = by + shape[i * 2 + 1];
                }
                if (!fox_path(ctx, pts, 3, 3.0f, accent, 1, 1))
                    fprintf(stderr, "[ctest] FAIL fox_path returned 0 for 3 points\n");
                if (fox_path(ctx, pts, 1, 3.0f, accent, 1, 1))
                    fprintf(stderr, "[ctest] FAIL fox_path accepted a 1-point path\n");





                {
                    static const unsigned char png[] = {
                        0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d,
                        0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x02,
                        0x08, 0x06, 0x00, 0x00, 0x00, 0x72, 0xb6, 0x0d, 0x24, 0x00, 0x00, 0x00,
                        0x12, 0x49, 0x44, 0x41, 0x54, 0x78, 0xda, 0x63, 0xf8, 0xcf, 0xc0, 0xf0,
                        0x1f, 0x0c, 0x81, 0x34, 0x18, 0x00, 0x00, 0x49, 0xc8, 0x09, 0xf7, 0x03,
                        0xd9, 0x64, 0xf1, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae,
                        0x42, 0x60, 0x82,
                    };
                    const fox_tex pt = fox_texture_create_png(ctx, png, sizeof(png));
                    if (pt == 0)
                        fprintf(stderr, "[ctest] FAIL fox_texture_create_png returned 0\n");
                    else
                        fox_image(ctx, (fox_rect){content.x + pad + 460.0f, y, 40, 40}, pt,
                                  (fox_color){255, 255, 255, 255});
                    if (fox_texture_create_png(ctx, "not a png", 9) != 0)
                        fprintf(stderr, "[ctest] FAIL bogus bytes accepted as a PNG\n");
                }


                {
                    int b = -1, s = -1, a = -1;
                    fox_set_quality(ctx, 1, 0, 1);
                    fox_get_quality(ctx, &b, &s, &a);
                    if (b != 1 || s != 0 || a != 1)
                        fprintf(stderr, "[ctest] FAIL quality round-trip: %d %d %d\n", b, s, a);
                }
            }
        }

        fox_end_frame(ctx);
        frames++;
    }

    fprintf(stderr, "[ctest] frames=%d instances=%u blur=%d scale=%.2f size=%dx%d\n", frames,
            fox_last_instance_count(ctx), fox_used_blur_path(ctx), fox_scale(ctx),
            fox_width(ctx), fox_height(ctx));

    if (!fox_dump_ppm(ctx, getenv("FOX_C_DUMP") ? getenv("FOX_C_DUMP") : "fox_c_test.ppm"))
    {
        fprintf(stderr, "[ctest] dump_ppm failed\n");
        glfwDestroyWindow(win);
        glfwTerminate();
        fox_destroy(ctx);
        return 1;
    }


    if (clipboard_roundtrip() != 0)
    {
        glfwDestroyWindow(win);
        glfwTerminate();
        fox_destroy(ctx);
        return 1;
    }
    if (ime_roundtrip(ctx) != 0)
    {
        glfwDestroyWindow(win);
        glfwTerminate();
        fox_destroy(ctx);
        return 1;
    }




    fox_destroy(ctx);
    glfwDestroyWindow(win);
    glfwTerminate();
    fprintf(stderr, "[ctest] OK\n");
    return 0;
}