#pragma once
//
// Quantum UI Library（量子 UI 库）—— qqui_* 内建函数的 C ABI
//
// ═══════════════════════════════════════════════════════════════════════
// 范式：谓词耦合的量子控件 (Predicate-Coupled Quantum Widgets, PCQW)
// ═══════════════════════════════════════════════════════════════════════
//
// cgui_* 是**经典**立即模式控件库：按钮、文本、面板。它没有"量子"
// 概念——不能表达"这个门此刻是超纠缠的"、"这个态与参考态属同一等价类"。
//
// qqui_* 在控件层引入两条经典 UI 从未需要的设计原则：
//
//   1 控件本身携带谓词。 量子控件不是纯视觉装饰，而是**判据的可读载体**。
//       每个纠缠相关控件必须能回答「从图形上我真的能读出这个结论吗」。
//       例如 qubit_row 的连线粗细 ∝ Schmidt 间隙，分离时收成虚线并转灰——
//       「不可分离」是一个**前注意（pre-attentive）**信号，无需读数字。
//       → 借鉴 Bley et al. (arXiv:2305.07596) 让"对称即可分离"成为
//         视觉属性，而非需要查表计算的性质。
//
//   2 不确定性内生于控件。 量子测量的结果是**分布**，不是标量。
//       ClassicGfx 只能画确定颜色的实心块。qqui_* 提供 bin-glyph：
//       垂直位置 = 概率，**列宽 = 不确定度**。
//       → 借鉴 Wallinger et al. (arXiv:2504.12776, StorySets)：分箱
//         直方图 + 曲线序关系把"集合包含"变成"曲线交叠"，可瞬时判读。
//
//   3 控件布局由量子结构决定。 嵌套条（nested bar）把 2^n 个振幅折叠成
//       2^(n−1) 个格子，20 比特态仍是可读的 2 行面板，而不是 1024 列。
//       层级不靠用户调 LOD 参数，而由 rvdl_qgfx_nested_shells() 直接给出。
//
//   4 交互式图元优先于读数。 "点一个振幅，看它变成什么概率" 是
//       QuantumEyes (arXiv:2311.07980) 与 GuiTeNet (arXiv:1808.00532)
//       共同验证的模式：每次视觉操作都应产生可代码化的结论。
//       qqui_amplitude_strip 返回被选中的基态索引；调用方据此取
//       rvdl_qgfx_probability / phase / interference。
//
// 与 cgui 共享同一 Nuklear 窗口与帧生命周期：必须在
// cgui_begin_frame / cgui_end_frame 之间调用。经典图层 (cgfx/cgui) 与
// 量子图层 (qgfx/qqui) 可在同一帧内任意混用。
//
#include "RuntimeApi.h"
#include "qhal/QuantumGfx.hpp"

#include <cstdint>

#ifndef RVDL_HOST_EXPORT
#define RVDL_HOST_EXPORT extern "C" RVDL_RT_API
#endif

extern "C"
{
    // ═══ 1：携带谓词的量子控件 ═════════════════════════════════════

    // 单个比特的 Bloch 字形：半径 = 局域纯度在 [0,1] 上的体现，
    // 盘面朝向 = Bloch 极坐标。纯态下半径恒为 1（r 是态矢量长度），
    // 半径变化只在混态下出现——这是**诚实的**视觉编码。
    RVDL_HOST_EXPORT void rvdl_qqui_qubit_glyph(int32_t x, int32_t y, int32_t size,
                                                uint64_t frame, int32_t qubit,
                                                int32_t color, int32_t alpha);

    // 比特行：一条带若干 qubit 字形的横带，右侧留出谓词标注槽。
    // count 个比特由左至右排列；x/y 为整行左上角。
    RVDL_HOST_EXPORT void rvdl_qqui_qubit_row(int32_t x, int32_t y, int32_t cell,
                                             uint64_t frame, int32_t first_qubit,
                                             int32_t count, int32_t color, int32_t alpha);

    // 纠缠连线：连接两个比特的**宽度编码 Schmidt 间隙**的连线。
    // 可分离（间隙 ≈ 0）时画成灰色虚线——「不纠缠」因此是前注意信号。
    // 返回实际绘制的间隙值，供调用方做数值断言。
    RVDL_HOST_EXPORT double rvdl_qqui_entangle_link(int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                                                   uint64_t frame, int32_t qa, int32_t qb,
                                                   int32_t color, int32_t alpha);

    // ═══ 2：不确定性 bin-glyph ═════════════════════════════════════

    // 单个基态的概率 bin：竖直条，高度 ∝ p，**宽度 ∝ shot_noise**。
    // shot_noise 为该概率的测量标准差（如 1 次测量时 p(1−p)）。
    // 宽度越宽 = 越不可信，从而"置信度"成为几何属性而非附加文字。
    RVDL_HOST_EXPORT void rvdl_qqui_uncertainty_bin(int32_t x, int32_t baseline_y,
                                                   int32_t slot_w, double probability,
                                                   double shot_noise,
                                                   int32_t color, int32_t alpha);

    // Wigner 截面：把相空间截面渲染为带符号的发散色标（负值蓝、正值红），
    // 这是相空间数据的标准惯例：Wigner 函数可以为负，而概率不能。
    RVDL_HOST_EXPORT void rvdl_qqui_wigner_slice(int32_t x, int32_t y, int32_t w, int32_t h,
                                                uint64_t frame, int32_t first_qubit,
                                                int32_t res);

    // ═══ 3：嵌套条（Wigner 条布局）═════════════════════════════════

    // 把 frame 折叠为 shells × columns 的嵌套条带。shells = 2^(n−1)。
    // 这就是 P4「层级内蕴于数据」的控件实现：调用者不选 LOD，
    // 布局由量子比特数唯一确定。
    RVDL_HOST_EXPORT void rvdl_qqui_nested_bar(int32_t x, int32_t y, int32_t cell,
                                              uint64_t frame, int32_t color, int32_t alpha);

    // ═══ 4：交互式图元（每次交互产生可代码化结论）═════════════════

    // 振幅条带：绘制 2^min(n, 12) 个基态的概率条。返回被点击的基态索引，
    // 未点击返回 −1。调用方据此取 rvdl_qgfx_probability / phase /
    // interference，完成"点一个振幅，看它变成什么"的交互闭环。
    RVDL_HOST_EXPORT int32_t rvdl_qqui_amplitude_strip(int32_t x, int32_t y, int32_t w, int32_t h,
                                                      uint64_t frame, int32_t color, int32_t alpha);

    // 态对摘要卡：绘制 concurrence、熵、归一化残差三个读数，
    // 并**显式标注投影损失了多少自由度**（P1 的 UI 落地）。
    // 若残差 > 1e-6 则显示为告警态而非静默归一化。
    RVDL_HOST_EXPORT void rvdl_qqui_state_card(int32_t x, int32_t y, int32_t w,
                                              uint64_t frame, int32_t projection,
                                              int32_t color, int32_t alpha);
}