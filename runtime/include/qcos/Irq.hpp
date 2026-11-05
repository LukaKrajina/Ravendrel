#pragma once
// ============================================================================
//  qcos/Irq.hpp —— 8259 PIC + IRQ 两级门分派（驯龙·龙之门分派 DragonGate）
//
//  吸收与创新：
//   - PIC 8259 重映射：IRQ0-7 → 0x20-0x27、IRQ8-15 → 0x28-0x2F（标准做法）。
//   - 两级门分派（Two-Tier Gate Dispatch）：每个 IRQ 绑定 hot gate（关中断、
//     硬中断上下文，只做 EOI + 关键置位，零开销）与 deferred gate（开中断、
//     可抢占的软中断上下文）——把「中断机制」与「处理策略」解耦，源自
//     sched_ext（2023/24）的机制/策略分离，并吸收 arXiv:2308.02896 最小化
//     hot-path 的主张。deferred gate 由调度器在 idle / 软中断上下文调用。
//
//  freestanding：仅依赖 Arch.hpp / Serial.hpp。
// ============================================================================

#include "Arch.hpp"
#include "Serial.hpp"

#if defined(QCOS_ARCH_X86_64)

namespace qcos
{

    // ---- PIC 端口与向量 ----
    constexpr u16 PIC1_CMD  = 0x20;
    constexpr u16 PIC1_DATA = 0x21;
    constexpr u16 PIC2_CMD  = 0xA0;
    constexpr u16 PIC2_DATA = 0xA1;

    constexpr u8  IRQ0_VECTOR = 0x20;      // IRQ 0-7  -> 0x20-0x27
    constexpr u8  IRQ8_VECTOR = 0x28;      // IRQ 8-15 -> 0x28-0x2F
    constexpr u8  IRQ_COUNT   = 16;

    // ---- 两级门处理器 ----
    using HotHandler      = void (*)(u8 irq);      // 硬中断上下文（关中断）
    using DeferredHandler = void (*)(u8 irq);      // 软中断上下文（开中断、可抢占）

    struct IrqGate
    {
        HotHandler      hot      = nullptr;
        DeferredHandler deferred = nullptr;
    };

    // ---- PIC 8259 初始化：级联重映射 + 掩码 ----
    inline void pic_remap()
    {
        outb(PIC1_CMD, 0x11);                    // ICW1：初始化 + 需要 ICW4
        outb(PIC1_DATA, IRQ0_VECTOR);            // ICW2：主片向量基址
        outb(PIC1_DATA, 0x04);                   // ICW3：主片 IRQ2 级联从片
        outb(PIC1_DATA, 0x01);                   // ICW4：8086 模式
        outb(PIC2_CMD, 0x11);
        outb(PIC2_DATA, IRQ8_VECTOR);            // ICW2：从片向量基址
        outb(PIC2_DATA, 0x02);                   // ICW3：从片接主片 IRQ2
        outb(PIC2_DATA, 0x01);                   // ICW4：8086 模式
    }

    // 发送 EOI（bit i = 1 表示屏蔽 IRQ i）
    inline void pic_eoi(u8 irq)
    {
        if (irq >= 8) outb(PIC2_CMD, 0x20);
        outb(PIC1_CMD, 0x20);
    }

    // 设置中断屏蔽字
    inline void pic_set_mask(u16 mask)
    {
        outb(PIC1_DATA, static_cast<u8>(mask & 0xFFu));
        outb(PIC2_DATA, static_cast<u8>((mask >> 8) & 0xFFu));
    }

    // ---------------------------------------------------------------------------
    //  IrqDispatcher —— 两级门分派表
    //    dispatch()     由中断 stub 在硬中断上下文调用（hot path + EOI）
    //    run_deferred() 由调度器 / idle 在软中断上下文调用（可抢占）
    // ---------------------------------------------------------------------------
    struct IrqDispatcher
    {
        IrqGate gates[IRQ_COUNT]{};

        void bind(u8 irq, HotHandler hot, DeferredHandler deferred = nullptr)
        {
            if (irq < IRQ_COUNT)
                gates[irq] = IrqGate{ hot, deferred };
        }

        // 硬中断 hot path：执行 hot gate 并 EOI
        void dispatch(u8 irq)
        {
            if (irq >= IRQ_COUNT) return;
            if (gates[irq].hot) gates[irq].hot(irq);
            pic_eoi(irq);
        }

        // 软中断 deferred path：执行 deferred gate（开中断、可抢占）
        void run_deferred(u8 irq)
        {
            if (irq < IRQ_COUNT && gates[irq].deferred) gates[irq].deferred(irq);
        }
    };

} // namespace qcos

#endif // QCOS_ARCH_X86_64
