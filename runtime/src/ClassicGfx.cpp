#include "qhal/ClassicGfx.hpp"
#include "qhal/ClassicGui.hpp"
#include "ClassicRoute.hpp"
#include "gui/src/render_context.h"
#include "nuklear_config.h"

namespace
{
    struct nk_command_buffer *get_canvas()
    {
        auto &st = qhal::cgui_state();
        auto *rc = static_cast<qgui::RenderContext *>(st.impl);
        if (!rc)
            return nullptr;
        auto *win = nk_window_find(rc->ctx(), st.title);
        if (!win)
            return nullptr;
        return &win->buffer;
    }

    struct nk_color color(int32_t c)
    {
        int r = (c >> 16) & 0xFF;
        int g = (c >> 8) & 0xFF;
        int b = c & 0xFF;
        return nk_rgb(r, g, b);
    }

    struct nk_color color_a(int32_t c, int32_t alpha)
    {
        int r = (c >> 16) & 0xFF;
        int g = (c >> 8) & 0xFF;
        int b = c & 0xFF;
        int a = alpha < 0 ? 0 : (alpha > 255 ? 255 : alpha);
        return nk_rgba(r, g, b, a);
    }
}

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
            return;

        if (qhal::classic_route::fox_active())
        {
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