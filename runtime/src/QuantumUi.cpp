// ============================================================
// Quantum UI Library（量子 UI 库）—— qqui_* 渲染层
//
// 在 Nuklear 的命令缓冲上绘制量子控件。与 ClassicGfx.cpp 共用同一
// qgui::RenderContext 与窗口生命周期（cgui_begin_frame/end_frame）。
//
// 范式：谓词耦合的量子控件 (PCQW)，完整论证见 qhal/QuantumUi.hpp 头。
// Nuklear 调用统一走 QuantumUiNuklear.hpp 的 shim（该 vendored 版本的
// nk_fill_rect 带 rounding 参数，与上游不同形）。
//
// 渲染后端不可用时（本文件单独编译的单元测试、无显示环境），所有函数
// 在取得画布后即安全返回，绘制逻辑不触碰 GPU —— 使控件在 headless 下
// 仍可被测试。
// ============================================================
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
        // 按窗口名每帧重新查找画布。Nuklear 的命令缓冲用 swap 机制管理，
        // &win->buffer 每帧变化；缓存指针会指向已交换失效的内存。
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

    // 预注意信号：可分离用灰色低注意力，纠缠用调用方指定色高注意力。
    // 「不纠缠」因此无需读数字即可感知。依据：Bley et al. (arXiv:2305.07596)
    // 让「对称即可分离」成为视觉属性的先例。
    constexpr int32_t kSeparatedGray = 0x707880;
    // 纠缠强度 > 此值才算显著纠缠，否则视为准可分离。
    constexpr double kVisibleEntanglement = 0.02;

    // 短于该像素长度的连线不绘制：噪声级纠缠不应占据注意力。
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
} // namespace

