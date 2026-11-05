#pragma once
// ============================================================================
//  qcos/Arch.hpp —— 架构检测 + 基础类型 + 停转原语
//
//  freestanding：不包含任何标准库头（仅 <stdint.h> / <stddef.h>，二者由编译器
//  提供，不依赖 libc/libstdc++）。
// ============================================================================

// 注意：必须用 C 头 <stdint.h>/<stddef.h> 而非 C++ 包装头 <cstdint>/<cstddef>。
// 后者依赖 C++ 标准库，而 aarch64-none-elf 等 freestanding 交叉目标没有
// C++ 标准库头；前者由 clang 对所有目标内置提供。
#include <stdint.h>
#include <stddef.h>

#if defined(__x86_64__)
  #define QCOS_ARCH_X86_64 1
#elif defined(__aarch64__)
  #define QCOS_ARCH_AARCH64 1
#elif defined(__loongarch__)
  // 龙芯自主指令集架构 LoongArch。
  //   __loongarch_grlen = 64 表示 LA64（64 位通用寄存器），= 32 表示 LA32。
  #define QCOS_ARCH_LOONGARCH 1
  #if defined(__loongarch_grlen) && __loongarch_grlen == 64
    #define QCOS_ARCH_LOONGARCH64 1
  #elif defined(__loongarch_grlen) && __loongarch_grlen == 32
    #define QCOS_ARCH_LOONGARCH32 1
  #endif
#else
  #define QCOS_ARCH_UNKNOWN 1
#endif

namespace qcos
{
    using u8    = ::uint8_t;
    using u16   = ::uint16_t;
    using u32   = ::uint32_t;
    using u64   = ::uint64_t;
    using i8    = ::int8_t;
    using i16   = ::int16_t;
    using i32   = ::int32_t;
    using i64   = ::int64_t;
    using usize = ::size_t;
    using isize = ::ptrdiff_t;

    constexpr usize PAGE_SIZE  = 4096;
    constexpr usize PAGE_SHIFT = 12;

    inline bool is_power_of_two(usize v) { return v != 0 && (v & (v - 1)) == 0; }
    inline usize align_up(usize v, usize align) { return (v + align - 1) & ~(align - 1); }
    inline usize align_down(usize v, usize align) { return v & ~(align - 1); }

    /**
     * 自旋等待时的 CPU 放松原语，降低总线争用 / 节能。
     *   x86_64   用 pause（提示处理器自旋等待）
     *   aarch64  用 yield（提示挂起执行）
     *   loongarch 用 dbar 0（full memory barrier，兼作放松）
     *   未知架构退化为编译屏障。
     */
    inline void cpu_relax()
    {
#if defined(__x86_64__)
        __asm__ volatile("pause" ::: "memory");
#elif defined(__aarch64__)
        __asm__ volatile("yield" ::: "memory");
#elif defined(__loongarch__)
        __asm__ volatile("dbar 0" ::: "memory");
#else
        __asm__ volatile("" ::: "memory");
#endif
    }

    /**
     * 停转当前核心。
     * x86_64 用 hlt、aarch64 用 wfi、loongarch 用 idle 0，未知架构退化为忙等。
     */
    [[noreturn]] inline void halt()
    {
        while (true)
        {
#if defined(__x86_64__)
            __asm__ volatile("hlt" ::: "memory");
#elif defined(__aarch64__)
            __asm__ volatile("wfi" ::: "memory");
#elif defined(__loongarch__)
            __asm__ volatile("idle 0" ::: "memory");
#else
            __asm__ volatile("" ::: "memory");
#endif
        }
    }
}