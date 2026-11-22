#pragma once
#include <cmath>
#include <cstdint>
#include "../qhal/Export.hpp"

#ifndef RVDL_HOST_EXPORT
#define RVDL_HOST_EXPORT extern "C" RVDL_RT_API
#endif

extern "C"
{
    RVDL_HOST_EXPORT double rvdl_shannon4(int32_t n0, int32_t n1, int32_t n2, int32_t n3);
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