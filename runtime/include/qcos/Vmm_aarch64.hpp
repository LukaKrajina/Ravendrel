#pragma once

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
        static constexpr u64 VALID = 1ull << 0;
        static constexpr u64 TABLE = 1ull << 1;
        static constexpr u64 AF = 1ull << 10;
        static constexpr u64 AP_USER = 1ull << 6;
        static constexpr u64 AP_RDONLY = 1ull << 7;
        static constexpr u64 PXN = 1ull << 53;
        static constexpr u64 UXN = 1ull << 54;
        static constexpr u64 DESC_MASK = VALID | TABLE;

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

            if (attrs.user) pte |= AP_USER;
            if (!attrs.writable) pte |= AP_RDONLY;

            if (attrs.no_exec) pte |= UXN;
            if (attrs.priv_no_exec) pte |= PXN;

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