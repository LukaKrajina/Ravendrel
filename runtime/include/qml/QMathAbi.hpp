#pragma once
//
// QMathAbi.hpp —— nqcmath（普通数学）+ expermath（量子数学）的语言层 ABI 桥接
//
// 把 numqk/nqcmath.hpp（矩阵 / 向量 / 高并发并行层）与 numqk/expermath.hpp
// （自研量子数学：纯度 / 线性熵 / Rényi 熵 / 保真度 / 迹距离 / Pauli 期望 /
// 量子 Fisher 信息 / QFT / Trotter）暴露为 rvdl 语言层可调用的 extern "C" 函数。
//
// 为什么独立于 QStateAbi.hpp：那个文件的量度（保真度 / 纠缠熵）走
// numqk/ComplexTensor 的 CTensor 路径；这里的量度走 numqk/expermath 的
// MatrixX<Complex> 路径 —— expermath 是自研高性能量子数学库（算法依据见
// expermath.hpp 头注释）。两者共存，各有各的数值内核，这也正是「expermath
// 可以被 rvdl 调用」的落点。
//
// 遵循 SoftLogicAbi.hpp 的「extern "C" 声明 + #if RVDL_RT_BUILD 非 inline 定义」
// 模式：非 inline 定义散落在 JIT.hpp 里会导致 ORC JIT 符号无法解析（Symbols not
// found），由 runtime_api.cpp 单点 include，保证符号正确导出。

#include "QStateAbi.hpp"          // ::QObject、global_qm、qml::reduced_density_of
#include "../numqk/nqcmath.hpp"   // MatrixX / Parallel
#include "../numqk/expermath.hpp" // purity / linear_entropy / renyi_entropy

#include <complex>
#include <cstdint>
#include <cmath>
#include <vector>

#ifndef RVDL_HOST_EXPORT
#define RVDL_HOST_EXPORT extern "C" RVDL_RT_API
#endif

namespace qml
{
    /// QObject → 约化密度矩阵（CTensor）→ expermath 的 MatrixX<Complex>。
    ///
    /// 这是两条量子数学库之间的桥：QStateAbi 用 CTensor，expermath 用
    /// MatrixX<Complex>。转一次内存布局（都是行主序，逐元素拷贝即可），
    /// 就让 expermath 的全部密度矩阵算子（纯度 / 熵族 / …）能直接吃 QObject。
    /// 空矩阵（0 行）表示「取不到约化密度矩阵」—— 规模超限或无态矢量时。
    inline numqk::nqcmath::MatrixX<std::complex<double>> rho_matrix_of(
        qhal::IQuantumBackend *be, const std::vector<size_t> &ids)
    {
        numqk::CTensor rho = reduced_density_of(be, ids);
        if (rho.size() == 0 || rho.get_shape().size() != 2)
            return numqk::nqcmath::MatrixX<std::complex<double>>();
        const size_t d = rho.get_shape()[0];
        if (d == 0 || rho.get_shape()[1] != d)
            return numqk::nqcmath::MatrixX<std::complex<double>>();
        numqk::nqcmath::MatrixX<std::complex<double>> m(d, d);
        for (size_t i = 0; i < d * d; ++i)
            m.data()[i] = rho.data()[i];
        return m;
    }
}

extern "C"
{
    // ── expermath 量子态量度（QObject → double）─────────────────────────
    // 语义与 QStateAbi 的 rvdl_qstate_entropy 一致：从 QObject 的约化密度矩阵
    // 出发。无态矢量 / 规模超限（>10 qubit）时返回 -1。

    /// 纯度 Tr(ρ²)（纯态 1，最大混合态 1/d）。
    RVDL_HOST_EXPORT double rvdl_qstate_purity(QObject *obj);
    /// 线性熵 1 − Tr(ρ²)（纯态 0，最大混合态 1−1/d）。
    RVDL_HOST_EXPORT double rvdl_qstate_linear_entropy(QObject *obj);
    /// Rényi 熵 S_α(ρ) = (1/(1−α)) log Tr(ρ^α)；α=1 退化为 von Neumann 熵。
    RVDL_HOST_EXPORT double rvdl_qstate_renyi_entropy(QObject *obj, double alpha);

    // ── nqcmath 向量运算（double* → double）────────────────────────────
    // 输入是 arr<double,N> 的元素指针（rvdl 数组 decay）。走 nqcmath::Parallel
    // 的常驻线程池 + 原子游标动态块归约 —— 这就是「高并发并行数学库」被 rvdl
    // 调用的那一条路。空指针 / n<=0 返回 0。

    /// 点积 Σ a[i]·b[i]。
    RVDL_HOST_EXPORT double rvdl_math_dot(const double *a, const double *b, std::int32_t n);
    /// L2 范数 √(Σ a[i]²)。
    RVDL_HOST_EXPORT double rvdl_math_norm(const double *a, std::int32_t n);
}

#if defined(RVDL_RT_BUILD)

double rvdl_qstate_purity(QObject *obj)
{
    if (!obj || !global_qm)
        return -1.0;
    const numqk::nqcmath::MatrixX<std::complex<double>> m =
        qml::rho_matrix_of(global_qm, obj->hardware_ids);
    if (m.rows() == 0)
        return -1.0;
    return numqk::expermath::purity(m);
}

double rvdl_qstate_linear_entropy(QObject *obj)
{
    if (!obj || !global_qm)
        return -1.0;
    const numqk::nqcmath::MatrixX<std::complex<double>> m =
        qml::rho_matrix_of(global_qm, obj->hardware_ids);
    if (m.rows() == 0)
        return -1.0;
    return numqk::expermath::linear_entropy(m);
}

double rvdl_qstate_renyi_entropy(QObject *obj, double alpha)
{
    if (!obj || !global_qm)
        return -1.0;
    const numqk::nqcmath::MatrixX<std::complex<double>> m =
        qml::rho_matrix_of(global_qm, obj->hardware_ids);
    if (m.rows() == 0)
        return -1.0;
    return numqk::expermath::renyi_entropy(m, alpha);
}

double rvdl_math_dot(const double *a, const double *b, std::int32_t n)
{
    if (!a || !b || n <= 0)
        return 0.0;
    return numqk::nqcmath::Parallel::parallel_reduce<double>(
        static_cast<size_t>(n), 0.0,
        [&](size_t i) { return a[i] * b[i]; },
        [](double x, double y) { return x + y; });
}

double rvdl_math_norm(const double *a, std::int32_t n)
{
    if (!a || n <= 0)
        return 0.0;
    const double sq = numqk::nqcmath::Parallel::parallel_reduce<double>(
        static_cast<size_t>(n), 0.0,
        [&](size_t i) { return a[i] * a[i]; },
        [](double x, double y) { return x + y; });
    return std::sqrt(sq);
}

#endif
