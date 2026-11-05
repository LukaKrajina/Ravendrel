#pragma once
// ============================================================================
//  qcos/Serial.hpp —— COM1 串口驱动（x86_64，端口 I/O）
//
//  内核最朴素的调试输出通道：无需 VGA/帧缓冲，QEMU 用 `-serial mon:stdio`
//  即可把串口输出转发到终端。
// ============================================================================

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
            outb(COM1 + 1, 0x00);   // 关闭中断
            outb(COM1 + 3, 0x80);   // 使能 DLAB
            outb(COM1 + 0, 0x03);   // 波特率 38400 低字节
            outb(COM1 + 1, 0x00);   // 高字节
            outb(COM1 + 3, 0x03);   // 8N1
            outb(COM1 + 2, 0xC7);   // 使能 FIFO，清空，14 字节阈值
            outb(COM1 + 4, 0x0B);   // IRQ 使能，RTS/DSR 置位
        }

        static bool tx_ready()
        {
            return (inb(COM1 + 5) & 0x20) != 0;   // LSR bit5 = THR 空
        }

        static void putc(char c)
        {
            while (!tx_ready()) { /* 忙等 */ }
            outb(COM1, static_cast<u8>(c));
        }

        static void puts(const char* s)
        {
            while (s && *s) putc(*s++);
        }
    };

#endif
}