#pragma once
//
// GeodesicAbi.hpp —— 量子非欧几里德曲面体几何原语 ABI 桥接
//
// 把「量子态流形（复投影空间 CP^{d-1}，其黎曼度量为 Fubini-Study 度量）」与
// 「弦论 O(D,D) 双空间（T-对偶）」的几何运算暴露为 rvdl 语言层内置函数，供
// Chimera 的「曲面体」组件自由组合四种几何移动（取代 Transformer 的「层堆叠」）：
//   投影（projective）：FS 测地线（指数映射的离散化）
//   反转（inversion） ：T-对偶 R → 1/R（O(D,D) 双空间对偶）
//   平行（parallel）  ：Berry 相位（参数空间和乐，后续扩展）
//   交叉（crossing）  ：section condition（双坐标场交叉，后续扩展）
//
// 第一版全部返回 double（纯数值），操作 QObject 态或标量参数。
//
//   rvdl_geodesic_distance(QObject* a, QObject* b) -> double  FS 测地线距离 arccos|⟨a|b⟩| ∈ [0, π/2]
//   rvdl_inversion(double R)                      -> double  T-对偶半径 R → 1/R
//   rvdl_hyperbolic_metric(double x)              -> double  Poincaré ball 度规 4/(1-|x|²)²
//   rvdl_hyperbolic_distance(double x, double y)  -> double  Poincaré ball 双曲距离
//
// 依赖：rvdl_qattention（SWAP test 重叠 |⟨a|b⟩|²，见 QAttentionAbi.hpp）。

#include "Inference.hpp"        // ::QObject（hardware_ids）、global_qm
#include "QAttentionAbi.hpp"    // rvdl_qattention（态重叠 |⟨a|b⟩|²）
#include <cmath>

#ifndef RVDL_HOST_EXPORT
#define RVDL_HOST_EXPORT extern "C" RVDL_RT_API
#endif

extern "C"
{
    RVDL_HOST_EXPORT double rvdl_geodesic_distance(QObject *a, QObject *b);
    RVDL_HOST_EXPORT double rvdl_inversion(double R);
    RVDL_HOST_EXPORT double rvdl_hyperbolic_metric(double x);
    RVDL_HOST_EXPORT double rvdl_hyperbolic_distance(double x, double y);
}

#if defined(RVDL_RT_BUILD)
double rvdl_geodesic_distance(QObject *a, QObject *b)
{
    if (!a || !b)
        return -1.0;
    // FS 测地线距离 = arccos|⟨a|b⟩|；重叠来自 SWAP test（qattention 的 |⟨a|b⟩|²）。
    double overlap = rvdl_qattention(a, b);
    if (overlap < 0.0)
        overlap = 0.0;
    if (overlap > 1.0)
        overlap = 1.0;
    return std::acos(std::sqrt(overlap));
}

double rvdl_inversion(double R)
{
    // T-对偶：环面半径 R → 1/R（弦论 O(D,D) 双空间的反转变换）。
    if (R == 0.0)
        return 0.0; // 奇点保护（自对偶点 R=1 处反转不动）
    return 1.0 / R;
}

double rvdl_hyperbolic_metric(double x)
{
    // Poincaré ball（单位圆盘）度规张量的标量分量：4/(1-|x|²)²。
    // |x| → 1 时度规发散，对应双曲空间的「无穷远边界」。
    const double x2 = x * x;
    if (x2 >= 1.0)
        return 0.0;
    const double denom = 1.0 - x2;
    return 4.0 / (denom * denom);
}

double rvdl_hyperbolic_distance(double x, double y)
{
    // Poincaré ball 上两点的一维投影双曲距离：
    //   d = arcosh(1 + 2|x−y|² / ((1−|x|²)(1−|y|²)))
    const double x2 = x * x;
    const double y2 = y * y;
    if (x2 >= 1.0 || y2 >= 1.0)
        return -1.0;
    const double dx = x - y;
    const double t = 1.0 + 2.0 * dx * dx / ((1.0 - x2) * (1.0 - y2));
    if (t < 1.0)
        return 0.0;
    return std::acosh(t);
}
#endif