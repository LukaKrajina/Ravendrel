// ============================================================
// Classic GUI（经典图形界面）—— cgui_* 的运行时后端选择 + 实现
//
// 后端顺序是**运行时**决定的：优先 fox（ciUd/fox），fox 建不起来 / 起不来 /
// 运行中判死，才退到 Nuklear（复用 qvm_visualizer 的 RenderContext）。
// 每次选择都说出理由 —— 见 qhal/FoxHost.hpp 文件头"绝不静默"。
//
// 为什么不做成编译期开关：需要回退的原因（驱动、扩展、swapchain、窗口）全都
// 只在运行时才知道，"我们猜这台机器不行"既盖不住真实故障，又让"fox 可用"
// 失去证据。编译期能选的只有"要不要 fox 这条路"，这里两条路都编进去。
// ============================================================
#include "qhal/ClassicGui.hpp"
#include "ClassicRoute.hpp" // 内部接缝：只有 cgfx 需要（见文件头）
#include "qhal/FoxHost.hpp"
#include "utils/A2a.hpp"
#include "gui/src/render_context.h"
#include "nuklear_config.h"
#include <cstring>
#include <cstdio>
#include <chrono>
// 手动声明 Beep，避免 #include <windows.h> 引入 min/max 等宏污染
#ifdef _WIN32
extern "C" __declspec(dllimport) int __stdcall Beep(unsigned long dwFreq, unsigned long dwDuration);
#endif

namespace
{
    // 帧耗时剖析：帧间隔 gap = rvdl逻辑 + end_frame + 未计入空隙
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

    // ── fox 优先 / Nuklear 兜底（见 qhal/FoxHost.hpp）────────────────────

    void fox_report(const char *line, void *user)
    {
        (void)user;
        std::fprintf(stderr, "[cgui] %s\n", line ? line : "");
    }

    /// fox 路径的窗口引用。窗口归本文件所有，fox 只借它建 VkSurfaceKHR
    /// （所有权约定见 fox.h）。放在工厂的 user 指针里，于是 FoxHost 不需要
    /// 认识 GLFWwindow。
    struct WinRef
    {
        GLFWwindow *win = nullptr;
    };

    void *glfw_create(int w, int h, const char *title, void *user)
    {
        glfwDefaultWindowHints();
        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API); // Vulkan 由 fox 管
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

    /// 后端路由。**一个进程一份**：cgui 的语义是单窗口全局状态
    /// （见 ClassicGui.hpp 的 cgui_state()），所以路由也只有一个。
    struct Route;
    void sync_quality(Route &r, bool force); // 定义在 Route 之后（start() 里要用）

    struct Route
    {
        // ⚠ 声明顺序即构造顺序：FoxApi 表与 WindowFactory 必须比 host 活得久
        //   （host 只存指针）。
        qhal::FoxApi api = qhal::fox_real_api();
        WinRef winref;
        qhal::FoxHost host;

        qgui::RenderContext *nk = nullptr; // 只有 Nuklear 路径存在
        bool decided = false;              // 已经做过后端选择
        bool fox = false;                  // 当前由 fox 负责
        int w = 640, h = 480;
        char title[256] = "Classic GUI";

        // 质量档（热压力）：A2a 决策 → fox_set_quality。见 sync_quality()。
        a2a::Quality quality{};
        bool have_quality = false;
        int quality_countdown = 0;

        bool start(int width, int height, const char *t)
        {
            w = width;
            h = height;
            std::snprintf(title, sizeof(title), "%s", t ? t : "Classic GUI");
            decided = true;

            // fox 要一个已经创建好的窗口，而 GLFW 必须先初始化。Nuklear 的
            // RenderContext::init 也会调 glfwInit：GLFW 对重复初始化是幂等的
            // （已初始化时直接返回 TRUE），所以这里先起没有问题。
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
                // 连续 3 帧提交失败就切走：一次 resize 竞态不至于切，而三帧
                // （~50ms）之后继续画没有意义。
                cfg.fault_threshold = 3;

                fox = host.init(api, wf, cfg);
            }

            if (!fox) return nuklear_ready();

