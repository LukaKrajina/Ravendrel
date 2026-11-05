// ═══════════════════════════════════════════════════════════════════════
// Bro 代码着色与行索引（给编辑器视图用）
// ═══════════════════════════════════════════════════════════════════════
//
// 设计取舍写在 BroSyntax.cpp 顶部，要点重复一遍：
//
//   界面上只画**可见的**那几十行，但「找第 N 行的起点」在 rvdl 里
//   只能从缓冲开头扫起 —— 一个 5000 行的文件，每帧 40 行
//   × 20 万码点 = 800 万次调用，1900fps 下完全不可行。
//
//   所以行索引和词法分类都在这里做，并按 gh_str_version() 缓存。
//   rvdl 侧变成「每可见行一次 FFI」，复杂度从
//   O(文件大小 × 可见行数) 降到 O(可见行数 × 行长)。
//
// 类别码是单字节 ASCII（见 .cpp 里的表），所以返回值能当 C 字符串用，
// rvdl 侧一趟顺序遍历就能把同类的连续码点并成一段绘制。
//
// 声明在这里、实现也在同一个 DLL：这是硬性要求，不是风格选择，在这儿坦诚说下。

#pragma once
#include "RuntimeApi.h"
#include <cstdint>

#ifndef RVDL_HOST_EXPORT
#define RVDL_HOST_EXPORT extern "C" RVDL_RT_API
#endif

extern "C"
{
    // ── 失效 ────────────────────────────────────────────────────────────
    //
    // 正常情况下不需要调：缓存靠 gh_str_version() 自己判断。
    // 宿主直接改了缓冲内部状态（比如绕过 gh_str_* 写文件）时才需要。
    RVDL_HOST_EXPORT void gh_syn_invalidate(void);

    // ── 行索引（0 基）─────────────────────────────────────────────────
    RVDL_HOST_EXPORT int32_t gh_syn_line_count(void);
    /// 第 line 行的起始**字节**偏移，越界 -1
    RVDL_HOST_EXPORT int32_t gh_syn_line_begin(int32_t line);
    /// 第 line 行结束的字节偏移（不含换行符），越界 -1
    RVDL_HOST_EXPORT int32_t gh_syn_line_end(int32_t line);
    /// 第 line 行的字节数
    RVDL_HOST_EXPORT int32_t gh_syn_line_len(int32_t line);

    // ── 词法分类 ────────────────────────────────────────────────────────
    //
    // 返回该行的类别串：每字节一个 ASCII 字母，
    // 长度 = 该行字节数（不含换行符）。
    //
    //   p 普通   k 关键字   s 字符串   c 注释   n 数字
    //   t 类型   f 函数名   o 标点     r 预处理指令
    //
    // 返回的指针指向内部缓冲，下次调用会被覆盖 —— 要存就先拷走。
    RVDL_HOST_EXPORT const char *gh_syn_line(int32_t line);
    /// 单点查询：第 line 行第 col 字节的类别码。越界 0。
    RVDL_HOST_EXPORT int32_t gh_syn_class_of(int32_t line, int32_t col);

    // ── 切片 ────────────────────────────────────────────────────────────
    //
    // rvdl 造不出字符串（`"a" + "b"` 是指针加法），所以按类别分段染色时
    // 没法自己拼子串。这一组是给「一个词法片段一块颜色」用的。
    //
    /// 取第 line 行 [b0, b1) 的 UTF-8 片段。列是**字节**下标，与
    /// gh_syn_line 的类别串一一对应。越界返回空串。指针指向内部缓冲，
    /// 下一次调用会被覆盖 —— rvdl 侧拿到后要立刻用掉。
    RVDL_HOST_EXPORT const char *gh_syn_slice(int32_t line, int32_t b0, int32_t b1);

    // ── 码点 ↔ 字节 ─────────────────────────────────────────────────────
    //
    // 缓冲的公开接口按**码点**索引（gh_ed_codepoint / gh_ed_cursor），
    // 而着色按**字节**。没有这组换算，点击定位和光标绘制都做不了。

    /// 第 line 行第 byte_col 字节处，对应全文档的第几个码点
    RVDL_HOST_EXPORT int32_t gh_syn_cp_index(int32_t line, int32_t byte_col);
    /// 码点下标 → 所在行号。越界 -1
    RVDL_HOST_EXPORT int32_t gh_syn_cp_line(int32_t cp_index);
    /// 码点下标 → 行内字节列。越界 -1
    RVDL_HOST_EXPORT int32_t gh_syn_cp_col(int32_t cp_index);

    // ── 文件信息 ────────────────────────────────────────────────────────
    RVDL_HOST_EXPORT int32_t gh_syn_bytes(void);
    /// 0 = 通用，1 = 脚本（首行有 shebang）
    RVDL_HOST_EXPORT int32_t gh_syn_language(void);
}
