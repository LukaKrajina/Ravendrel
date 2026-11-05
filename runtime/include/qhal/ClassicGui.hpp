#pragma once
//
// Classic GUI（经典图形界面）—— cgui_* 内建函数的 C ABI
//
// 面向 rvdl 语言的「经典」立即模式 GUI（区别于后续的量子专用 GUI qgui_*）。
// 对齐 Nuklear + GLFW：rvdl 每帧调用
//   cgui_begin_frame() → 绘制控件 → cgui_end_frame()。
// 真实实现在 ClassicGui.cpp（复用 qvm_visualizer 的 RenderContext）。
//
#include "RuntimeApi.h"
#include <cstdint>

#ifndef RVDL_HOST_EXPORT
#define RVDL_HOST_EXPORT extern "C" RVDL_RT_API
#endif

namespace qhal
{
    // 经典 GUI 全局状态
    struct ClassicGuiState
    {
        int32_t initialized = 0;
        int32_t width = 640;
        int32_t height = 480;
        int32_t mouse_x = 0;
        int32_t mouse_y = 0;
        int32_t left_clicked = 0;
        char title[256] = "Classic GUI";
        void *impl = nullptr; // 指向 qgui::RenderContext
    };

    inline ClassicGuiState &cgui_state()
    {
        static ClassicGuiState s;
        return s;
    }
}

extern "C"
{
    RVDL_HOST_EXPORT int32_t rvdl_cgui_init(int32_t w, int32_t h, const char *title);
    RVDL_HOST_EXPORT int32_t rvdl_cgui_should_close();
    RVDL_HOST_EXPORT void rvdl_cgui_begin_frame();
    RVDL_HOST_EXPORT void rvdl_cgui_end_frame();
    RVDL_HOST_EXPORT int32_t rvdl_cgui_button(const char *label);
    RVDL_HOST_EXPORT void rvdl_cgui_text(const char *text);
    RVDL_HOST_EXPORT void rvdl_cgui_text_int(int32_t value);
    RVDL_HOST_EXPORT int32_t rvdl_cgui_mouse_x();
    RVDL_HOST_EXPORT int32_t rvdl_cgui_mouse_y();
    RVDL_HOST_EXPORT int32_t rvdl_cgui_mouse_left_clicked();
    RVDL_HOST_EXPORT void rvdl_cgui_beep(int32_t frequency, int32_t duration_ms);
    RVDL_HOST_EXPORT int32_t rvdl_cgui_width();
    RVDL_HOST_EXPORT int32_t rvdl_cgui_height();
    RVDL_HOST_EXPORT void rvdl_cgui_panel(int32_t x, int32_t y, int32_t w, int32_t h, const char *title);
    RVDL_HOST_EXPORT void rvdl_cgui_panel_end();
    RVDL_HOST_EXPORT void rvdl_cgui_row(int32_t cols, int32_t height);
}
