// ============================================================
// Classic Graphics Engine（经典图形引擎）—— cgfx_* 通用 2D 图形原语
// 与 cgui 共享同一后端（fox 优先 / Nuklear 兜底，见 ClassicGui.cpp）。
// 颜色：int32 十六进制 0xRRGGBB。
//
// 坐标是**屏幕绝对坐标**；这是一条与调用顺序无关的约定，因为 Nuklear 的
// 命令缓冲本身就是屏幕空间（面板窗口不会给手绘命令建立局部原点）。于是
// cgfx_* 在两条后端路径上的语义完全一致。
//
// ⚠ 一处已知的、刻意接受的不一致：图层顺序。Nuklear 路径下 cgfx_* 画进
//   背景窗口的命令缓冲，而背景窗口总是最先被绘制 —— 图形在面板**下面**；
//   fox 路径下原语进入显示列表，按调用顺序排序。想画在面板之上，用 fox 的
//   顺序语义或在面板之后调用；两条路径都不承诺"跨后端的像素级同一画面"。
// ============================================================
#include "qhal/ClassicGfx.hpp"
#include "qhal/ClassicGui.hpp"
#include "ClassicRoute.hpp"
#include "gui/src/render_context.h"
#include "nuklear_config.h"

namespace
{
    struct nk_command_buffer *get_canvas()
    {
        // 按窗口名每帧动态查找背景窗口的画布。
        // Nuklear 的命令缓冲用 swap 机制管理，&win->buffer 的内容每帧变化，
        // 缓存指针会指向已交换失效的内存（命令堆积 → 渲染冻结）。
        auto &st = qhal::cgui_state();
        auto *rc = static_cast<qgui::RenderContext *>(st.impl);
        if (!rc)
            return nullptr;
        auto *win = nk_window_find(rc->ctx(), st.title);
        if (!win)
            return nullptr;
        return &win->buffer;
    }

    // 0xRRGGBB → nk_color
    struct nk_color color(int32_t c)
    {
        int r = (c >> 16) & 0xFF;
        int g = (c >> 8) & 0xFF;
        int b = c & 0xFF;
        return nk_rgb(r, g, b);
    }

    // 0xRRGGBB + alpha(0-255) → nk_color（带 alpha 混合）
    struct nk_color color_a(int32_t c, int32_t alpha)
    {
        int r = (c >> 16) & 0xFF;
        int g = (c >> 8) & 0xFF;
        int b = c & 0xFF;
        int a = alpha < 0 ? 0 : (alpha > 255 ? 255 : alpha);
        return nk_rgba(r, g, b, a);
    }
} // namespace

