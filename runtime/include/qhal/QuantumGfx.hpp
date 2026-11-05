#pragma once
//
// Quantum Graphics Engine（量子图形引擎）—— qgfx_* 内建函数的 C ABI
//
// ═══════════════════════════════════════════════════════════════════════
// 范式：显式投影的量子图形学 (Projection-Explicit Quantum Graphics, PEQG)
// ═══════════════════════════════════════════════════════════════════════
//
// rvdl 已有两套**经典**图形层：
//   - cgfx_*  2D 通用原语（矩形/线/椭圆/三角形）
//   - cgui_*  立即模式 GUI 控件（按钮/文本/面板）
//
// 这两套都是**应用无关**的绘图原语，无法表达量子语义：它们只能画
// 「一根线」，不能说明这根线是「纠缠还是可分离」。qgfx_* 补齐的正是
// 这一层——**以量子态为一等公民**的绘图原语。
//
// 关键设计（完整论证见 qhal/QuantumState.hpp 文件头）：
//
//   1 投影显式化。 n 比特纯态的实维数是 2(2^n−1)（n=2 → 6，n=3 → 14），
//      因此不存在等距的量子态图像。每个 qgfx 图元都要求调用者显式给出
//      **投影算子** qgfx_projection_t，并在运行期可反查
//      （rvdl_qgfx_projection_dims 返回"丢失了多少维"）。
//
//   2 谓词可读化。 每个投影自带 O(1) 的代数判据，可直接从图形读出真伪：
//      - quaternionic（四元数部分）≈0 ⟺ 两比特可分离
//      - concurrency 条码长度 > 1 ⟺ 存在纠缠
//      - bar 的持久度 = Schmidt 间隙
//
//   3 复值精确图元。 幅度图元用 |a| 控强度、arg(a) 控**干涉相位**，
//      权重取 Dirichlet 核（闭式、振荡）而非高斯。因为高斯图元会静默
//      丢弃支配相干干涉的相位——那正是量子态区别于经典分布的信息。
//
//   4 层级内蕴。 嵌套球把 2^n 的振幅数压成 2^(n−1) 个同心壳，
//      20 比特态可读为约 5 圈环，而不是 1024 根柱。
//
//   5 一次模拟多视图。 所有原语只读同一份 frame 快照（rvdl_qgfx_frame_*），
//      切换视图零重算。
//
// ───────────────────────────────────────────────────────────────────────
// 与 cgfx/cgui 共用同一 Nuklear 窗口（qgui::RenderContext），故 qgfx
// 必须在 cgui_begin_frame / cgui_end_frame 之间调用。
//
#include "RuntimeApi.h"
#include "qhal/QuantumState.hpp"

#include <cstdint>

#ifndef RVDL_HOST_EXPORT
#define RVDL_HOST_EXPORT extern "C" RVDL_RT_API
#endif

namespace qhal
{
    struct QuantumGfxState
    {
        int32_t initialized = 0;
        // 进程内帧表：句柄 → 帧索引。刻意用「索引 + 世代号」而非裸指针，
        // 使 use-after-free 退化为可检出的失效句柄而非野指针解引用。
        std::vector<QuantumFrame> frames;
        std::vector<uint32_t> generations;
    };

    inline QuantumGfxState &qgfx_state()
    {
        static QuantumGfxState s;
        return s;
    }

    // O(1) 分配帧槽位，返回句柄。高位编码世代号。
    inline uint64_t qgfx_alloc_frame()
    {
        auto &st = qgfx_state();
        for (size_t i = 0; i < st.frames.size(); ++i)
        {
            if (st.generations[i] == 0)
            {
                st.generations[i] = 1;
                return (uint64_t(1) << 48) | uint64_t(i);
            }
        }
        st.frames.emplace_back();
        st.generations.push_back(1);
        return (uint64_t(1) << 48) | uint64_t(st.frames.size() - 1);
    }

