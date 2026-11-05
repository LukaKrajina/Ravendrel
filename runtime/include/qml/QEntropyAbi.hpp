#pragma once
//
// QEntropyAbi.hpp —— 测量熵（非酉性香农熵）语言层 ABI 桥接
//
// 量子混沌的「丰富度」不能用邻近态重叠度量：纯酉演化是等距的，
// |⟨Uψ|U(ψ+δ)⟩|² = |⟨ψ|ψ+δ⟩|² 恒定不变。真正的量子混沌量化必须引入
// 测量坍缩的非酉性 —— 这正是「测量诱导相变」（MIPT，Nature 2023）与
// 「多体量子混沌边缘 QRC」（PRL 2026）的核心：
//
//   有序（dt→0）   → 测量分布退化 → 香农熵 S → 0
//   混沌边缘       → 分布部分混合 → 0 < S < ln d（最优学习点，PRL 2026）
//   强混沌（dt 大）→ 分布均匀     → S → ln d
//
//   S = -Σ p_i ln p_i，p_i 为「漂移 + 测量」后第 i 个涌现情绪态的频率。
//
// 本文件提供标量香农熵 ABI（rvdl 语言层无 ln 数学原语，故在 C++ 侧计算）。
//
#include <cmath>
#include <cstdint>
#include "../qhal/Export.hpp" // RVDL_RT_API

#ifndef RVDL_HOST_EXPORT
#define RVDL_HOST_EXPORT extern "C" RVDL_RT_API
#endif

extern "C"
{
    // 4 态香农熵：S = -Σ_{i=0..3} p_i ln p_i，p_i = n_i / (n0+n1+n2+n3)
    RVDL_HOST_EXPORT double rvdl_shannon4(int32_t n0, int32_t n1, int32_t n2, int32_t n3);
    // 8 态香农熵（通用多情绪态版本）
    RVDL_HOST_EXPORT double rvdl_shannon8(int32_t n0, int32_t n1, int32_t n2, int32_t n3,
                                         int32_t n4, int32_t n5, int32_t n6, int32_t n7);
}

#if defined(RVDL_RT_BUILD)
namespace
{
    inline double shannon_of(const int32_t *counts, int nbins)
    {
        int64_t total = 0;
        for (int i = 0; i < nbins; ++i)
            total += counts[i];
        if (total <= 0)
            return 0.0;
        double S = 0.0;
        for (int i = 0; i < nbins; ++i)
        {
            if (counts[i] > 0)
            {
                double p = static_cast<double>(counts[i]) / static_cast<double>(total);
                S -= p * std::log(p);
            }
        }
        return S;
    }
}

double rvdl_shannon4(int32_t n0, int32_t n1, int32_t n2, int32_t n3)
{
    const int32_t c[4] = {n0, n1, n2, n3};
    return shannon_of(c, 4);
}

double rvdl_shannon8(int32_t n0, int32_t n1, int32_t n2, int32_t n3,
                   int32_t n4, int32_t n5, int32_t n6, int32_t n7)
{
    const int32_t c[8] = {n0, n1, n2, n3, n4, n5, n6, n7};
    return shannon_of(c, 8);
}
#endif
