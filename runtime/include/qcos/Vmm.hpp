#pragma once

#include "Arch.hpp"

namespace qcos
{
    struct PageAttrs
    {
        bool present       = true;
        bool writable      = false;
        bool user          = false;
        bool no_exec = true;
        bool priv_no_exec = true;
        bool write_through = false;
        bool cache_disable = false;
    };











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