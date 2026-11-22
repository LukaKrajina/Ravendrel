#include "qhal/ClassicGui.hpp"
#include "ClassicRoute.hpp"
#include "qhal/FoxHost.hpp"
#include "utils/A2a.hpp"
#include "gui/src/render_context.h"
#include "nuklear_config.h"
#include <cstring>
#include <cstdio>
#include <chrono>
#ifdef _WIN32
extern "C" __declspec(dllimport) int __stdcall Beep(unsigned long dwFreq, unsigned long dwDuration);
#endif

namespace
{
    std::chrono::steady_clock::time_point g_frame_begin;
    std::chrono::steady_clock::time_point g_last_begin;
    std::chrono::steady_clock::time_point g_last_begin_prev;
    double g_qk_us = 0, g_render_us = 0, g_gap_us = 0;
    int g_prof_frames = 0;
    int g_begin_count = 0;
    void prof_report()
    {
        if (++g_prof_frames >= 20)
        {
            std::fprintf(stderr, "[PROF] gap=%.0fus rvdl=%.0fus render=%.0fus (%.1f fps)\n",
                         g_gap_us / g_prof_frames, g_qk_us / g_prof_frames,
                         g_render_us / g_prof_frames,
                         1000000.0 / (g_gap_us / g_prof_frames));
            g_qk_us = g_render_us = g_gap_us = 0;
            g_prof_frames = 0;
        }
    }


    void fox_report(const char *line, void *user)
    {
        (void)user;
        std::fprintf(stderr, "[cgui] %s\n", line ? line : "");
    }

    struct WinRef
    {
        GLFWwindow *win = nullptr;
    };

    void *glfw_create(int w, int h, const char *title, void *user)
    {
        glfwDefaultWindowHints();
        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
        glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
        GLFWwindow *win = glfwCreateWindow(w, h, title ? title : "Classic GUI", nullptr, nullptr);
        if (win) static_cast<WinRef *>(user)->win = win;
        return win;
    }

    void glfw_destroy(void *window, void *user)
    {
        static_cast<WinRef *>(user)->win = nullptr;
        glfwDestroyWindow(static_cast<GLFWwindow *>(window));
    }

    struct Route;
    void sync_quality(Route &r, bool force);

    struct Route
    {
        qhal::FoxApi api = qhal::fox_real_api();
        WinRef winref;
        qhal::FoxHost host;

        qgui::RenderContext *nk = nullptr;
        bool decided = false;
        bool fox = false;
        int w = 640, h = 480;
        char title[256] = "Classic GUI";

        a2a::Quality quality{};
        bool have_quality = false;
        int quality_countdown = 0;

        bool start(int width, int height, const char *t)
        {
            w = width;
            h = height;
            std::snprintf(title, sizeof(title), "%s", t ? t : "Classic GUI");
            decided = true;

            if (!glfwInit())
            {
                std::fprintf(stderr,
                             "[cgui] glfwInit failed; fox skipped (no window backend), "
                             "using Nuklear\n");
            }
            else
            {
                qhal::FoxHost::WindowFactory wf;
                wf.create = &glfw_create;
                wf.destroy = &glfw_destroy;
                wf.user = &winref;

                qhal::FoxHost::Config cfg;
                cfg.width = w;
                cfg.height = h;
                cfg.title = title;
                cfg.report = &fox_report;
                cfg.fault_threshold = 3;

                fox = host.init(api, wf, cfg);
            }

            if (!fox) return nuklear_ready();

            const char *gpu = host.gpu_name();
            a2a::report_gpu(gpu ? gpu : "", host.gpu_vendor());
            sync_quality(*this, true);
            std::fprintf(stderr, "[cgui] renderer: fox (GPU '%s', vendor 0x%04X, %dx%d logical)\n",
                         gpu && *gpu ? gpu : "?", host.gpu_vendor(), w, h);
            return true;
        }

