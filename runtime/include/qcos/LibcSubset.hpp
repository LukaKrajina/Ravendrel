#pragma once
// ============================================================================
//  qcos/LibcSubset.hpp —— 内核所需的极小 libc 子集
//
//  freestanding 环境下没有 libc/libstdc++，内核只能依赖编译器内置与这组
//  手写的内存/字符串原语。只覆盖内核启动与调度所需的最小子集。
// ============================================================================

#include <stddef.h>

namespace qcos
{
    inline void* memcpy(void* dst, const void* src, size_t n)
    {
        auto* d = static_cast<unsigned char*>(dst);
        const auto* s = static_cast<const unsigned char*>(src);
        for (size_t i = 0; i < n; ++i) d[i] = s[i];
        return dst;
    }

    inline void* memset(void* dst, int c, size_t n)
    {
        auto* d = static_cast<unsigned char*>(dst);
        const auto v = static_cast<unsigned char>(c);
        for (size_t i = 0; i < n; ++i) d[i] = v;
        return dst;
    }

    inline int memcmp(const void* a, const void* b, size_t n)
    {
        const auto* x = static_cast<const unsigned char*>(a);
        const auto* y = static_cast<const unsigned char*>(b);
        for (size_t i = 0; i < n; ++i)
        {
            if (x[i] != y[i]) return static_cast<int>(x[i]) - static_cast<int>(y[i]);
        }
        return 0;
    }

    inline size_t strlen(const char* s)
    {
        size_t n = 0;
        while (s && s[n]) ++n;
        return n;
    }
}