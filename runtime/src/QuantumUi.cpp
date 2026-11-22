#ifndef RVDL_RT_BUILD
#define RVDL_RT_BUILD
#endif

#include "qhal/QuantumUi.hpp"
#include "qhal/QuantumUiNuklear.hpp"
#include "gui/src/render_context.h"
#include "qhal/ClassicGui.hpp"
#include "nuklear_config.h"

#include <algorithm>
#include <cmath>

namespace
{
    struct nk_command_buffer *canvas()
    {
        auto &st = qhal::cgui_state();
        auto *rc = static_cast<qgui::RenderContext *>(st.impl);
        if (!rc)
            return nullptr;
        auto *win = nk_window_find(rc->ctx(), st.title);
        return win ? &win->buffer : nullptr;
    }

    struct nk_color rgb(int32_t c)
    {
        return nk_rgb((c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF);
    }

    struct nk_color rgba(int32_t c, int32_t a)
    {
        const int al = a < 0 ? 0 : (a > 255 ? 255 : a);
        return nk_rgba((c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF, al);
    }

    constexpr int32_t kSeparatedGray = 0x707880;
    constexpr double kVisibleEntanglement = 0.02;

    constexpr float kMinLinkPixels = 6.0f;

    void stroke_link(struct nk_command_buffer *cv, float x0, float y0, float x1, float y1,
                     float width, int32_t color, int32_t alpha)
    {
        const float dx = x1 - x0;
        const float dy = y1 - y0;
        if (std::max(std::fabs(dx), std::fabs(dy)) < kMinLinkPixels)
            return;
        qqui::stroke_line(cv, x0, y0, x1, y1, width, rgba(color, alpha));
    }
}

extern "C"
{

    RVDL_HOST_EXPORT void rvdl_qqui_qubit_glyph(int32_t x, int32_t y, int32_t size,
                                                uint64_t frame, int32_t qubit,
                                                int32_t color, int32_t alpha)
    {
        auto *cv = canvas();
        if (!cv)
            return;
        const qhal::QuantumFrame *f = qhal::qgfx_resolve_const(frame);
        if (!f || size <= 0)
            return;

        double bx, by, bz;
        f->local_bloch(static_cast<uint32_t>(qubit < 0 ? 0 : qubit), bx, by, bz);

        const double r = std::sqrt(bx * bx + by * by + bz * bz);
        const float rad = std::max(1.0f, static_cast<float>(size) * 0.5f *
                                               static_cast<float>(std::min(1.0, r)));

        const float cx = static_cast<float>(x) + static_cast<float>(size) * 0.5f;
        const float cy = static_cast<float>(y) + static_cast<float>(size) * 0.5f;
        const double theta = std::acos(std::clamp(bz, -1.0, 1.0));
        const double phi = (r > 1e-12) ? std::atan2(by, bx) : 0.0;
        const float ax = static_cast<float>(std::cos(theta) * std::cos(phi));
        const float ay = static_cast<float>(std::sin(theta));
        const float inner = rad * 0.72f;

        qqui::fill_circle(cv, cx, cy, rad, rgba(color, alpha < 40 ? 40 : alpha));
        qqui::stroke_line(cv, cx, cy, cx + ax * inner, cy - ay * inner,
                          std::max(1.0f, static_cast<float>(size) * 0.08f),
                          rgba(0xFFFFFF, std::min(255, alpha + 60)));
    }

    RVDL_HOST_EXPORT void rvdl_qqui_qubit_row(int32_t x, int32_t y, int32_t cell,
                                             uint64_t frame, int32_t first_qubit,
                                             int32_t count, int32_t color, int32_t alpha)
    {
        if (count <= 0)
            return;
        if (count > 32)
            count = 32;
        for (int32_t i = 0; i < count; ++i)
            rvdl_qqui_qubit_glyph(x + i * cell, y, cell, frame, first_qubit + i, color, alpha);
    }

    RVDL_HOST_EXPORT double rvdl_qqui_entangle_link(int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                                                   uint64_t frame, int32_t qa, int32_t qb,
                                                   int32_t color, int32_t alpha)
    {
        if (qa == qb)
            return 0.0;
        const qhal::QuantumFrame *f = qhal::qgfx_resolve_const(frame);
        if (!f)
            return 0.0;

        const uint32_t ua = static_cast<uint32_t>(std::max(qa, 0));
        const uint32_t ub = static_cast<uint32_t>(std::max(qb, 0));
        const double strength = f->pair_entanglement(ua, ub);
        const bool entangled = f->entanglement_exists(ua, ub);

        auto *cv = canvas();
        if (!cv)
            return strength;

        const bool visible = entangled && strength > kVisibleEntanglement;
        const float w = visible ? static_cast<float>(1.0 + strength * 4.0) : 1.0f;
        const int32_t c = visible ? color : kSeparatedGray;
        const int32_t a = visible ? alpha : std::min(alpha, 70);
        stroke_link(cv, static_cast<float>(x0), static_cast<float>(y0),
                    static_cast<float>(x1), static_cast<float>(y1), w, c, a);
        return strength;
    }


    RVDL_HOST_EXPORT void rvdl_qqui_uncertainty_bin(int32_t x, int32_t baseline_y,
                                                   int32_t slot_w, double probability,
                                                   double shot_noise,
                                                   int32_t color, int32_t alpha)
    {
        if (slot_w <= 0)
            return;
        auto *cv = canvas();
        if (!cv)
            return;

        const double p = std::clamp(probability, 0.0, 1.0);
        double rel = 0.0;
        if (p > 1e-9)
            rel = std::clamp(shot_noise / p, 0.0, 4.0);
        const double conf = 1.0 / (1.0 + rel);
        const int32_t w = std::max(1, static_cast<int32_t>(slot_w * conf));
        const int32_t h = static_cast<int32_t>(std::round(slot_w * p));
        if (h <= 0)
            return;

        qqui::fill_rect(cv, static_cast<float>(x), static_cast<float>(baseline_y - h),
                        static_cast<float>(w), static_cast<float>(h), rgba(color, alpha));

        if (shot_noise > 1e-9)
        {
            const int32_t e = static_cast<int32_t>(std::round(slot_w * std::min(1.0, p + shot_noise)));
            if (e > h)
                qqui::stroke_line(cv, static_cast<float>(x + w * 0.5f),
                                  static_cast<float>(baseline_y - e),
                                  static_cast<float>(x + w * 0.5f),
                                  static_cast<float>(baseline_y),
                                  1.0f, rgba(0xFFFFFF, alpha));
        }
    }

    RVDL_HOST_EXPORT void rvdl_qqui_wigner_slice(int32_t x, int32_t y, int32_t w, int32_t h,
                                                uint64_t frame, int32_t first_qubit,
                                                int32_t res)
    {
        if (w <= 0 || h <= 0)
            return;
        auto *cv = canvas();
        if (!cv)
            return;
        const qhal::QuantumFrame *f = qhal::qgfx_resolve_const(frame);
        if (!f)
            return;
        res = std::clamp(res, 8, 64);

        const uint32_t q = static_cast<uint32_t>(first_qubit < 0 ? 0 : first_qubit);
        const size_t dim = f->hilbert_dim();
        const size_t mask = size_t(1) << std::min<size_t>(q, 23);

        double X = 0.0, P = 0.0;
        for (size_t i = 0; i < dim; ++i)
        {
            const size_t j = i ^ mask;
            if (j < i)
                continue;
            const qhal::qcomplex z = f->amplitude(i) * std::conj(f->amplitude(j));
            X += 2.0 * z.real();
            P += -2.0 * z.imag();
        }

        const double cx = static_cast<double>(x) + static_cast<double>(w) * 0.5;
        const double cy = static_cast<double>(y) + static_cast<double>(h) * 0.5;
        const double cell = static_cast<double>(w) / static_cast<double>(res);
        const double inv_r = 1.0 / std::sqrt(X * X + P * P + 1e-12);
        const float fw = static_cast<float>(cell) + 1.0f;

        for (int32_t iy = 0; iy < res; ++iy)
        {
            for (int32_t ix = 0; ix < res; ++ix)
            {
                const double px = static_cast<double>(x) + (ix + 0.5) * cell;
                const double py = static_cast<double>(y) + (iy + 0.5) * cell;
                const double dx = px - cx;
                const double dy = py - cy;
                const double g = std::exp(-(dx * dx + dy * dy) / (2.0 * cell * cell));
                const double signed_val = g * (dx * X - dy * P) * inv_r;
                const double a = std::clamp(std::fabs(signed_val), 0.0, 1.0);
                if (a < 0.02)
                    continue;
                const int32_t c = signed_val >= 0.0 ? 0xE0503A : 0x3A70E0;
                qqui::fill_rect(cv, static_cast<float>(px - cell * 0.5),
                                static_cast<float>(py - cell * 0.5), fw, fw,
                                rgba(c, static_cast<int32_t>(40 + 200 * a)));
            }
        }
    }


    RVDL_HOST_EXPORT void rvdl_qqui_nested_bar(int32_t x, int32_t y, int32_t cell,
                                              uint64_t frame, int32_t color, int32_t alpha)
    {
        if (cell <= 0)
            return;
        auto *cv = canvas();
        if (!cv)
            return;
        const qhal::QuantumFrame *f = qhal::qgfx_resolve_const(frame);
        if (!f)
            return;

        const int32_t shells = static_cast<int32_t>(qhal::QuantumFrame::nested_shell_count(f->num_qubits()));
        const int32_t rows = std::min(shells, 32);
        for (int32_t r = 0; r < rows; ++r)
        {
            const double mass = rvdl_qgfx_nested_mass(frame, r, shells);
            const int32_t h = static_cast<int32_t>(std::round(cell * mass));
            if (h <= 0)
                continue;
            qqui::fill_rect(cv, static_cast<float>(x),
                            static_cast<float>(y + r * cell - h),
                            static_cast<float>(cell), static_cast<float>(h),
                            rgba(color, alpha));
        }
    }


    RVDL_HOST_EXPORT int32_t rvdl_qqui_amplitude_strip(int32_t x, int32_t y, int32_t w, int32_t h,
                                                      uint64_t frame, int32_t color, int32_t alpha)
    {
        const qhal::QuantumFrame *f = qhal::qgfx_resolve_const(frame);
        if (!f || w <= 0 || h <= 0)
            return -1;
        const uint32_t shown_q = std::min<uint32_t>(f->num_qubits(), 12);
        const size_t n = size_t(1) << shown_q;

        auto *cv = canvas();
        if (!cv)
            return -1;

        const double slot = static_cast<double>(w) / static_cast<double>(n);
        const auto &st = qhal::cgui_state();
        const double mx = static_cast<double>(st.mouse_x);
        const double my = static_cast<double>(st.mouse_y);
        const bool clicked = st.left_clicked != 0;
        int32_t hit = -1;

        for (size_t i = 0; i < n; ++i)
        {
            const double p = f->probability(i);
            const int32_t bh = static_cast<int32_t>(std::round(h * p));
            if (bh <= 0)
                continue;
            const double bx = static_cast<double>(x) + static_cast<double>(i) * slot;
            const int32_t c = (p > 1e-12)
                                  ? static_cast<int32_t>(std::abs(rvdl_qgfx_phase_hue(f->phase(i))))
                                  : color;
            qqui::fill_rect(cv, static_cast<float>(bx), static_cast<float>(y + h - bh),
                            static_cast<float>(slot) + 0.5f, static_cast<float>(bh),
                            rgba(c, alpha));
            if (clicked && mx >= bx && mx < bx + slot &&
                my >= static_cast<double>(y + h - bh) && my <= static_cast<double>(y + h))
                hit = static_cast<int32_t>(i);
        }
        if (hit >= 0 && shown_q < f->num_qubits())
            return -hit - 1;
        return hit;
    }

    RVDL_HOST_EXPORT void rvdl_qqui_state_card(int32_t x, int32_t y, int32_t w,
                                              uint64_t frame, int32_t projection,
                                              int32_t color, int32_t alpha)
    {
        if (w <= 0)
            return;
        auto *cv = canvas();
        if (!cv)
            return;
        const qhal::QuantumFrame *f = qhal::qgfx_resolve_const(frame);
        if (!f)
            return;

        const double src = qhal::qgf_state_real_dims(f->num_qubits());
        const double dst = qhal::qgf_projection_target_dims(
            static_cast<qhal::QgfProjection>(projection), f->num_qubits());
        const double loss = src > 0.0 ? std::clamp(1.0 - dst / src, 0.0, 1.0) : 0.0;

        const double resid = f->normalization_residual();
        const bool suspect = resid > 1e-6;
        const int32_t bar_c = suspect ? 0xE0A03A : color;

        qqui::fill_rect(cv, static_cast<float>(x), static_cast<float>(y),
                        static_cast<float>(w), 6.0f, rgba(bar_c, alpha));

        const int32_t loss_w = static_cast<int32_t>(w * loss);
        if (loss_w > 0)
            qqui::fill_rect(cv, static_cast<float>(x), static_cast<float>(y + 8.0f),
                            static_cast<float>(loss_w), 4.0f,
                            rgba(kSeparatedGray, alpha));
    }
}