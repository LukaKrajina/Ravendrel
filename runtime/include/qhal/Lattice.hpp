#pragma once
#include "RuntimeApi.h"
#include <cstdint>
#include <cstdio>
#include <vector>

#ifndef RVDL_HOST_EXPORT
#define RVDL_HOST_EXPORT extern "C" RVDL_RT_API
#endif

namespace qhal
{
    struct Lattice
    {
        int32_t rank = 0;
        int64_t dims[2] = {0, 0};
        int32_t boundary = 0;
        std::vector<int32_t> data;

        inline int64_t index(int64_t x, int64_t y) const
        {
            int64_t ix = resolve(0, x);
            int64_t iy = resolve(1, y);
            if (ix < 0 || iy < 0)
                return -1;
            return iy * dims[0] + ix;
        }

        inline int64_t resolve(int dim, int64_t v) const
        {
            if (dim >= rank)
                return 0;
            int64_t n = dims[dim];
            if (n <= 0)
                return 0;
            switch (boundary)
            {
            case 1:
                v = v % n;
                if (v < 0)
                    v += n;
                return v;
            case 2:
                v = v % (2 * n);
                if (v < 0)
                    v += 2 * n;
                if (v >= n)
                    v = 2 * n - 1 - v;
                return v;
            default:
                if (v < 0 || v >= n)
                    return -1;
                return v;
            }
        }
    };
}

extern "C"
{
    RVDL_HOST_EXPORT void *rvdl_lattice_new(int32_t rank, int32_t d0, int32_t d1, int32_t boundary);
    RVDL_HOST_EXPORT void rvdl_lattice_free(void *L);
    RVDL_HOST_EXPORT int32_t rvdl_lattice_ref(void *L, int32_t x, int32_t y);
    RVDL_HOST_EXPORT void rvdl_lattice_set(void *L, int32_t x, int32_t y, int32_t value);
    RVDL_HOST_EXPORT int32_t rvdl_lattice_rank(void *L);
    RVDL_HOST_EXPORT int32_t rvdl_lattice_size(void *L, int32_t dim);
    RVDL_HOST_EXPORT int32_t rvdl_lattice_boundary(void *L);
}

#if defined(RVDL_RT_BUILD)

void *rvdl_lattice_new(int32_t rank, int32_t d0, int32_t d1, int32_t boundary)
{
    auto *L = new qhal::Lattice();
    L->rank = rank < 1 ? 1 : (rank > 2 ? 2 : rank);
    L->dims[0] = d0 > 0 ? d0 : 1;
    L->dims[1] = (L->rank > 1 && d1 > 0) ? d1 : 1;
    L->boundary = boundary;
    int64_t total = L->dims[0] * L->dims[1];
    L->data.assign(static_cast<size_t>(total), 0);
    return L;
}

void rvdl_lattice_free(void *L)
{
    delete static_cast<qhal::Lattice *>(L);
}

int32_t rvdl_lattice_ref(void *L, int32_t x, int32_t y)
{
    auto *lat = static_cast<qhal::Lattice *>(L);
    if (!lat)
        return 0;
    int64_t i = lat->index(x, y);
    if (i < 0 || i >= static_cast<int64_t>(lat->data.size()))
        return 0;
    return lat->data[static_cast<size_t>(i)];
}

void rvdl_lattice_set(void *L, int32_t x, int32_t y, int32_t value)
{
    auto *lat = static_cast<qhal::Lattice *>(L);
    if (!lat)
        return;
    int64_t i = lat->index(x, y);
    if (i < 0 || i >= static_cast<int64_t>(lat->data.size()))
        return;
    lat->data[static_cast<size_t>(i)] = value;
}

int32_t rvdl_lattice_rank(void *L)
{
    auto *lat = static_cast<qhal::Lattice *>(L);
    return lat ? lat->rank : 0;
}

int32_t rvdl_lattice_size(void *L, int32_t dim)
{
    auto *lat = static_cast<qhal::Lattice *>(L);
    if (!lat || dim < 0 || dim >= 2)
        return 0;
    return static_cast<int32_t>(lat->dims[dim]);
}

int32_t rvdl_lattice_boundary(void *L)
{
    auto *lat = static_cast<qhal::Lattice *>(L);
    return lat ? lat->boundary : 0;
}

#endif