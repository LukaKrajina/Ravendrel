#pragma once
// ============================================================================
//  虚拟内存：页表抽象
// ============================================================================

#include "Arch.hpp"

namespace qcos
{
    struct PageAttrs
    {
        bool present       = true;
        bool writable      = false;
        bool user          = false;
        bool no_exec       = true;    // 用户态（非特权 EL0）不可执行
        bool priv_no_exec  = true;    // 特权态（内核 EL1）不可执行
        bool write_through = false;
        bool cache_disable = false;
    };

    /**
     * 页表抽象接口。双后端实现：
     *   - VmmX86_64   ：x86_64 4 级（PML4 -> PDPT -> PD -> PT）
     *   - VmmAarch64  ：aarch64 VMSAv8-64 4KB 颗粒、48-bit VA、4 级（TTBR0）
     *
     * translate() 返回物理地址（0 表示未映射）；
     * activate() 把根页表装载到CR3 / TTBR0_EL1 并刷新 TLB。
     *
     * 说明：map/unmap/translate 是纯内存操作（改页表项），不触碰控制寄存器，
     * 因此可在宿主上安全地做逻辑测试；只有 activate() 会改 CR3/TTBR。
     */
    class IPageTable
    {
    public:
        virtual ~IPageTable() = default;
        virtual bool map(u64 vaddr, u64 paddr, PageAttrs attrs) = 0;
        virtual bool unmap(u64 vaddr) = 0;
        virtual u64  translate(u64 vaddr) const = 0;
        virtual void activate() = 0;
    };
}