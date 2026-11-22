#pragma once
#include "RuntimeApi.h"
#include "qhal/QuantumGfx.hpp"

#include <cstdint>

#ifndef RVDL_HOST_EXPORT
#define RVDL_HOST_EXPORT extern "C" RVDL_RT_API
#endif

extern "C"
{

    RVDL_HOST_EXPORT void rvdl_qqui_qubit_glyph(int32_t x, int32_t y, int32_t size,
                                                uint64_t frame, int32_t qubit,
                                                int32_t color, int32_t alpha);

    RVDL_HOST_EXPORT void rvdl_qqui_qubit_row(int32_t x, int32_t y, int32_t cell,
                                             uint64_t frame, int32_t first_qubit,
                                             int32_t count, int32_t color, int32_t alpha);

    RVDL_HOST_EXPORT double rvdl_qqui_entangle_link(int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                                                   uint64_t frame, int32_t qa, int32_t qb,
                                                   int32_t color, int32_t alpha);


    RVDL_HOST_EXPORT void rvdl_qqui_uncertainty_bin(int32_t x, int32_t baseline_y,
                                                   int32_t slot_w, double probability,
                                                   double shot_noise,
                                                   int32_t color, int32_t alpha);

    RVDL_HOST_EXPORT void rvdl_qqui_wigner_slice(int32_t x, int32_t y, int32_t w, int32_t h,
                                                uint64_t frame, int32_t first_qubit,
                                                int32_t res);


    RVDL_HOST_EXPORT void rvdl_qqui_nested_bar(int32_t x, int32_t y, int32_t cell,
                                              uint64_t frame, int32_t color, int32_t alpha);


    RVDL_HOST_EXPORT int32_t rvdl_qqui_amplitude_strip(int32_t x, int32_t y, int32_t w, int32_t h,
                                                      uint64_t frame, int32_t color, int32_t alpha);

    RVDL_HOST_EXPORT void rvdl_qqui_state_card(int32_t x, int32_t y, int32_t w,
                                              uint64_t frame, int32_t projection,
                                              int32_t color, int32_t alpha);
}