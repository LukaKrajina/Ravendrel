#pragma once

#include "Arch.hpp"
#include "Serial.hpp"

#if defined(QCOS_ARCH_X86_64)

namespace qcos
{

    constexpr u16 PIC1_CMD  = 0x20;
    constexpr u16 PIC1_DATA = 0x21;
    constexpr u16 PIC2_CMD  = 0xA0;
    constexpr u16 PIC2_DATA = 0xA1;

    constexpr u8 IRQ0_VECTOR = 0x20;
    constexpr u8 IRQ8_VECTOR = 0x28;
    constexpr u8  IRQ_COUNT   = 16;

    using HotHandler = void (*)(u8 irq);
    using DeferredHandler = void (*)(u8 irq);

    struct IrqGate
    {
        HotHandler      hot      = nullptr;
        DeferredHandler deferred = nullptr;
    };

    inline void pic_remap()
    {
        outb(PIC1_CMD, 0x11);
        outb(PIC1_DATA, IRQ0_VECTOR);
        outb(PIC1_DATA, 0x04);
        outb(PIC1_DATA, 0x01);
        outb(PIC2_CMD, 0x11);
        outb(PIC2_DATA, IRQ8_VECTOR);
        outb(PIC2_DATA, 0x02);
        outb(PIC2_DATA, 0x01);
    }

    inline void pic_eoi(u8 irq)
    {
        if (irq >= 8) outb(PIC2_CMD, 0x20);
        outb(PIC1_CMD, 0x20);
    }

    inline void pic_set_mask(u16 mask)
    {
        outb(PIC1_DATA, static_cast<u8>(mask & 0xFFu));
        outb(PIC2_DATA, static_cast<u8>((mask >> 8) & 0xFFu));
    }

    struct IrqDispatcher
    {
        IrqGate gates[IRQ_COUNT]{};

        void bind(u8 irq, HotHandler hot, DeferredHandler deferred = nullptr)
        {
            if (irq < IRQ_COUNT)
                gates[irq] = IrqGate{ hot, deferred };
        }

        void dispatch(u8 irq)
        {
            if (irq >= IRQ_COUNT) return;
            if (gates[irq].hot) gates[irq].hot(irq);
            pic_eoi(irq);
        }

        void run_deferred(u8 irq)
        {
            if (irq < IRQ_COUNT && gates[irq].deferred) gates[irq].deferred(irq);
        }
    };

}

#endif