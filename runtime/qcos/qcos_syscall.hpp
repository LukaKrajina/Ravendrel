#pragma once

#include "qcos/Arch.hpp"

extern "C" void* rvdl_gc_alloc(long long n);
extern "C" void  rvdl_gc_free(void* p);

namespace qcos
{
    namespace sys
    {
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

        enum StatusCode : i32
        {
            OK     = 0,
            EPERM  = -1,
            EINVAL = -2,
            ENOSYS = -3,
            ENOMEM = -4,
        };

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
            outb8(COM1 + 1, 0x00);
            outb8(COM1 + 3, 0x80);
            outb8(COM1 + 0, 0x03);
            outb8(COM1 + 1, 0x00);
            outb8(COM1 + 3, 0x03);
            outb8(COM1 + 2, 0xC7);
            outb8(COM1 + 4, 0x0B);
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

        inline char* itoa(i32 v, char* buf)
        {
            char tmp[12];
            u32 u;
            const bool neg = v < 0;
            if (neg) u = static_cast<u32>(-static_cast<i64>(v));
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

        inline void emit_log(i32 , const char* msg)
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

        inline i32 call(i32 num, i32 a0, i32 a1, i32 a2)
        {
            (void)a0; (void)a1; (void)a2;
            switch (num)
            {
            case SYS_LOG:   return OK;
            case SYS_YIELD: cpu_relax(); return OK;
            case SYS_EXIT: halt(); return OK;
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