#pragma once
// ============================================================================
// MoeAbi.hpp —— MoE 对偶融合的 RVDL 宿主 ABI
//
// 把 numqk/moe.hpp 的算法内核暴露成 rvdl 可直接调用的内建函数，于是模型层
// （.rvdl）不再需要硬编码 num_experts，也不必手写「专家 A × 专家 B」的组合式：
//
//   · 拓扑推导：moe_groups / moe_experts_per / moe_virtual_experts / moe_savings
//       —— 由模型 pB 自动推导（0.5pB→4、1pB→4、3pB→18、5pB→32）
//   · 曲率门控：curvature_gate(g) = σ(−g)
//   · 对偶融合：dual_fuse(a, b, g, theta) —— 两专家的曲率玻尔兹曼混合
//       （专家 A 原空间线性 × 专家 B T-对偶反转 + polymer 铁磁权重）
//   · 完整路由：dual_moe_forward(...) —— 乘积键选 top-2 再做对偶融合
//
// 与 SoftLogicAbi / GeodesicAbi 同一套模式：extern "C" 声明 + RVDL_RT_BUILD
// 下的实现，由 JIT.hpp 的 HostApiMap 注册给 rvdl 代码。
// ============================================================================

#include <cmath>
#include <cstddef>
#include <cstdint>

#include "numqk/moe.hpp"

#ifndef RVDL_HOST_EXPORT
#define RVDL_HOST_EXPORT extern "C" RVDL_RT_API
#endif

extern "C"
{
    // ─── 拓扑自动推导（替代 config 里的 num_experts 硬编码）─────────────────
    // 语义上是整数（G / K / G×K），但 rvdl 侧 scalarMathFns 统一以 double 返回，
    // 故这里也返回 double（值为 2.0 / 4.0 / 32.0 等整值），避免签名表分叉。
    /** 组数 G（曲率分箱）：clamp(2 + floor(log2(pb)), 2, 4) */
    RVDL_HOST_EXPORT double rvdl_moe_groups(double pb);
    /** 每组专家 K：clamp(round(2·pb), 2, 8) */
    RVDL_HOST_EXPORT double rvdl_moe_experts_per(double pb);
    /** 等效专家数 G×K（表达力） */
    RVDL_HOST_EXPORT double rvdl_moe_virtual_experts(double pb);
    /** 高效倍数 (G×K)/(G+K)：比朴素展开 G×K 个专家省多少 */
    RVDL_HOST_EXPORT double rvdl_moe_savings(double pb);

    // ─── 曲率门控与对偶融合 ────────────────────────────────────────────────
    /** 曲率玻尔兹曼门控 σ(−g)：g 大 → 权重小 → 切到对偶专家 */
    RVDL_HOST_EXPORT double rvdl_curvature_gate(double g);
    /** 两专家对偶前向融合：σ(−g)·(a·θ) + (1−σ(−g))·inversion(b)·polymer */
    RVDL_HOST_EXPORT double rvdl_dual_fuse(double expert_a, double expert_b,
                                           double g, double theta);

    // ─── 完整路由（乘积键选 top-2 + 对偶融合）───────────────────────────────
    /**
     * activations[0..G*K)  = 各专家对当前输入的标量响应
     * group_gates[0..G)    = 组（曲率 regime）门控
     * intra_gates[0..K)    = 组内专家门控
     * 返回：路由选出的 top-2 专家经曲率对偶融合后的输出
     */
    RVDL_HOST_EXPORT double rvdl_dual_moe_forward(const double *activations,
                                                  const double *group_gates,
                                                  const double *intra_gates,
                                                  std::int32_t G, std::int32_t K,
                                                  double g, double theta);
}

#if defined(RVDL_RT_BUILD)

double rvdl_moe_groups(double pb)
{
    return static_cast<double>(numqk::moe::auto_moe_topology(pb).groups);
}

double rvdl_moe_experts_per(double pb)
{
    return static_cast<double>(numqk::moe::auto_moe_topology(pb).experts_per);
}

double rvdl_moe_virtual_experts(double pb)
{
    return static_cast<double>(numqk::moe::auto_moe_topology(pb).virtual_experts);
}

double rvdl_moe_savings(double pb)
{
    return numqk::moe::auto_moe_topology(pb).savings;
}

double rvdl_curvature_gate(double g)
{
    return numqk::moe::curvature_gate(g);
}

double rvdl_dual_fuse(double expert_a, double expert_b, double g, double theta)
{
    return numqk::moe::dual_fuse(expert_a, expert_b, g, theta);
}

double rvdl_dual_moe_forward(const double *activations, const double *group_gates,
                             const double *intra_gates, std::int32_t G, std::int32_t K,
                             double g, double theta)
{
    if (!activations || !group_gates || !intra_gates || G <= 0 || K <= 0)
        return 0.0;
    numqk::moe::DualRoute r = numqk::moe::dual_moe_forward(
        activations, static_cast<size_t>(G), static_cast<size_t>(K),
        group_gates, intra_gates, g, theta);
    return r.value;
}

#endif // RVDL_RT_BUILD