extern "C"
{
    // ═══ W1：携带谓词的量子控件 ═════════════════════════════════════

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

        // 半径编码 |r|。纯态下 |r| = 1（半径恒满），因此该半径**只在混态
        // 或未归一化时**才变化 —— 如实反映，不制造假的"混合度"。
        const double r = std::sqrt(bx * bx + by * by + bz * bz);
        const float rad = std::max(1.0f, static_cast<float>(size) * 0.5f *
                                               static_cast<float>(std::min(1.0, r)));

        const float cx = static_cast<float>(x) + static_cast<float>(size) * 0.5f;
        const float cy = static_cast<float>(y) + static_cast<float>(size) * 0.5f;
        // 极坐标 (θ, φ) → 盘面轴向。θ=0 指向 +Z，屏幕上方对应 −Y。
        const double theta = std::acos(std::clamp(bz, -1.0, 1.0));
        const double phi = (r > 1e-12) ? std::atan2(by, bx) : 0.0;
        const float ax = static_cast<float>(std::cos(theta) * std::cos(phi));
        const float ay = static_cast<float>(std::sin(theta));
        const float inner = rad * 0.72f;

        qqui::fill_circle(cv, cx, cy, rad, rgba(color, alpha < 40 ? 40 : alpha));
        // 盘面轴：从中心指向 Bloch 方向的一条短线，即局域态指向
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
        // 超出可用宽度时钳制，避免溢出窗口
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

        // 口径统一（W1：控件与图元共享同一谓词，避免"看起来纠缠"与
        // "算出来纠缠"出现两套判据）。
        //
        // pair_entanglement 给出**纠缠强度**（线宽编码）；
        // entanglement_exists 给出**可分离/纠缠二分**（颜色与 alpha 编码）。
        // 二者刻意分离：pair_entanglement 对可分离态也非零（见其文档），
        // 绝不可拿它当可分离性判据。
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

    // ═══ W2：不确定性 bin-glyph ═════════════════════════════════════

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

        // 宽度 = 置信度的几何表达：不确定度越大越宽。
        // 由 shot_noise 反推相对标准差 rs = σ/p，宽度 ∝ 1/(1+rs)。
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

        // 1σ 区间以顶帽表示；覆盖保证直接来自 shot_noise 的定义，
        // 而非手工标注（Petek et al., arXiv:2508.00937：让覆盖保证从
        // 渲染函数本身涌现，而不是逐图元特判错误棒）。
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

        // 单比特在 (x, p) 相平面上的 Wigner 截面。
        //
        // 用相干态基元展开： W(ξ) ∝ Σ_ij c_ij W_ξ(i,j)，其中
        // W_ξ(i,j) = ⟨i|ξ⟩⟨ξ|j⟩ 是相干态重叠的二维高斯包络
        // （Susskind–Glogower）。取其一阶主导项，闭式 O(1)/像素：
        //   W(ξ) ≈ g(ξ) · Re[Σ_j c_0j ⟨ξ|j⟩ / ⟨0|ξ⟩]
        // 其中 g 为与屏幕分辨率一致的各向同性高斯（σ = cell），不制造
        // 超出采样率的假细节。
        const uint32_t q = static_cast<uint32_t>(first_qubit < 0 ? 0 : first_qubit);
        const size_t dim = f->hilbert_dim();
        const size_t mask = size_t(1) << std::min<size_t>(q, 23);

        // (⟨X⟩, ⟨P⟩)：一阶矩决定高斯中心（即局域 Bloch 指向）
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
                // 高斯包络：σ = cell，与屏幕分辨率一致
                const double g = std::exp(-(dx * dx + dy * dy) / (2.0 * cell * cell));
                // 有符号：沿局域 Bloch 方向为正，垂直方向为负
                // （位移与 Bloch 投影的叉积符号）
                const double signed_val = g * (dx * X - dy * P) * inv_r;
                const double a = std::clamp(std::fabs(signed_val), 0.0, 1.0);
                if (a < 0.02)
                    continue;
                // 发散色标：正红负蓝。Wigner 函数**可为负**，概率不能 ——
                // 用单极色标会把这层反相结构整个抹掉。
                const int32_t c = signed_val >= 0.0 ? 0xE0503A : 0x3A70E0;
                qqui::fill_rect(cv, static_cast<float>(px - cell * 0.5),
                                static_cast<float>(py - cell * 0.5), fw, fw,
                                rgba(c, static_cast<int32_t>(40 + 200 * a)));
            }
        }
    }

    // ═══ W3：嵌套条 ═════════════════════════════════════════════════

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

        // 布局完全由比特数决定（调用者不选 LOD）：
        //   shells = 2^(n−1) 行 —— 20 比特仍是可读的多行面板，而非 1024 列。
        const int32_t shells = static_cast<int32_t>(qhal::QuantumFrame::nested_shell_count(f->num_qubits()));
        const int32_t rows = std::min(shells, 32); // 超出可视区钳制
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

    // ═══ W4：交互式图元 ════════════════════════════════════════════

    RVDL_HOST_EXPORT int32_t rvdl_qqui_amplitude_strip(int32_t x, int32_t y, int32_t w, int32_t h,
                                                      uint64_t frame, int32_t color, int32_t alpha)
    {
        const qhal::QuantumFrame *f = qhal::qgfx_resolve_const(frame);
        if (!f || w <= 0 || h <= 0)
            return -1;
        // 2^n 超过可读上限时截断到 12 比特（4096 条），并以返回值中的
        // 负号编码「已截断」，使调用方能告知用户而非静默丢失态。
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
            // 相位 → 感知均匀色相：与计算层共用 rvdl_qgfx_phase_hue，
            // 保证「图」与「数」出自同一函数（W1：口径唯一）。
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
            return -hit - 1; // 负值编码「截断下的选中项」
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

        // P1 的 UI 落地：把「此投影丢了多少自由度」显式画出来。
        // 静默的有损投影是量子可视化最常见的误导来源。
        const double src = qhal::qgf_state_real_dims(f->num_qubits());
        const double dst = qhal::qgf_projection_target_dims(
            static_cast<qhal::QgfProjection>(projection), f->num_qubits());
        const double loss = src > 0.0 ? std::clamp(1.0 - dst / src, 0.0, 1.0) : 0.0;

        // 归一化残差 > 1e-6 时改用告警色，而不是静默归一化（P2）
        const double resid = f->normalization_residual();
        const bool suspect = resid > 1e-6;
        const int32_t bar_c = suspect ? 0xE0A03A : color;

        qqui::fill_rect(cv, static_cast<float>(x), static_cast<float>(y),
                        static_cast<float>(w), 6.0f, rgba(bar_c, alpha));

        // 信息损失条：填充宽度 ∝ 丢失比例
        const int32_t loss_w = static_cast<int32_t>(w * loss);
        if (loss_w > 0)
            qqui::fill_rect(cv, static_cast<float>(x), static_cast<float>(y + 8.0f),
                            static_cast<float>(loss_w), 4.0f,
                            rgba(kSeparatedGray, alpha));
    }
}
