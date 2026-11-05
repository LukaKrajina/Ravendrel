#pragma once
// ============================================================================
//  qcos/Idt.hpp —— x86_64 中断描述符表（驯龙·龙之门 DragonGate）
//
//  吸收与创新：
//   - 门描述符 bit-packing 与 examples/rvdl_idt.rvdl / rvdl_idt2.rvdl 同一 ABI，延续
//     项目「纯 RVDL 打包 IDT」的思路，这里是 freestanding C++ 版本。
//   - 两级门分派（Two-Tier Gate Dispatch）：每个向量可绑 hot gate（关中断、
//     零开销，只做 EOI + 状态置位）与 deferred gate（开中断、可抢占的软中断
//     上下文）——把「门机制」与「处理策略」解耦。该思想源自 sched_ext
//     （2023/24）的机制/策略分离，并融合 Linux bottom-half 与 arXiv:2308.02896
//     「硬件辅助快速中断」的最小化 hot-path 主张。
//
//  freestanding：仅依赖 Arch.hpp，无 libc / libstdc++。
// ============================================================================

#include "Arch.hpp"

#if defined(QCOS_ARCH_X86_64)

namespace qcos
{

    // ---- x86_64 64 位门描述符（16 字节，打包为两个 u64）------------------
    //   lo = offset[15:0] | selector<<16 | ist[2:0]<<32 | type_attr<<40
    //                      | offset[31:16]<<48
    //   hi = offset[63:32]
    struct IdtEntry
    {
        u64 lo = 0;
        u64 hi = 0;
    };

    // 门类型（P 位恒置 1）：DPL 与 type 位编码
    enum class GateType : u8
    {
        INTERRUPT = 0x8E,   // P=1 DPL=0 type=0xE：中断门（进门前清 IF）
        TRAP      = 0x8F,   // P=1 DPL=0 type=0xF：陷阱门（保留 IF）
        USER_INT  = 0xEE,   // P=1 DPL=3 type=0xE：用户态可触发
    };

    // 门描述符打包（offset=处理函数地址，selector=代码段选择子）
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

    // ---------------------------------------------------------------------------
    //  InterruptFrame —— x86_64 内核态（ring0）中断帧
    //
    //  与 runtime/qcos/interrupt.S 的 push 顺序严格一致：中断发生时 CPU push
    //  rip/cs/rflags，stub push 错误码与向量号，再 push 15 个通用寄存器。
    //  任务上下文即「任务内核栈上保存的 InterruptFrame」，切换任务 = 切换栈指针
    //  后 iretq 恢复。字段顺序 = 从 rsp（最低地址）向上。
    // ---------------------------------------------------------------------------
    struct InterruptFrame
    {
        u64 r15;      // +0x00（最后 push，最低地址）
        u64 r14;      // +0x08
        u64 r13;      // +0x10
        u64 r12;      // +0x18
        u64 r11;      // +0x20
        u64 r10;      // +0x28
        u64 r9;       // +0x30
        u64 r8;       // +0x38
        u64 rbp;      // +0x40
        u64 rdi;      // +0x48
        u64 rsi;      // +0x50
        u64 rdx;      // +0x58
        u64 rcx;      // +0x60
        u64 rbx;      // +0x68
        u64 rax;      // +0x70
        u64 vector;   // +0x78（中断向量号）
        u64 err_code; // +0x80（错误码，无则为 0）
        u64 rip;      // +0x88（CPU push）
        u64 cs;       // +0x90
        u64 rflags;   // +0x98
    };

    // ---- 中断描述符表（256 项 × 16 字节 = 4 KB）----
    struct Idt
    {
        static constexpr usize VECTORS = 256;

        IdtEntry entries[VECTORS]{};

        // selector 0x08 = 内核 64 位代码段（与 boot_x86_64.S 的 GDT 一致）
        void set_gate(u8 vec, u64 handler, GateType type = GateType::INTERRUPT, u8 ist = 0)
        {
            entries[vec] = idt_gate(handler, 0x08, type, ist);
        }

        // 加载 IDTR（lidt）
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