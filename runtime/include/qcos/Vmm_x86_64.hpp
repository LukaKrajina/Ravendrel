#pragma once
// ============================================================================
//  x86_64 4 级页表（PML4 -> PDPT -> PD -> PT）
//
//  每级 512 项、每项 8 字节、页表页 4KB 对齐。中间级已收紧为内核态专用
//  （仅 PRESENT|WRITABLE，不设 USER 位）。
// ============================================================================

#include "Vmm.hpp"
#include "LibcSubset.hpp"

#if defined(QCOS_ARCH_X86_64)

namespace qcos
{
    class VmmX86_64 : public IPageTable
    {
    private:
        static constexpr usize ENTRIES     = 512;
        static constexpr u64   P_ADDR_MASK = 0x000FFFFFFFFFF000ull;
        static constexpr u64   PRESENT     = 1ull << 0;
        static constexpr u64   WRITABLE    = 1ull << 1;
        static constexpr u64   USER        = 1ull << 2;
        static constexpr u64   PWT         = 1ull << 3;
        static constexpr u64   PCD         = 1ull << 4;
        static constexpr u64   NX          = 1ull << 63;

        u64 root_;        // PML4 物理地址
        u64 pool_base_;   // 页表页池基址
        u64 pool_cursor_; // 池内 bump 游标

        u64 alloc_pt_page()
        {
            const u64 p = pool_cursor_;
            pool_cursor_ += PAGE_SIZE;
            memset(reinterpret_cast<void*>(p), 0, PAGE_SIZE);
            return p;
        }

        /** 取下一级表物理地址；缺失则从池中分配一个新表并回填。 */
        u64 walk_or_alloc(u64 table_phys, usize idx)
        {
            u64* table = reinterpret_cast<u64*>(table_phys);
            u64  entry = table[idx];
            if ((entry & PRESENT) == 0)
            {
                const u64 next = alloc_pt_page();
                // 中间级不设 USER：整棵页表为内核态专用。x86 页表走表时对
                // 各级 USER 取 AND，故用户态映射须在各级同时传播 USER 位。
                table[idx] = next | PRESENT | WRITABLE;
                return next;
            }
            return entry & P_ADDR_MASK;
        }

        /** 只读遍历：取下一级表物理地址；缺失返回 0。 */
        u64 walk(u64 table_phys, usize idx) const
        {
            const u64* table = reinterpret_cast<const u64*>(table_phys);
            const u64  entry = table[idx];
            if ((entry & PRESENT) == 0) return 0;
            return entry & P_ADDR_MASK;
        }

    public:
        /** pml4_phys 与页表页池都要求 4KB 对齐。 */
        VmmX86_64(u64 pml4_phys, u64 pool_base, u64 pool_size)
            : root_(pml4_phys), pool_base_(pool_base), pool_cursor_(pool_base)
        {
            (void)pool_size;
            memset(reinterpret_cast<void*>(root_), 0, PAGE_SIZE);
        }

        bool map(u64 vaddr, u64 paddr, PageAttrs attrs) override
        {
            const u64 pml4 = walk_or_alloc(root_, (vaddr >> 39) & 0x1FF);
            const u64 pdpt = walk_or_alloc(pml4,  (vaddr >> 30) & 0x1FF);
            const u64 pd   = walk_or_alloc(pdpt,  (vaddr >> 21) & 0x1FF);
            const u64 pt   = walk_or_alloc(pd,    (vaddr >> 12) & 0x1FF);

            u64 pte = paddr & P_ADDR_MASK;
            if (attrs.present)       pte |= PRESENT;
            if (attrs.writable)      pte |= WRITABLE;
            if (attrs.user)          pte |= USER;
            if (attrs.write_through) pte |= PWT;
            if (attrs.cache_disable) pte |= PCD;
            if (attrs.no_exec || attrs.priv_no_exec) pte |= NX;

            reinterpret_cast<u64*>(pt)[(vaddr >> 12) & 0x1FF] = pte;
            return true;
        }

        bool unmap(u64 vaddr) override
        {
            const u64 pml4 = walk(root_, (vaddr >> 39) & 0x1FF);
            const u64 pdpt = pml4 ? walk(pml4, (vaddr >> 30) & 0x1FF) : 0;
            const u64 pd   = pdpt ? walk(pdpt, (vaddr >> 21) & 0x1FF) : 0;
            const u64 pt   = pd   ? walk(pd,   (vaddr >> 12) & 0x1FF) : 0;
            if (!pt) return false;
            reinterpret_cast<u64*>(pt)[(vaddr >> 12) & 0x1FF] = 0;
            return true;
        }

        u64 translate(u64 vaddr) const override
        {
            const u64 pml4 = walk(root_, (vaddr >> 39) & 0x1FF);
            const u64 pdpt = pml4 ? walk(pml4, (vaddr >> 30) & 0x1FF) : 0;
            const u64 pd   = pdpt ? walk(pdpt, (vaddr >> 21) & 0x1FF) : 0;
            const u64 pt   = pd   ? walk(pd,   (vaddr >> 12) & 0x1FF) : 0;
            if (!pt) return 0;
            const u64 entry = reinterpret_cast<const u64*>(pt)[(vaddr >> 12) & 0x1FF];
            if ((entry & PRESENT) == 0) return 0;
            return (entry & P_ADDR_MASK) | (vaddr & (PAGE_SIZE - 1));
        }

        void activate() override
        {
            __asm__ volatile("mov %0, %%cr3" :: "r"(root_) : "memory");
        }
    };
}

#endif