            // GPU 身份 → A2a。A2a 不猜 GPU 类别（见 utils/A2a.hpp 第 2 条：
            // 探测与决策分离），所以"是哪块 GPU"只有这里能报。
            const char *gpu = host.gpu_name();
            a2a::report_gpu(gpu ? gpu : "", host.gpu_vendor());
            sync_quality(*this, true);
            std::fprintf(stderr, "[cgui] renderer: fox (GPU '%s', vendor 0x%04X, %dx%d logical)\n",
                         gpu && *gpu ? gpu : "?", host.gpu_vendor(), w, h);
            return true;
        }

        /// 启动 Nuklear 路径（已经有一条 fox 路径在跑时不要调这个 —— 会开出
        /// 第二个窗口）。
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
        // ⚠ 故意不析构。退出顺序无法保证：Route 的析构会先销毁 fox 上下文、
        //   再销毁窗口，而 RenderContext 的析构里还有 glfwTerminate —— 静态
        //   对象在退出阶段的顺序一旦反过来就是崩溃。进程退出本来就回收内存。
        static Route *r = new Route();
        return *r;
    }

    /// 把 A2a 的质量档下发给 fox。每 N 帧对一次表（Android 上 current_quality
    /// 要读 sysfs，不能每帧读），并且只在**变化时**才调用 fox —— 也只在变化时
    /// 打印一行，这样"什么时候降的级、降到哪一档"在日志里有据可查。
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

    /// fox 运行中判死：这一帧起改由 Nuklear 画。
    ///
    /// 显示列表在 rvdl 侧每帧重建，所以不需要迁移任何状态 —— 下一帧的绘制
    /// 调用全部落到 Nuklear 上，画面自然重画一遍。代价只有一次窗口重建
    /// （新窗口在默认位置，不做位置记忆：那不是渲染正确性的一部分）。
    void switch_to_nuklear(Route &r)
    {
        std::fprintf(stderr, "[cgui] switching off fox: %s\n",
                     r.host.reason()[0] ? r.host.reason() : "unknown");
        // 顺序是规范要求：后端先销毁（它拥有 VkSurfaceKHR / VkInstance），
        // 窗口后销毁。
        r.host.shutdown();
        r.fox = false;
        r.nuklear_ready();
    }
} // namespace

// ── cgfx 的转发接缝（见 ClassicRoute.hpp）───────────────────────────────
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
    } // namespace classic_route
} // namespace qhal

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

    /// 运行时的 Fox 上下文 —— 拥有窗口和 Vulkan 后端的那个，
    /// 它的显示列表才是真正被**呈现**出来的。
    ///
    /// ⚠ 为什么需要这个出口：宿主（rvdl 的 shell）通常也会自己建一个 fox
    ///   上下文，而那个上下文是**无窗**的（只用来做文本度量）。如果宿主把
    ///   UI 画进自己那个上下文，那些绘制命令就进了**一个永远不会被渲染的
    ///   显示列表** —— 窗口开得出来、帧也在跑，屏幕上却什么都没有。
    ///
    ///   这正是 OpenButler 桌面端「窗口一片空白」的根因：它的 29 处
    ///   gh_ui_text 与 28 处 gh_ui_rect_a 全画进了无窗上下文，只有 1 个
    ///   cgfx_ellipse 走对了地方 —— 于是每帧只有 3 个实例。
    ///
    /// 返回 NULL = 运行时的 fox 后端还没起来（宿主应回退到自己那个）。
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
            return 1; // 没有任何渲染器：结束程序，而不是转一个空循环
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
                    r.quality_countdown = 60; // 约每秒一次
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
            // 上一帧被判定失败：本帧起 Nuklear 接管（帧边界切换，不中断帧）。
            switch_to_nuklear(r);
            if (!r.nk)
                return;
        }

        auto *rc = static_cast<qgui::RenderContext *>(s.impl);
        if (!rc)
            return;
        rc->begin_frame();
        // 默认窗口作为背景画布（无输入，不抢鼠标）；分区由 cgui_panel 控制。
        // cgfx 经 nk_window_find(此名) 每帧动态取 &win->buffer（swap 机制下指针会变，不可缓存）。
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
            // 故障记账与"下一帧切走"的决定都在里面（见 FoxHost::end_frame）。
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
        // 音效反馈（技能触发等）；同步蜂鸣，短时长不阻塞游戏循环
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

    // 开一个面板窗口（分区布局：顶部 HUD / 中央棋盘 / 底部操作）
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

    // 设置当前行为多列布局（横排按钮）
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
