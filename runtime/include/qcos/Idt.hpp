#pragma once

#include "Arch.hpp"

#if defined(QCOS_ARCH_X86_64)

namespace qcos
{

    struct IdtEntry
    {
        u64 lo = 0;
        u64 hi = 0;
    };

    enum class GateType : u8
    {
        INTERRUPT = 0x8E,
        TRAP = 0x8F,
        USER_INT = 0xEE,
    };

    inline IdtEntry idt_gate(u64 handler, u16 selector, GateType type, u8 ist = 0)
    {
        IdtEntry e;
        e.lo = (handler & 0xFFFFull)
             | (static_cast<u64>(selector) << 16)
             | (static_cast<u64>(ist & 0x7u) << 32)
             | (static_cast<u64>(type) << 40)
             | (((handler >> 16) & 0xFFFFull) << 48);
        e.hi = handler >> 32;
        return e;
    }

    struct InterruptFrame
    {
        u64 r15;
        u64 r14;
        u64 r13;
        u64 r12;
        u64 r11;
        u64 r10;
        u64 r9;
        u64 r8;
        u64 rbp;
        u64 rdi;
        u64 rsi;
        u64 rdx;
        u64 rcx;
        u64 rbx;
        u64 rax;
        u64 vector;
        u64 err_code;
        u64 rip;
        u64 cs;
        u64 rflags;
    };

    struct Idt
    {
        static constexpr usize VECTORS = 256;

        IdtEntry entries[VECTORS]{};

        void set_gate(u8 vec, u64 handler, GateType type = GateType::INTERRUPT, u8 ist = 0)
        {
            entries[vec] = idt_gate(handler, 0x08, type, ist);
        }

        void load()
        {
            struct Idtr
            {
                u16 limit;
                u64 base;
            } __attribute__((packed)) idtr;
            idtr.limit = static_cast<u16>(sizeof(entries) - 1);
            idtr.base  = reinterpret_cast<u64>(&entries[0]);
            __asm__ volatile("lidt %0" :: "m"(idtr) : "memory");
        }
    };

}

#endif