#pragma once

#include <stdint.h>
#include <stddef.h>

#if defined(__x86_64__)
  #define QCOS_ARCH_X86_64 1
#elif defined(__aarch64__)
  #define QCOS_ARCH_AARCH64 1
#elif defined(__loongarch__)
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