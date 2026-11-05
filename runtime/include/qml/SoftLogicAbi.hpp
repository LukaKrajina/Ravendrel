#pragma once
//
// SoftLogicAbi.hpp —— 神经 / 软逻辑 / polymer 原语 ABI 桥接
//
// 把 numqk/SoftLogic.hpp（玻尔兹曼 softmax / log-sum-exp / mellowmax / tanh 量化 /
// 代理梯度 / t-norm）、qbNs/SpikeBeliefPropagation.hpp（LIF 神经元）、
// qhal/PolymerSampler.hpp（铁磁 polymer 配分函数）、qhal/Qms.hpp（量子 Markov 半群谱隙）
// 暴露为 rvdl 语言层可调用的 extern "C" 函数。
//
// 经调试过程中发现这些原语原本以「非 inline 函数定义」散落在 JIT.hpp 内（JIT.hpp 被多个头文件
// include，导致符号在 LLVM ORC JIT 中无法解析：JIT session error "Symbols not found"）。
// 故此处改为与 GeodesicAbi.hpp 一致的「extern "C" 声明 + #if RVDL_RT_BUILD 非 inline 定义」，
// 由 runtime_api.cpp 单点 include，保证符号正确导出。

#include "../numqk/SoftLogic.hpp"
#include "../qbNs/SpikeBeliefPropagation.hpp"
#include "../qhal/PolymerSampler.hpp"
#include "../qhal/Qms.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>

#ifndef RVDL_HOST_EXPORT
#define RVDL_HOST_EXPORT extern "C" RVDL_RT_API
#endif

extern "C"
{
    // 神经 / 软逻辑原语
    RVDL_HOST_EXPORT double rvdl_surrogate(double x, double theta, double gamma);
    RVDL_HOST_EXPORT double rvdl_tanh_quantize(double w, double alpha, int bits);
    RVDL_HOST_EXPORT double rvdl_lif_step(double v, double current, double tau, double theta);
    RVDL_HOST_EXPORT double rvdl_mellowmax2(double a, double b, double omega);
    RVDL_HOST_EXPORT double rvdl_logsumexp2(double a, double b, double beta);
    RVDL_HOST_EXPORT double rvdl_boltzmann2(double a, double b, double beta);
    RVDL_HOST_EXPORT double rvdl_tnorm_luk(double a, double b);
    RVDL_HOST_EXPORT double rvdl_tnorm_prod(double a, double b);
    RVDL_HOST_EXPORT double rvdl_tnorm_godel(double a, double b);
    RVDL_HOST_EXPORT double rvdl_polymer_weight(double beta, double h, double len);
    RVDL_HOST_EXPORT double rvdl_polymer_mix_bound(double n, double eps);
    // QMS 数值内核
    RVDL_HOST_EXPORT double rvdl_qms_gap(std::int32_t model, double p, double q);
    RVDL_HOST_EXPORT double rvdl_mix_bound(double gap, double n, double eps);
    RVDL_HOST_EXPORT double rvdl_qms_conc(double gap, double eps);
}

#if defined(RVDL_RT_BUILD)

double rvdl_surrogate(double x, double theta, double gamma)
{
    return numqk::surrogate_gradient(x, theta, gamma);
}

double rvdl_tanh_quantize(double w, double alpha, int bits)
{
    return numqk::tanh_quantize_scalar(w, alpha, bits);
}

double rvdl_lif_step(double v, double current, double tau, double theta)
{
    return qbns::lif_step(v, current, tau, theta);
}

double rvdl_mellowmax2(double a, double b, double omega)
{
    return numqk::mellowmax2(a, b, omega);
}

double rvdl_logsumexp2(double a, double b, double beta)
{
    return numqk::logsumexp2(a, b, beta);
}

double rvdl_boltzmann2(double a, double b, double beta)
{
    return numqk::boltzmann2(a, b, beta);
}

double rvdl_tnorm_luk(double a, double b) { return numqk::lukasiewicz_meet(a, b); }
double rvdl_tnorm_prod(double a, double b) { return numqk::product_meet(a, b); }
double rvdl_tnorm_godel(double a, double b) { return numqk::godel_meet(a, b); }

double rvdl_polymer_weight(double beta, double h, double len)
{
    return qhal::PolymerSampler::ferromagnetic_weight(beta, h, static_cast<size_t>(len));
}

double rvdl_polymer_mix_bound(double n, double eps)
{
    return n * std::log(n / eps);
}

double rvdl_qms_gap(std::int32_t model, double p, double q)
{
    return qhal::qms::spectral_gap(model, p, q);
}

double rvdl_mix_bound(double gap, double n, double eps)
{
    return qhal::qms::mixing_bound(gap, n, eps);
}

double rvdl_qms_conc(double gap, double eps)
{
    return qhal::qms::variance_contraction_time(gap, eps);
}

#endif