extern "C"
{
    RVDL_HOST_EXPORT void rvdl_cgfx_rect(int32_t x, int32_t y, int32_t w, int32_t h, int32_t c)
    {
        if (qhal::classic_route::fox_active())
        {
            qhal::classic_route::rect((float)x, (float)y, (float)w, (float)h, c, 255);
            return;
        }
        auto *canvas = get_canvas();
        if (!canvas)
            return;
        nk_fill_rect(canvas, nk_rect((float)x, (float)y, (float)w, (float)h), 0.0f, color(c));
    }

    RVDL_HOST_EXPORT void rvdl_cgfx_line(int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t thickness, int32_t c)
    {
        if (qhal::classic_route::fox_active())
        {
            qhal::classic_route::line((float)x0, (float)y0, (float)x1, (float)y1,
                                      thickness > 0 ? (float)thickness : 1.0f, c, 255);
            return;
        }
        auto *canvas = get_canvas();
        if (!canvas)
            return;
        nk_stroke_line(canvas, (float)x0, (float)y0, (float)x1, (float)y1,
                       thickness > 0 ? (float)thickness : 1.0f, color(c));
    }

    RVDL_HOST_EXPORT void rvdl_cgfx_polyline(const int32_t *pts, int32_t count, int32_t thickness, int32_t c)
    {
        rvdl_cgfx_polyline_a(pts, count, thickness, c, 255);
    }

    RVDL_HOST_EXPORT void rvdl_cgfx_polyline_a(const int32_t *pts, int32_t count, int32_t thickness, int32_t c, int32_t alpha)
    {
        if (!pts || count < 2)
            return; // 一条线段都构不成 —— 不画，也不报错（调用方在攒点的过程中）

        if (qhal::classic_route::fox_active())
        {
            // int32 → float 在此转换：两条后端（fox 的 push_path / Nuklear 的
            // nk_stroke_polyline）都要 float，而 rvdl 只有 int32。
            // 堆上限防呆：count 来自 rvdl 侧，一个错误的计数不该变成一次
            // 巨大的栈分配。
            const size_t n = (size_t)count > 4096 ? 4096 : (size_t)count;
            static thread_local float xy[8192];
            for (size_t i = 0; i < n * 2; ++i)
                xy[i] = (float)pts[i];
            qhal::classic_route::polyline(xy, (int)n,
                                          thickness > 0 ? (float)thickness : 1.0f, c, alpha);
            return;
        }
        auto *canvas = get_canvas();
        if (!canvas)
            return;
        // Nuklear 兜底同样按**一条路径**画（nk_stroke_polyline），保持
        // 「接缝不错位」这条语义与 fox 路径一致。
        static thread_local float xy[8192];
        const size_t n = (size_t)count > 4096 ? 4096 : (size_t)count;
        for (size_t i = 0; i < n * 2; ++i)
            xy[i] = (float)pts[i];
        nk_stroke_polyline(canvas, xy, (int)n, thickness > 0 ? (float)thickness : 1.0f,
                           color_a(c, alpha));
    }

    RVDL_HOST_EXPORT void rvdl_cgfx_ellipse(int32_t cx, int32_t cy, int32_t rx, int32_t ry, int32_t c)
    {
        if (qhal::classic_route::fox_active())
        {
            qhal::classic_route::ellipse((float)cx, (float)cy, (float)rx, (float)ry, c, 255);
            return;
        }
        auto *canvas = get_canvas();
        if (!canvas)
            return;
        nk_fill_circle(canvas,
                       nk_rect((float)(cx - rx), (float)(cy - ry), (float)(rx * 2), (float)(ry * 2)),
                       color(c));
    }

    RVDL_HOST_EXPORT void rvdl_cgfx_triangle(int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t x2, int32_t y2, int32_t c)
    {
        if (qhal::classic_route::fox_active())
        {
            qhal::classic_route::triangle((float)x0, (float)y0, (float)x1, (float)y1, (float)x2,
                                          (float)y2, c, 255);
            return;
        }
        auto *canvas = get_canvas();
        if (!canvas)
            return;
        nk_fill_triangle(canvas, (float)x0, (float)y0, (float)x1, (float)y1, (float)x2, (float)y2, color(c));
    }

    // ─── 带 alpha 的原语 ────────────────────────────────────────
    RVDL_HOST_EXPORT void rvdl_cgfx_rect_a(int32_t x, int32_t y, int32_t w, int32_t h, int32_t c, int32_t alpha)
    {
        if (qhal::classic_route::fox_active())
        {
            qhal::classic_route::rect((float)x, (float)y, (float)w, (float)h, c, alpha);
            return;
        }
        auto *canvas = get_canvas();
        if (!canvas)
            return;
        nk_fill_rect(canvas, nk_rect((float)x, (float)y, (float)w, (float)h), 0.0f, color_a(c, alpha));
    }

    RVDL_HOST_EXPORT void rvdl_cgfx_line_a(int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t thickness, int32_t c, int32_t alpha)
    {
        if (qhal::classic_route::fox_active())
        {
            qhal::classic_route::line((float)x0, (float)y0, (float)x1, (float)y1,
                                      thickness > 0 ? (float)thickness : 1.0f, c, alpha);
            return;
        }
        auto *canvas = get_canvas();
        if (!canvas)
            return;
        nk_stroke_line(canvas, (float)x0, (float)y0, (float)x1, (float)y1,
                       thickness > 0 ? (float)thickness : 1.0f, color_a(c, alpha));
    }

    RVDL_HOST_EXPORT void rvdl_cgfx_ellipse_a(int32_t cx, int32_t cy, int32_t rx, int32_t ry, int32_t c, int32_t alpha)
    {
        if (qhal::classic_route::fox_active())
        {
            qhal::classic_route::ellipse((float)cx, (float)cy, (float)rx, (float)ry, c, alpha);
            return;
        }
        auto *canvas = get_canvas();
        if (!canvas)
            return;
        nk_fill_circle(canvas,
                       nk_rect((float)(cx - rx), (float)(cy - ry), (float)(rx * 2), (float)(ry * 2)),
                       color_a(c, alpha));
    }

    RVDL_HOST_EXPORT void rvdl_cgfx_triangle_a(int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t x2, int32_t y2, int32_t c, int32_t alpha)
    {
        if (qhal::classic_route::fox_active())
        {
            qhal::classic_route::triangle((float)x0, (float)y0, (float)x1, (float)y1, (float)x2,
                                          (float)y2, c, alpha);
            return;
        }
        auto *canvas = get_canvas();
        if (!canvas)
            return;
        nk_fill_triangle(canvas, (float)x0, (float)y0, (float)x1, (float)y1, (float)x2, (float)y2, color_a(c, alpha));
    }
}
