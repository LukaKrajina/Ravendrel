#pragma once
#include "RuntimeApi.h"
#include <cstdint>

#ifndef RVDL_HOST_EXPORT
#define RVDL_HOST_EXPORT extern "C" RVDL_RT_API
#endif

extern "C"
{
    RVDL_HOST_EXPORT void gh_syn_invalidate(void);

    RVDL_HOST_EXPORT int32_t gh_syn_line_count(void);
    RVDL_HOST_EXPORT int32_t gh_syn_line_begin(int32_t line);
    RVDL_HOST_EXPORT int32_t gh_syn_line_end(int32_t line);
    RVDL_HOST_EXPORT int32_t gh_syn_line_len(int32_t line);

    RVDL_HOST_EXPORT const char *gh_syn_line(int32_t line);
    RVDL_HOST_EXPORT int32_t gh_syn_class_of(int32_t line, int32_t col);

    RVDL_HOST_EXPORT const char *gh_syn_slice(int32_t line, int32_t b0, int32_t b1);


    RVDL_HOST_EXPORT int32_t gh_syn_cp_index(int32_t line, int32_t byte_col);
    RVDL_HOST_EXPORT int32_t gh_syn_cp_line(int32_t cp_index);
    RVDL_HOST_EXPORT int32_t gh_syn_cp_col(int32_t cp_index);

    RVDL_HOST_EXPORT int32_t gh_syn_bytes(void);
    RVDL_HOST_EXPORT int32_t gh_syn_language(void);
}