    // 句柄 → 帧引用；失效句柄返回 nullptr（调用方须判空）。
    inline QuantumFrame *qgfx_resolve(uint64_t handle)
    {
        if (handle == 0)
            return nullptr;
        if ((handle >> 48) != 1)
            return nullptr;
        auto &st = qgfx_state();
        const size_t idx = size_t(handle & 0xFFFFFFFFFFFFull);
        if (idx >= st.frames.size() || st.generations[idx] == 0)
            return nullptr;
        return &st.frames[idx];
    }

    inline const QuantumFrame *qgfx_resolve_const(uint64_t handle)
    {
        return qgfx_resolve(handle);
    }

    inline void qgfx_release_frame(uint64_t handle)
    {
        if (!qgfx_resolve(handle))
            return; // 句柄已失效 → 幂等返回（重复 release 安全）
        auto &st = qgfx_state();
        const size_t idx = size_t(handle & 0xFFFFFFFFFFFFull);
        st.generations[idx] = 0; // 世代号清零 → 槽位可复用
    }
} // namespace qhal

extern "C"
{
    // ═══ 帧生命周期（P5：一次模拟，多视图）═══════════════════════════

    // 新建空帧，返回句柄。句柄必须经 qgfx_release_frame 释放。
    //
    // 【契约】新帧的振幅为全零 —— 这是**非物理**状态，归一化残差为 1.0。
    // 刻意不填成 |0…0>：零向量如实标记「此帧尚无有效数据」，而 |0…0> 会
    // 伪装成「已捕获到一个合法态」，在诊断读取器里被误读为真空态。
    // 调用方应先检查 rvdl_qgfx_frame_residual() 再信任任何谓词读数。
    RVDL_HOST_EXPORT uint64_t rvdl_qgfx_frame_new(void);

    // 从量子后端捕获一次振幅快照。
    // backend 是 qhal::IQuantumBackend*（按 void* 传入以保持 C ABI 纯净）。
    // 返回捕获到的振幅数（0 表示后端不可用或未分配量子比特，此时帧保持
    // |0…0> —— 调用方应用 rvdl_qgfx_frame_residual 确认是否可信）。
    RVDL_HOST_EXPORT int32_t rvdl_qgfx_frame_capture(uint64_t handle, void *backend,
                                                    int32_t num_qubits);

    // 直接以显式振幅设置帧（re/im 交错，长度 = 2 * 2^num_qubits）。
    RVDL_HOST_EXPORT void rvdl_qgfx_frame_set_amplitudes(uint64_t handle, const double *re_im,
                                                        int32_t count, int32_t num_qubits);

    // 释放帧。重复释放安全（幂等）。
    RVDL_HOST_EXPORT void rvdl_qgfx_frame_release(uint64_t handle);

    // ═══ 诊断与投影内省（1）═══════════════════════════════════════

    // 归一化残差 |1−‖ψ‖|。>1e-6 意味着捕获到了未归一化态，概率轴不可信，
    // 渲染层应显示为诊断告警而非默默归一化。
    RVDL_HOST_EXPORT double rvdl_qgfx_frame_residual(uint64_t handle);

    RVDL_HOST_EXPORT int32_t rvdl_qgfx_frame_num_qubits(uint64_t handle);
    RVDL_HOST_EXPORT int32_t rvdl_qgfx_frame_basis_size(uint64_t handle);

    // 投影算子的名称 / 投影前真实维数 / 投影后可见维数。
    // 三者相减即"此投影丢弃了多少自由度"，供 UI 显式告知用户。
    RVDL_HOST_EXPORT const char *rvdl_qgfx_projection_name(int32_t projection);
    RVDL_HOST_EXPORT double rvdl_qgfx_projection_source_dims(int32_t num_qubits);
    RVDL_HOST_EXPORT double rvdl_qgfx_projection_target_dims(int32_t projection,
                                                            int32_t num_qubits);

    // ═══ 2：谓词（O(1) 代数判据，供 UI 直接读数）═══════════════════

    // 两比特 concurrence（Wootters，纯态精确式）。非 2 比特返回 0。
    RVDL_HOST_EXPORT double rvdl_qgfx_concurrence(uint64_t handle);

    // 四元数投影坐标 (θ, φ, 四元数部分, 全局相位) 写入 out[4]。
    // quaternionic ≈ 0 ⟺ 可分离。out == nullptr 时仅返回是否有效。
    RVDL_HOST_EXPORT int32_t rvdl_qgfx_quaternionic(uint64_t handle, double *out4);

    // 冯·诺依曼熵（比特）。whole==1 时对全体求（纯态恒为 0）。
    RVDL_HOST_EXPORT double rvdl_qgfx_entropy(uint64_t handle, int32_t first_qubit,
                                             int32_t count, int32_t whole);

    // 条码：把 Schmidt 谱写入 out[2*max_bars]（birth,death 成对），
    // 返回条数。max_bars <= 0 时仅返回所需条数。
    RVDL_HOST_EXPORT int32_t rvdl_qgfx_barcode(uint64_t handle, int32_t first_qubit,
                                              int32_t count, double *out_bars, int32_t max_bars);

    // 状态同一性判据：对局域幺正不变。分属同一等价类返回 1。
    RVDL_HOST_EXPORT int32_t rvdl_qgfx_same_class(uint64_t a, uint64_t b, int32_t first_qubit,
                                                 int32_t count, double tol);

    // ═══ 3：复值精确幅度图元 ═════════════════════════════════════
    //
    // 幅度 = Dirichlet 加权的复值核（精确、振荡、闭式 O(1)）。
    // |a| 控强度，arg(a) 控干涉相位；相位以返回的 double 交由调用方着色，
    // 也可由 rvdl_qgfx_amplitude_hsv 直接取感知均匀的相位→色相。
    RVDL_HOST_EXPORT double rvdl_qgfx_amplitude(uint64_t handle, int32_t basis, int32_t peaks);

    // 振幅 + 它的相位（弧度）。零振幅处相位无定义，返回 0。
    RVDL_HOST_EXPORT double rvdl_qgfx_amplitude_phase(uint64_t handle, int32_t basis);

    // 相邻基态间的干涉项 Re(a_i a_j*)：量子与经典分布的分界信号。
    // 经典直方图永远为 0（无相位概念），此处一般非 0。
    RVDL_HOST_EXPORT double rvdl_qgfx_interference(uint64_t handle, int32_t i, int32_t j);

    // 感知均匀的相位→色相映射（OKLCH 简化版，hue ∈ [0,360)）。
    // 把 arg(a) 变成视觉上等距的色相，避免 HSV 在色相环上的强烈畸变。
    RVDL_HOST_EXPORT double rvdl_qgfx_phase_hue(double phase_radians);

    // ═══ 4：嵌套层级 ═════════════════════════════════════════════

    // 嵌套球壳数 = 2^(n−1)。壳 0 最外，壳 count−1 最内（纠缠所在）。
    RVDL_HOST_EXPORT int32_t rvdl_qgfx_nested_shells(int32_t num_qubits);

    // 第 shell 层的占据概率总和（用于把 2^n 个振幅压成 2^(n−1) 个壳）。
    // 这是 P4「层级内蕴于数据结构」的具体实现：无需用户指定 LOD，
    // 壳的划分由量子比特分层唯一确定。
    RVDL_HOST_EXPORT double rvdl_qgfx_nested_mass(uint64_t handle, int32_t shell,
                                                 int32_t total_shells);

    // 局域 Bloch 极坐标 (θ, φ)，写入 out2。任意比特在纯态下 |r| = 1。
    RVDL_HOST_EXPORT void rvdl_qgfx_bloch(uint64_t handle, int32_t qubit, double *out2);

    // Born 概率与相位（原子操作，供自定义图元使用）。
    RVDL_HOST_EXPORT double rvdl_qgfx_probability(uint64_t handle, int32_t basis);
}