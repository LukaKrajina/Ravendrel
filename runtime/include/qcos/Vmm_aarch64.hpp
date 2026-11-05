#pragma once
// ============================================================================
//  aarch64 VMSAv8-64 页表（TTBR0，4KB 颗粒，48-bit VA）
//
//  4 级：L0 -> L1 -> L2 -> L3。每级 512 项、每项 8 字节、页表页 4KB 对齐。
//  输出地址在 bits 12..47。
//
//  仅在 aarch64 目标下编译（QCOS_ARCH_AARCH64）；x86_64 宿主不会包含本文件。
// ============================================================================

#include "Vmm.hpp"
#include "LibcSubset.hpp"

#if defined(QCOS_ARCH_AARCH64)

namespace qcos
{
    class VmmAarch64 : public IPageTable
    {
    private:
        static constexpr usize ENTRIES     = 512;
        static constexpr u64   P_ADDR_MASK = 0x0000FFFFFFFFF000ull;
        static constexpr u64   VALID       = 1ull << 0;    // 描述符有效（bit0）
        static constexpr u64   TABLE       = 1ull << 1;    // 表/页（bit1）
        static constexpr u64   AF          = 1ull << 10;   // 访问标志
        static constexpr u64   AP_USER     = 1ull << 6;    // AP[1]：EL0 可访问
        static constexpr u64   AP_RDONLY   = 1ull << 7;    // AP[2]：只读
        static constexpr u64   PXN         = 1ull << 53;   // 特权（EL1）不可执行
        static constexpr u64   UXN         = 1ull << 54;   // 非特权（EL0）不可执行
        static constexpr u64   DESC_MASK   = VALID | TABLE; // 0b11

        u64 ttbr0_;
        u64 pool_base_;
        u64 pool_cursor_;

        u64 alloc_pt_page()
        {
            const u64 p = pool_cursor_;
            pool_cursor_ += PAGE_SIZE;
            memset(reinterpret_cast<void*>(p), 0, PAGE_SIZE);
            return p;
        }

        u64 walk_or_alloc(u64 table_phys, usize idx)
        {
            u64* table = reinterpret_cast<u64*>(table_phys);
            u64  entry = table[idx];
            if ((entry & DESC_MASK) != DESC_MASK)
            {
                const u64 next = alloc_pt_page();
                table[idx] = next | DESC_MASK | AF;
                return next;
            }
            return entry & P_ADDR_MASK;
        }

        u64 walk(u64 table_phys, usize idx) const
        {
            const u64* table = reinterpret_cast<const u64*>(table_phys);
            const u64  entry = table[idx];
            if ((entry & DESC_MASK) != DESC_MASK) return 0;
            return entry & P_ADDR_MASK;
        }

    public:
        VmmAarch64(u64 ttbr0, u64 pool_base, u64 pool_size)
            : ttbr0_(ttbr0), pool_base_(pool_base), pool_cursor_(pool_base)
        {
            (void)pool_size;
            memset(reinterpret_cast<void*>(ttbr0_), 0, PAGE_SIZE);
        }

        bool map(u64 vaddr, u64 paddr, PageAttrs attrs) override
        {
            const u64 l0 = walk_or_alloc(ttbr0_, (vaddr >> 39) & 0x1FF);
            const u64 l1 = walk_or_alloc(l0,     (vaddr >> 30) & 0x1FF);
            const u64 l2 = walk_or_alloc(l1,     (vaddr >> 21) & 0x1FF);
            const u64 l3 = walk_or_alloc(l2,     (vaddr >> 12) & 0x1FF);

            u64 pte = (paddr & P_ADDR_MASK) | DESC_MASK | AF;

            // 访问权限（AP[2:1]，bit7:bit6）：bit6=AP[1]=user（EL0 可访问），
            // bit7=AP[2]=readonly。ARM VMSAv8-64 stage 1 编码：
            //   00: EL1 RW（EL0 禁）   01: EL1 RW + EL0 RW
            //   10: EL1 RO（EL0 禁）   11: EL1 RW + EL0 RO
            if (attrs.user)          pte |= AP_USER;    // bit6
            if (!attrs.writable)     pte |= AP_RDONLY;  // bit7

            // 执行权限：UXN 禁止 EL0 执行、PXN 禁止 EL1 执行，二者独立。
            if (attrs.no_exec)       pte |= UXN;        // bit54
            if (attrs.priv_no_exec)  pte |= PXN;        // bit53

            reinterpret_cast<u64*>(l3)[(vaddr >> 12) & 0x1FF] = pte;
            return true;
        }

        bool unmap(u64 vaddr) override
        {
            const u64 l0 = walk(ttbr0_, (vaddr >> 39) & 0x1FF);
            const u64 l1 = l0 ? walk(l0, (vaddr >> 30) & 0x1FF) : 0;
            const u64 l2 = l1 ? walk(l1, (vaddr >> 21) & 0x1FF) : 0;
            const u64 l3 = l2 ? walk(l2, (vaddr >> 12) & 0x1FF) : 0;
            if (!l3) return false;
            reinterpret_cast<u64*>(l3)[(vaddr >> 12) & 0x1FF] = 0;
            return true;
        }

        u64 translate(u64 vaddr) const override
        {
            const u64 l0 = walk(ttbr0_, (vaddr >> 39) & 0x1FF);
            const u64 l1 = l0 ? walk(l0, (vaddr >> 30) & 0x1FF) : 0;
            const u64 l2 = l1 ? walk(l1, (vaddr >> 21) & 0x1FF) : 0;
            const u64 l3 = l2 ? walk(l2, (vaddr >> 12) & 0x1FF) : 0;
            if (!l3) return 0;
            const u64 entry = reinterpret_cast<const u64*>(l3)[(vaddr >> 12) & 0x1FF];
            if ((entry & DESC_MASK) != DESC_MASK) return 0;
            return (entry & P_ADDR_MASK) | (vaddr & (PAGE_SIZE - 1));
        }

        void activate() override
        {
            __asm__ volatile("msr ttbr0_el1, %0" :: "r"(ttbr0_) : "memory");
            __asm__ volatile("dsb ish" ::: "memory");
            __asm__ volatile("tlbi vmalle1" ::: "memory");
            __asm__ volatile("dsb ish" ::: "memory");
            __asm__ volatile("isb" ::: "memory");
        }
    };
}
#endif