        bool nuklear_ready()
        {
            if (nk) return true;
            auto *rc = new qgui::RenderContext();
            if (!rc->init(title, w, h))
            {
                delete rc;
                std::fprintf(stderr, "[cgui] Nuklear was the fallback and it also failed: "
                                     "no renderer\n");
                return false;
            }
            nk = rc;
            qhal::cgui_state().impl = rc;
            std::fprintf(stderr, "[cgui] renderer: Nuklear\n");
            return true;
        }
    };

    Route &route()
    {
        static Route *r = new Route();
        return *r;
    }

    void sync_quality(Route &r, bool force)
    {
        const a2a::Quality q = a2a::current_quality();
        const int blur = (int)q.blur;
        const int shadows = (int)q.shadows;
        const int anim = q.animations ? 1 : 0;
        if (!force && r.have_quality && blur == (int)r.quality.blur &&
            shadows == (int)r.quality.shadows && anim == (r.quality.animations ? 1 : 0))
            return;
        r.quality = q;
        r.have_quality = true;
        r.host.set_quality(blur, shadows, anim);
        std::fprintf(stderr, "[cgui] quality: blur=%d shadows=%d animations=%d (thermal tier %s)\n",
                     blur, shadows, anim, a2a::tier_name(q.tier));
    }

    void switch_to_nuklear(Route &r)
    {
        std::fprintf(stderr, "[cgui] switching off fox: %s\n",
                     r.host.reason()[0] ? r.host.reason() : "unknown");
        r.host.shutdown();
        r.fox = false;
        r.nuklear_ready();
    }
}

namespace qhal
{
    namespace classic_route
    {
        bool fox_active() { return route().fox; }
        void rect(float x, float y, float w, float h, int32_t rgb, int32_t alpha)
        {
            route().host.fill_rect((int)x, (int)y, (int)w, (int)h, rgb, alpha);
        }
        void line(float x0, float y0, float x1, float y1, float thickness, int32_t rgb,
                  int32_t alpha)
        {
            route().host.stroke_line(x0, y0, x1, y1, thickness, rgb, alpha);
        }
        void polyline(const float *xy, int count, float thickness, int32_t rgb, int32_t alpha)
        {
            route().host.stroke_polyline(xy, count, thickness, rgb, alpha);
        }
        void ellipse(float cx, float cy, float rx, float ry, int32_t rgb, int32_t alpha)
        {
            route().host.fill_ellipse(cx, cy, rx, ry, rgb, alpha);
        }
        void triangle(float x0, float y0, float x1, float y1, float x2, float y2, int32_t rgb,
                      int32_t alpha)
        {
            route().host.fill_triangle(x0, y0, x1, y1, x2, y2, rgb, alpha);
        }
    }
}

