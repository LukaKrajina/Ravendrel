#pragma once
#include "RuntimeApi.h"
#include <cstdint>

#ifndef RVDL_HOST_EXPORT
#define RVDL_HOST_EXPORT extern "C" RVDL_RT_API
#endif

extern "C"
{
    RVDL_HOST_EXPORT void gh_str_reset(void);
    RVDL_HOST_EXPORT int32_t gh_str_add(int32_t cp);
    RVDL_HOST_EXPORT int32_t gh_str_add_ascii(int32_t c);
    RVDL_HOST_EXPORT int32_t gh_str_backspace(void);
    RVDL_HOST_EXPORT int32_t gh_str_set(const char *utf8);
    RVDL_HOST_EXPORT int32_t gh_str_append(const char *utf8);






    RVDL_HOST_EXPORT int32_t gh_str_insert_str(const char *utf8);

    RVDL_HOST_EXPORT int32_t gh_str_len(void);
    RVDL_HOST_EXPORT int32_t gh_str_bytes(void);
    RVDL_HOST_EXPORT int32_t gh_str_version(void);
    RVDL_HOST_EXPORT int32_t gh_str_codepoint(int32_t index);

    RVDL_HOST_EXPORT const char *gh_str_cstr(void);

    RVDL_HOST_EXPORT int32_t gh_str_insert(int32_t index, int32_t cp);
    RVDL_HOST_EXPORT int32_t gh_str_delete(int32_t index);
    RVDL_HOST_EXPORT int32_t gh_str_truncate(int32_t count);

    RVDL_HOST_EXPORT int32_t gh_str_set_cursor(int32_t pos);
    RVDL_HOST_EXPORT int32_t gh_str_select_extend(int32_t pos);
    RVDL_HOST_EXPORT int32_t gh_str_set_anchor(int32_t pos);
    RVDL_HOST_EXPORT int32_t gh_str_cursor(void);
    RVDL_HOST_EXPORT int32_t gh_str_anchor(void);
    RVDL_HOST_EXPORT int32_t gh_str_select_clear(void);
    RVDL_HOST_EXPORT int32_t gh_str_sel_start(void);
    RVDL_HOST_EXPORT int32_t gh_str_sel_end(void);
    RVDL_HOST_EXPORT int32_t gh_str_has_selection(void);
    RVDL_HOST_EXPORT int32_t gh_str_delete_selection(void);
    RVDL_HOST_EXPORT const char *gh_str_selection(void);

    RVDL_HOST_EXPORT void gh_str_dump(void);
}


extern "C"
{
    RVDL_HOST_EXPORT void gh_ed_reset(void);









    RVDL_HOST_EXPORT int32_t gh_ed_set(const char *utf8, const char *path);
    RVDL_HOST_EXPORT int32_t gh_ed_append(const char *utf8);
    RVDL_HOST_EXPORT int32_t gh_ed_add(int32_t cp);
    RVDL_HOST_EXPORT int32_t gh_ed_backspace(void);

    RVDL_HOST_EXPORT int32_t gh_ed_len(void);
    RVDL_HOST_EXPORT int32_t gh_ed_bytes(void);
    RVDL_HOST_EXPORT int32_t gh_ed_version(void);
    RVDL_HOST_EXPORT int32_t gh_ed_codepoint(int32_t index);
    RVDL_HOST_EXPORT const char *gh_ed_cstr(void);

    RVDL_HOST_EXPORT int32_t gh_ed_set_cursor(int32_t pos);
    RVDL_HOST_EXPORT int32_t gh_ed_cursor(void);
    RVDL_HOST_EXPORT int32_t gh_ed_select_extend(int32_t pos);
    RVDL_HOST_EXPORT int32_t gh_ed_set_anchor(int32_t pos);
    RVDL_HOST_EXPORT int32_t gh_ed_anchor(void);
    RVDL_HOST_EXPORT int32_t gh_ed_select_clear(void);
    RVDL_HOST_EXPORT int32_t gh_ed_sel_start(void);
    RVDL_HOST_EXPORT int32_t gh_ed_sel_end(void);
    RVDL_HOST_EXPORT int32_t gh_ed_has_selection(void);
    RVDL_HOST_EXPORT int32_t gh_ed_delete_selection(void);
    RVDL_HOST_EXPORT const char *gh_ed_selection(void);


    RVDL_HOST_EXPORT int32_t gh_ed_insert_str(const char *utf8);

    RVDL_HOST_EXPORT int32_t gh_ed_newline(void);

    RVDL_HOST_EXPORT int32_t gh_ed_delete_at(int32_t index);

    RVDL_HOST_EXPORT const char *gh_utf8_cp(int32_t cp);

    RVDL_HOST_EXPORT int32_t gh_ed_undo(void);
    RVDL_HOST_EXPORT int32_t gh_ed_redo(void);
    RVDL_HOST_EXPORT int32_t gh_ed_can_undo(void);
    RVDL_HOST_EXPORT int32_t gh_ed_can_redo(void);






    RVDL_HOST_EXPORT int32_t gh_ed_undo_depth(void);

    RVDL_HOST_EXPORT int32_t gh_ed_dirty(void);


    RVDL_HOST_EXPORT int32_t gh_ed_save(const char *utf8_path);

    RVDL_HOST_EXPORT const char *gh_save_error(void);
}


extern "C"
{
    RVDL_HOST_EXPORT int32_t gh_find_set(int32_t field, const char *utf8);
    RVDL_HOST_EXPORT int32_t gh_find_add(int32_t field, int32_t cp);
    RVDL_HOST_EXPORT int32_t gh_find_backspace(int32_t field);
    RVDL_HOST_EXPORT int32_t gh_find_clear(int32_t field);
    RVDL_HOST_EXPORT int32_t gh_find_len(int32_t field);
    RVDL_HOST_EXPORT const char *gh_find_cstr(int32_t field);
    RVDL_HOST_EXPORT int32_t gh_find_codepoint(int32_t field, int32_t index);
}


extern "C"
{
    RVDL_HOST_EXPORT int32_t gh_ed_find(const char *needle, int32_t from_cp);

    RVDL_HOST_EXPORT int32_t gh_ed_rfind(const char *needle, int32_t from_cp);

    RVDL_HOST_EXPORT int32_t gh_ed_find_count(const char *needle);

    RVDL_HOST_EXPORT int32_t gh_ed_ordinal(const char *needle, int32_t cp);








    RVDL_HOST_EXPORT int32_t gh_ed_replace_selection(const char *replacement);







    RVDL_HOST_EXPORT int32_t gh_ed_replace_all(const char *needle, const char *replacement);


    RVDL_HOST_EXPORT int32_t gh_ed_mc_clear(void);
    RVDL_HOST_EXPORT int32_t gh_ed_mc_count(void);
    RVDL_HOST_EXPORT int32_t gh_ed_mc_add(int32_t pos);
    RVDL_HOST_EXPORT int32_t gh_ed_mc_pos(int32_t i);
    RVDL_HOST_EXPORT int32_t gh_ed_mc_sel(int32_t i);






    RVDL_HOST_EXPORT int32_t gh_ed_mc_add_next(void);

    RVDL_HOST_EXPORT int32_t gh_ed_mc_type(int32_t cp);
    RVDL_HOST_EXPORT int32_t gh_ed_mc_str(const char *utf8);
    RVDL_HOST_EXPORT int32_t gh_ed_mc_backspace(void);
    RVDL_HOST_EXPORT int32_t gh_ed_mc_delete(void);
}