#pragma once
// ============================================================================
// TauAbi.hpp —— 量子时间缩微调度（τ scaling）的 rvdl 宿主 ABI
//
// 把 qml/TauScale.hpp 的算法内核暴露成 rvdl 可直接调用的内建标量函数。
// 数学灵感来源是通读韬定律论文（ChinaXiv:202605.00224v2）的时间缩微理论后，延伸到
// 量子 AI 层后的六个标量入口（全部 double，方便模型层组合）：
//
//   · tau_alpha(preset)          —— 领域年度缩放因子（0 移动 1.3 / 1 自驾 1.5 / 2 AI 10）
//   · tau_generational(tau, α)   —— 代际法则 τ_{n+1} = τ_n / α
//   · tau_fold_depth(L, T)       —— LogicFolding：串行深度 L 折进 T 层后的关键路径 ⌈L/T⌉
//   · tau_gear_ratio(E_c, E_i)   —— 齿轮比 g = 跨层边 / 层内边（论文阈值 3）
//   · tau_route_cost(D, g, tg, tc)—— 关键路径+跨层通信的总调度代价
//   · tau_route_price(v, c, λ)   —— λ 时间影子价格下的净价值 v − λ·c（预算路由门控）
//
// 与 MoeAbi / SoftLogicAbi 同一套模式：extern "C" 声明 + RVDL_RT_BUILD 下的
// 实现，由 JIT.hpp 的 HostApiMap 注册给 rvdl 代码。
// ============================================================================

#include <cstdint>

#include "qml/TauScale.hpp"

#ifndef RVDL_HOST_EXPORT
#define RVDL_HOST_EXPORT extern "C" RVDL_RT_API
#endif

extern "C"
{
    // ─── 代际法则（论文第 2 节：τ_{n+1} = τ_n / α）───────────────────────
    /** 领域年度缩放因子：0 移动设备 1.3 / 1 自动驾驶 1.5 / 2 AI 10.0；其余 1.0 */
    RVDL_HOST_EXPORT double rvdl_tau_alpha(double preset);
    /** 一代时间缩微：τ/α（α ≤ 0 返原值） */
    RVDL_HOST_EXPORT double rvdl_tau_generational(double tau, double alpha);

    // ─── 关键路径与折叠（论文第 4 节 LogicFolding）────────────────────────
    /** 折叠深度：串行深度 L 折进 T 个并行层后的关键路径 ⌈L/T⌉ */
    RVDL_HOST_EXPORT double rvdl_tau_fold_depth(double serial_depth, double tiers);
    /** 齿轮比：g = E_cross / E_intra（intra ≤ 0 且 cross > 0 时返回大数 2^53） */
    RVDL_HOST_EXPORT double rvdl_tau_gear_ratio(double e_cross, double e_intra);
    /** 调度总代价：τ_gate·D + τ_comm·D·g（每跨一层付一次通信，量正比齿轮比） */
    RVDL_HOST_EXPORT double rvdl_tau_route_cost(double depth, double gear,
                                                double tau_gate, double tau_comm);

    // ─── 时间预算路由（论文 AI 领域 α→10 的调度对偶）──────────────────────
    /** λ 影子价格下的净价值 v − λ·c：>0 表示值得在当前时间价格下调度 */
    RVDL_HOST_EXPORT double rvdl_tau_route_price(double value, double cost, double lambda);
}

#if defined(RVDL_RT_BUILD)

double rvdl_tau_alpha(double preset)
{
    return numqk::tau::alpha_preset(static_cast<std::int32_t>(preset));
}

double rvdl_tau_generational(double tau, double alpha)
{
    return numqk::tau::generational(tau, alpha);
}

double rvdl_tau_fold_depth(double serial_depth, double tiers)
{
    return numqk::tau::fold_depth(serial_depth, tiers);
}

double rvdl_tau_gear_ratio(double e_cross, double e_intra)
{
    return numqk::tau::gear_ratio(e_cross, e_intra);
}

double rvdl_tau_route_cost(double depth, double gear, double tau_gate, double tau_comm)
{
    return numqk::tau::route_cost(depth, gear, tau_gate, tau_comm);
}

double rvdl_tau_route_price(double value, double cost, double lambda)
{
    return numqk::tau::route_price(value, cost, lambda);
}

#endif