#pragma once

#include "Arch.hpp"
#include "Spinlock.hpp"

namespace qcos
{

template<u32 MAX_ORDER = 10>
class DragonPmm
{
public:
    static constexpr u32 MAX_ORDER_VALUE = MAX_ORDER;

    struct CpuCache
    {
        static constexpr u32 BATCH = 32;
        u64 pages[BATCH] = {};
        u32 count        = 0;
    };

    static constexpr usize cache_array_bytes(u32 core_count)
    {
        return static_cast<usize>(core_count) * sizeof(CpuCache);
    }

private:
    static constexpr u64 FREE_FLAG = 1ull << 63;
    static constexpr u64 NEXT_MASK = ~FREE_FLAG;

    u64 base_ = 0;
    u64 total_pages_ = 0;
    u64 free_pages_ = 0;
    u32 core_count_ = 1;
    bool is_multi_ = false;
    CpuCache* cpu_caches_ = nullptr;

    u64 free_lists_[MAX_ORDER + 1] = {};
    u64 order_map_ = 0;

    TicketSpinlock lock_;

    u64*       block_link(u64 addr)       { return reinterpret_cast<u64*>(addr); }
    const u64* block_link(u64 addr) const { return reinterpret_cast<const u64*>(addr); }

    static u32 ilog2_ceil(u64 v)
    {
        u32 r = 0;
        while ((1ull << r) < v) ++r;
        return r;
    }

    bool in_range(u64 addr) const
    {
        return addr >= base_ && addr < base_ + total_pages_ * PAGE_SIZE;
    }

    void list_push(u32 order, u64 addr)
    {
        block_link(addr)[1] = order;
        block_link(addr)[0] = free_lists_[order] | FREE_FLAG;
        free_lists_[order] = addr;
        order_map_ |= (1ull << order);
    }

    u64 list_pop(u32 order)
    {
        const u64 addr = free_lists_[order];
        const u64 next = block_link(addr)[0] & NEXT_MASK;
        block_link(addr)[0] = 0;
        free_lists_[order] = next;
        if (next == 0) order_map_ &= ~(1ull << order);
        return addr;
    }

    void list_remove(u32 order, u64 target)
    {
        u64 prev = 0;
        u64 cur  = free_lists_[order];
        while (cur != 0 && cur != target)
        {
            prev = cur;
            cur  = block_link(cur)[0] & NEXT_MASK;
        }
        if (cur != target) return;
        const u64 next = block_link(target)[0] & NEXT_MASK;
        if (prev == 0) free_lists_[order] = next;
        else           block_link(prev)[0] = next | FREE_FLAG;
        block_link(target)[0] = 0;
        if (free_lists_[order] == 0) order_map_ &= ~(1ull << order);
    }

    bool buddy_is_free(u64 addr, u32 order) const
    {
        if (!in_range(addr)) return false;
        if ((block_link(addr)[0] & FREE_FLAG) == 0) return false;
        return block_link(addr)[1] == order;
    }

    void free_pages_add(i64 delta)
    {
        __atomic_fetch_add(&free_pages_, static_cast<u64>(delta), __ATOMIC_RELAXED);
    }
    u64 free_pages_get() const
    {
        return __atomic_load_n(&free_pages_, __ATOMIC_RELAXED);
    }


    u64 pop_central(u32 order)
    {
        if ((order_map_ >> order) == 0) return 0;

        u32 o = order;
        while ((order_map_ & (1ull << o)) == 0) ++o;

        u64 addr = list_pop(o);
        while (o > order)
        {
            --o;
            list_push(o, addr + (1ull << o) * PAGE_SIZE);
        }
        return addr;
    }

    void coalesce_into_central(u64 addr, u32 order)
    {
        u64 page = (addr - base_) / PAGE_SIZE;
        block_link(addr)[1] = order;
        block_link(addr)[0] = FREE_FLAG;

        while (order < MAX_ORDER)
        {
            const u64 buddy_page = page ^ (1ull << order);
            const u64 buddy      = base_ + buddy_page * PAGE_SIZE;
            if (!buddy_is_free(buddy, order)) break;
            list_remove(order, buddy);
            page = (page < buddy_page) ? page : buddy_page;
            addr = base_ + page * PAGE_SIZE;
            ++order;
            block_link(addr)[1] = order;
            block_link(addr)[0] = FREE_FLAG;
        }

        list_push(order, addr);
    }

    u64 alloc_locked(u32 order)
    {
        const u64 addr = pop_central(order);
        if (addr) free_pages_add(-static_cast<i64>(1ull << order));
        return addr;
    }

