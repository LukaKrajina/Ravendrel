#pragma once

#include "Arch.hpp"

namespace qcos
{
#if defined(QCOS_ARCH_X86_64)

    inline void outb(u16 port, u8 value)
    {
        __asm__ volatile("outb %0, %1" :: "a"(value), "Nd"(port));
    }

    inline u8 inb(u16 port)
    {
        u8 result;
        __asm__ volatile("inb %1, %0" : "=a"(result) : "Nd"(port));
        return result;
    }

    class Serial
    {
    private:
        static constexpr u16 COM1 = 0x3F8;

    public:
        static void init()
        {
            outb(COM1 + 1, 0x00);
            outb(COM1 + 3, 0x80);
            outb(COM1 + 0, 0x03);
            outb(COM1 + 1, 0x00);
            outb(COM1 + 3, 0x03);
            outb(COM1 + 2, 0xC7);
            outb(COM1 + 4, 0x0B);
        }

        static bool tx_ready()
        {
            return (inb(COM1 + 5) & 0x20) != 0;
        }

        static void putc(char c)
        {
            while (!tx_ready()) { }
            outb(COM1, static_cast<u8>(c));
        }

        static void puts(const char* s)
        {
            while (s && *s) putc(*s++);
        }
    };

#endif
}