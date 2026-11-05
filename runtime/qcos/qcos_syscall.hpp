#pragma once
// ============================================================================
//  QCOS 系统调用层（freestanding，内核侧）
//
//  与 runtime/include/qhal/QcosSyscall.hpp（hosted）对应的裸机实现。rvdl 语言
//  更新后，IR 生成器（server/src/ir.ts）声明了完整的 QCOS syscall ABI：
//      rvdl_sys_call / rvdl_sys_calld / rvdl_sys_log / rvdl_sys_logi / rvdl_sys_callp
//      rvdl_gc_alloc / rvdl_gc_free
//  头文件提供跨架构（x86_64 / i386）的 freestanding 辅助实现，
//  供runtime/qcos/rvdl_shim.cpp 与 rvdl_shim32.cpp 以 extern "C" 符号对外导出。
//
//  它仅依赖 qcos/Arch.hpp（编译器内置头 <stdint.h>），无 libc / libstdc++。
//  串口是唯一输出通道，SYS_TIME 用 x86 TSC 单调时钟，SYS_EXIT 停转当前核心。
// ============================================================================

#include "qcos/Arch.hpp"

// 前置声明：由 rvdl_shim.cpp / rvdl_shim32.cpp 的 bump 分配器提供定义。
// SYS_MAP / SYS_UNMAP 经 rvdl_sys_callp 转调这两个符号。
extern "C" void* rvdl_gc_alloc(long long n);
extern "C" void  rvdl_gc_free(void* p);

namespace qcos
{
    namespace sys
    {
        // ---- 系统调用号（与 runtime/include/qhal/QcosSyscall.hpp 保持一致）----
        enum SyscallNumber : i32
        {
            SYS_LOG          = 1,
            SYS_YIELD        = 2,
            SYS_EXIT         = 3,
            SYS_MAP          = 4,
            SYS_UNMAP        = 5,
            SYS_IRQ_REGISTER = 6,
            SYS_IRQ_ACK      = 7,
            SYS_QPU_SUBMIT   = 8,
            SYS_QPU_AWAIT    = 9,
            SYS_TIME         = 10,
            SYS_PANIC        = 11,
        };

        // ---- 错误码（与 hosted 一致）----
        enum StatusCode : i32
        {
            OK     = 0,
            EPERM  = -1,
            EINVAL = -2,
            ENOSYS = -3,
            ENOMEM = -4,
        };

        // ---- x86 端口 I/O（32/64 位通用；qcos/Serial.hpp 仅覆盖 x86_64）----
    #if defined(__x86_64__) || defined(__i386__)
        inline void outb8(u16 port, u8 value)
        {
            __asm__ volatile("outb %0, %1" :: "a"(value), "Nd"(port));
        }

        inline u8 inb8(u16 port)
        {
            u8 result;
            __asm__ volatile("inb %1, %0" : "=a"(result) : "Nd"(port));
            return result;
        }

        constexpr u16 COM1 = 0x3F8;

        inline void com1_init()
        {
            outb8(COM1 + 1, 0x00);   // 关闭中断
            outb8(COM1 + 3, 0x80);   // 使能 DLAB
            outb8(COM1 + 0, 0x03);   // 波特率 38400 低字节
            outb8(COM1 + 1, 0x00);   // 高字节
            outb8(COM1 + 3, 0x03);   // 8N1
            outb8(COM1 + 2, 0xC7);   // 使能 FIFO，清空
            outb8(COM1 + 4, 0x0B);   // IRQ 使能，RTS/DSR
        }

        inline bool com1_tx_ready()
        {
            return (inb8(COM1 + 5) & 0x20) != 0;
        }

        inline void com1_putc(char c)
        {
            while (!com1_tx_ready()) { cpu_relax(); }
            outb8(COM1, static_cast<u8>(c));
        }

        inline void com1_puts(const char* s)
        {
            while (s && *s) com1_putc(*s++);
        }
    #else
        inline void com1_init() {}
    #endif

        // ---- 单调时钟：x86 时间戳计数器（TSC）----
        inline u64 monotonic_ticks()
        {
    #if defined(__x86_64__) || defined(__i386__)
            u32 lo = 0;
            u32 hi = 0;
            __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
            return (static_cast<u64>(hi) << 32) | lo;
    #else
            return 0;
    #endif
        }

        // ---- itoa：32 位有符号整数转十进制字符串（freestanding）----
        // buf 至少 12 字节（含结尾 '\0'）。返回 buf。
        inline char* itoa(i32 v, char* buf)
        {
            char tmp[12];
            u32 u;
            const bool neg = v < 0;
            if (neg) u = static_cast<u32>(-static_cast<i64>(v));   // 避免 INT32_MIN 取负溢出
            else     u = static_cast<u32>(v);

            int i = 0;
            do
            {
                tmp[i++] = static_cast<char>('0' + (u % 10u));
                u /= 10u;
            } while (u != 0u);

            int j = 0;
            if (neg) buf[j++] = '-';
            while (i > 0) buf[j++] = tmp[--i];
            buf[j] = '\0';
            return buf;
        }

        // ---- 日志 ----
        inline void emit_log(i32 /*level*/, const char* msg)
        {
    #if defined(__x86_64__) || defined(__i386__)
            com1_puts(msg ? msg : "");
            com1_puts("\n");
    #else
            (void)msg;
    #endif
        }

        inline i32 logi(i32 level, i32 value)
        {
            char buf[12];
            emit_log(level, itoa(value, buf));
            return OK;
        }

        // ---- syscall 分发 ----
        inline i32 call(i32 num, i32 a0, i32 a1, i32 a2)
        {
            (void)a0; (void)a1; (void)a2;
            switch (num)
            {
            case SYS_LOG:   return OK;
            case SYS_YIELD: cpu_relax(); return OK;
            case SYS_EXIT:  halt(); return OK;    // halt() 为 [[noreturn]]
            case SYS_TIME:  return static_cast<i32>(monotonic_ticks() & 0x7FFFFFFFull);
            default:        return ENOSYS;
            }
        }

        inline double calld(i32 num, double a0, double a1)
        {
            (void)a0; (void)a1;
            switch (num)
            {
            case SYS_TIME: return static_cast<double>(monotonic_ticks());
            default:       return static_cast<double>(ENOSYS);
            }
        }

        inline void* callp(i32 num, i64 a0, i64 a1, i64 a2)
        {
            (void)a1; (void)a2;
            switch (num)
            {
            case SYS_MAP:
                if (a0 <= 0) return nullptr;
                return rvdl_gc_alloc(static_cast<long long>(a0));
            case SYS_UNMAP:
                if (a0 != 0) rvdl_gc_free(reinterpret_cast<void*>(static_cast<u64>(a0)));
                return nullptr;
            default:
                return nullptr;
            }
        }
    }
}