extern "C"
{
    RVDL_HOST_EXPORT int32_t rvdl_cgui_init(int32_t w, int32_t h, const char *title)
    {
        auto &s = qhal::cgui_state();
        if (s.initialized)
            return 1;
        Route &r = route();
        s.width = w > 0 ? w : 640;
        s.height = h > 0 ? h : 480;
        if (title)
            strncpy(s.title, title, sizeof(s.title) - 1);
        if (!r.start(s.width, s.height, s.title))
            return 0;
        s.initialized = 1;
        return 1;
    }

    RVDL_HOST_EXPORT void *rvdl_cgui_fox_ctx()
    {
        Route &r = route();
        if (!r.fox) return nullptr;
        return (void *)r.host.fox_ctx_handle();
    }

    RVDL_HOST_EXPORT int32_t rvdl_cgui_should_close()
    {
        static int sc_count = 0;
        if (++sc_count % 500 == 0)
            std::fprintf(stderr, "[PROF] should_close calls: %d\n", sc_count);
        Route &r = route();
        if (r.fox)
            return r.host.should_close() ? 1 : 0;
        auto *rc = static_cast<qgui::RenderContext *>(qhal::cgui_state().impl);
        if (!rc)
            return 1;
        return rc->should_close() ? 1 : 0;
    }

    RVDL_HOST_EXPORT void rvdl_cgui_begin_frame()
    {
        auto now = std::chrono::steady_clock::now();
        g_gap_us += std::chrono::duration_cast<std::chrono::microseconds>(now - g_last_begin).count();
        g_last_begin = now;
        g_frame_begin = now;
        auto &s = qhal::cgui_state();
        Route &r = route();

        if (r.fox)
        {
            if (r.host.active())
            {
                r.host.begin_frame();
                if (--r.quality_countdown <= 0)
                {
                    r.quality_countdown = 60;
                    sync_quality(r, false);
                }
                {
                    int n = ++g_begin_count;
                    if (n <= 200)
                    {
                        double gapMs = (now - g_last_begin_prev).count() / 1e6;
                        std::fprintf(stderr, "[F] frame %d gap %.2fms\n", n, gapMs);
                    }
                }
                g_last_begin_prev = now;
                return;
            }
            switch_to_nuklear(r);
            if (!r.nk)
                return;
        }

        auto *rc = static_cast<qgui::RenderContext *>(s.impl);
        if (!rc)
            return;
        rc->begin_frame();
        nk_begin(rc->ctx(), s.title,
                 nk_rect(0, 0, (float)s.width, (float)s.height),
                 NK_WINDOW_BORDER | NK_WINDOW_NO_SCROLLBAR | NK_WINDOW_NO_INPUT);
        {
            int n = ++g_begin_count;
            if (n <= 200)
            {
                double gapMs = (now - g_last_begin_prev).count() / 1e6;
                std::fprintf(stderr, "[F] frame %d gap %.2fms\n", n, gapMs);
            }
        }
        g_last_begin_prev = now;
    }

    RVDL_HOST_EXPORT void rvdl_cgui_end_frame()
    {
        auto t0 = std::chrono::steady_clock::now();
        auto &s = qhal::cgui_state();
        Route &r = route();
        if (r.fox)
        {
            r.host.end_frame();
        }
        else
        {
            auto *rc = static_cast<qgui::RenderContext *>(s.impl);
            if (!rc)
                return;
            nk_end(rc->ctx());
            rc->end_frame();
        }
        auto t1 = std::chrono::steady_clock::now();
        g_qk_us += std::chrono::duration_cast<std::chrono::microseconds>(t0 - g_frame_begin).count();
        g_render_us += std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
        prof_report();
    }

    RVDL_HOST_EXPORT int32_t rvdl_cgui_button(const char *label)
    {
        std::fprintf(stderr, "[BTN>] %s\n", label ? label : "(null)");
        Route &r = route();
        int32_t result = 0;
        if (r.fox)
        {
            result = r.host.button(label) ? 1 : 0;
        }
        else
        {
            auto *rc = static_cast<qgui::RenderContext *>(qhal::cgui_state().impl);
            if (rc)
                result = nk_button_label(rc->ctx(), label) ? 1 : 0;
        }
        std::fprintf(stderr, "[BTN<] %s = %d\n", label ? label : "(null)", result);
        return result;
    }

    RVDL_HOST_EXPORT void rvdl_cgui_text(const char *text)
    {
        Route &r = route();
        if (r.fox)
        {
            r.host.text(text);
            return;
        }
        auto *rc = static_cast<qgui::RenderContext *>(qhal::cgui_state().impl);
        if (!rc)
            return;
        nk_label(rc->ctx(), text, NK_TEXT_LEFT);
    }

    RVDL_HOST_EXPORT void rvdl_cgui_text_int(int32_t value)
    {
        Route &r = route();
        if (r.fox)
        {
            r.host.text_int(value);
            return;
        }
        auto *rc = static_cast<qgui::RenderContext *>(qhal::cgui_state().impl);
        if (!rc)
            return;
        char buf[32];
        snprintf(buf, sizeof(buf), "%d", value);
        nk_label(rc->ctx(), buf, NK_TEXT_LEFT);
    }

    RVDL_HOST_EXPORT int32_t rvdl_cgui_mouse_x()
    {
        auto &s = qhal::cgui_state();
        Route &r = route();
        if (r.fox)
            return (int32_t)r.host.mouse_x();
        auto *rc = static_cast<qgui::RenderContext *>(s.impl);
        if (!rc || !rc->window())
            return s.mouse_x;
        double x, y;
        glfwGetCursorPos(rc->window(), &x, &y);
        return (int32_t)x;
    }

    RVDL_HOST_EXPORT int32_t rvdl_cgui_mouse_y()
    {
        auto &s = qhal::cgui_state();
        Route &r = route();
        if (r.fox)
            return (int32_t)r.host.mouse_y();
        auto *rc = static_cast<qgui::RenderContext *>(s.impl);
        if (!rc || !rc->window())
            return s.mouse_y;
        double x, y;
        glfwGetCursorPos(rc->window(), &x, &y);
        return (int32_t)y;
    }

    RVDL_HOST_EXPORT int32_t rvdl_cgui_mouse_left_clicked()
    {
        auto &s = qhal::cgui_state();
        Route &r = route();
        if (r.fox)
            return r.host.mouse_left_down() ? 1 : 0;
        auto *rc = static_cast<qgui::RenderContext *>(s.impl);
        if (!rc || !rc->window())
            return s.left_clicked;
        return glfwGetMouseButton(rc->window(), GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS ? 1 : 0;
    }

    RVDL_HOST_EXPORT void rvdl_cgui_beep(int32_t frequency, int32_t duration_ms)
    {
        std::fprintf(stderr, "[BEEP] %d\n", frequency);
#ifdef _WIN32
        Beep(frequency > 0 ? frequency : 800, duration_ms > 0 ? duration_ms : 100);
#else
        (void)frequency;
        (void)duration_ms;
        std::printf("\a");
        std::fflush(stdout);
#endif
    }

    RVDL_HOST_EXPORT int32_t rvdl_cgui_width()
    {
        auto &s = qhal::cgui_state();
        Route &r = route();
        if (r.fox)
            return r.host.width();
        auto *rc = static_cast<qgui::RenderContext *>(s.impl);
        if (rc && rc->window())
        {
            int w, h;
            glfwGetWindowSize(rc->window(), &w, &h);
            return w;
        }
        return s.width;
    }

    RVDL_HOST_EXPORT int32_t rvdl_cgui_height()
    {
        auto &s = qhal::cgui_state();
        Route &r = route();
        if (r.fox)
            return r.host.height();
        auto *rc = static_cast<qgui::RenderContext *>(s.impl);
        if (rc && rc->window())
        {
            int w, h;
            glfwGetWindowSize(rc->window(), &w, &h);
            return h;
        }
        return s.height;
    }

    RVDL_HOST_EXPORT void rvdl_cgui_panel(int32_t x, int32_t y, int32_t w, int32_t h, const char *title)
    {
        Route &r = route();
        if (r.fox)
        {
            r.host.panel(x, y, w, h, title);
            return;
        }
        auto *rc = static_cast<qgui::RenderContext *>(qhal::cgui_state().impl);
        if (!rc)
            return;
        nk_begin(rc->ctx(), title, nk_rect((float)x, (float)y, (float)w, (float)h),
                 NK_WINDOW_BORDER | NK_WINDOW_NO_SCROLLBAR);
    }

    RVDL_HOST_EXPORT void rvdl_cgui_panel_end()
    {
        Route &r = route();
        if (r.fox)
        {
            r.host.panel_end();
            return;
        }
        auto *rc = static_cast<qgui::RenderContext *>(qhal::cgui_state().impl);
        if (!rc)
            return;
        nk_end(rc->ctx());
    }

    RVDL_HOST_EXPORT void rvdl_cgui_row(int32_t cols, int32_t height)
    {
        Route &r = route();
        if (r.fox)
        {
            r.host.row(cols, height);
            return;
        }
        auto *rc = static_cast<qgui::RenderContext *>(qhal::cgui_state().impl);
        if (!rc)
            return;
        nk_layout_row_dynamic(rc->ctx(), (float)(height > 0 ? height : 34), cols > 0 ? cols : 1);
    }
}