    void free_locked(u64 addr, u32 order)
    {
        const u32 orig_order = order;
        coalesce_into_central(addr, order);
        free_pages_add(static_cast<i64>(1ull << orig_order));
    }

    u64 cache_pop(u32 cpu_id)
    {
        CpuCache& c = cpu_caches_[cpu_id];
        if (c.count == 0) return 0;
        return c.pages[--c.count];
    }

    bool cache_push(u32 cpu_id, u64 addr)
    {
        CpuCache& c = cpu_caches_[cpu_id];
        if (c.count >= CpuCache::BATCH) return false;
        c.pages[c.count++] = addr;
        return true;
    }

    void refill_cache(u32 cpu_id)
    {
        CpuCache& c = cpu_caches_[cpu_id];
        while (c.count < CpuCache::BATCH)
        {
            const u64 a = pop_central(0);
            if (a == 0) break;
            c.pages[c.count++] = a;
        }
    }

    void drain_cache(u32 cpu_id)
    {
        CpuCache& c = cpu_caches_[cpu_id];
        while (c.count > CpuCache::BATCH / 2)
        {
            coalesce_into_central(c.pages[--c.count], 0);
        }
    }

    u64 alloc_order0(u32 cpu_id)
    {
        u64 a = cache_pop(cpu_id);
        if (a != 0) { free_pages_add(-1); return a; }

        lock_.lock();
        refill_cache(cpu_id);
        lock_.unlock();

        a = cache_pop(cpu_id);
        if (a != 0) free_pages_add(-1);
        return a;
    }

    void free_order0(u64 addr, u32 cpu_id)
    {
        if (cache_push(cpu_id, addr)) { free_pages_add(1); return; }

        lock_.lock();
        drain_cache(cpu_id);
        lock_.unlock();

        cache_push(cpu_id, addr);
        free_pages_add(1);
    }

public:






    void init(u64 base, u64 size_bytes, u32 core_count = 1, CpuCache* caches = nullptr)
    {
        base_        = base;
        total_pages_ = size_bytes / PAGE_SIZE;
        free_pages_  = 0;
        core_count_  = (core_count == 0) ? 1 : core_count;
        cpu_caches_  = caches;

        if (core_count_ > 1 && cpu_caches_ == nullptr)
            core_count_ = 1;
        is_multi_ = (core_count_ > 1);

        for (u32 i = 0; i <= MAX_ORDER; ++i) free_lists_[i] = 0;
        order_map_ = 0;
        if (cpu_caches_ != nullptr)
        {
            for (u32 i = 0; i < core_count_; ++i)
            {
                for (u32 j = 0; j < CpuCache::BATCH; ++j) cpu_caches_[i].pages[j] = 0;
                cpu_caches_[i].count = 0;
            }
        }
        lock_ = TicketSpinlock{};

        u64 p = 0;
        while (p < total_pages_)
        {
            u32 order = MAX_ORDER;
            while (order > 0)
            {
                const u64 blk = 1ull << order;
                if (p % blk == 0 && p + blk <= total_pages_) break;
                --order;
            }
            list_push(order, base_ + p * PAGE_SIZE);
            free_pages_add(static_cast<i64>(1ull << order));
            p += (1ull << order);
        }
    }




    u64 alloc(u64 count = 1, u32 cpu_id = 0)
    {
        if (count == 0) return 0;
        const u32 order = ilog2_ceil(count);
        if (order > MAX_ORDER) return 0;

        if (is_multi_ && order == 0)
        {
            return alloc_order0(cpu_id < core_count_ ? cpu_id : 0);
        }

        if (is_multi_) lock_.lock();
        const u64 addr = alloc_locked(order);
        if (is_multi_) lock_.unlock();
        return addr;
    }

    void free(u64 addr, u64 count = 1, u32 cpu_id = 0)
    {
        if (!in_range(addr) || (addr & (PAGE_SIZE - 1))) return;

        u32 order = ilog2_ceil(count);
        if (order > MAX_ORDER) order = MAX_ORDER;

        if (is_multi_ && order == 0)
        {
            free_order0(addr, cpu_id < core_count_ ? cpu_id : 0);
            return;
        }

        if (is_multi_) lock_.lock();
        free_locked(addr, order);
        if (is_multi_) lock_.unlock();
    }

    u64 free_page_count()  const { return free_pages_get(); }
    u64 total_page_count() const { return total_pages_; }
    u64 base()             const { return base_; }
    u32 core_count()       const { return core_count_; }
    u32 max_order()        const { return MAX_ORDER; }
};

}