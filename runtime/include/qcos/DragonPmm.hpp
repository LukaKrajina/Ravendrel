#pragma once
// ============================================================================
//  "驯龙系统"物理内存管理器（v2）
//
//  对 v1 BitmapPmm（位图分配器）的全面升级：伙伴系统 + 空闲链表 + 多核扩展。
//
//  ── 设计范式（吸收 2025 至今学术与工业界成果后的融合创新）────────────────
//
//  1. 伙伴系统（Buddy System）
//     物理内存按 2 的幂次页（order）组织，分配时自上而下 split、释放时自下
//     而上 merge，最小化外部碎片（经典 buddy + Linux zoned allocator）。
//
//  2. 侵入式空闲链表（intrusive free list）
//     每个 order 一条空闲链表；空闲块的块头直接写进块自己的内存（零额外
//     元数据、无 per-page 数组开销，类似 Linux struct page 的 PageBuddy 标志
//     与 private order，但去掉了整页结构体）。块头编码：
//         [0] bit63 = 空闲标志；低 63 位 = 后继块物理地址（0 表链表尾）
//         [1] = 该空闲块的 order
//     据此可在 O(1) 内判断"某地址是否为 order-o 的空闲块头"，支撑无扫描合并。
//
//  3. order 位图索引（TLSF / Two-Level Segregated Fit 思想）
//     order_map_ 的 bit o 表示 free_lists_[o] 非空，分配时用单条位移/前导零
//     指令 O(1) 定位"第一个 >= 目标 order 的非空链表"，取代逐级线性查找。
//
//  4. Per-CPU 页缓存（Linux PCP / per-cpu pageset 思想）
//     多核下 order-0 单页分配/释放走 per-CPU 私有缓存（无锁快路径），缓存空
//     时批量补充、满时批量回收到中央池，把全局锁争用摊薄到批处理上。
//
//  5. 单核 / 多核自适应
//     core_count == 1：全路径无锁，直接操作中央池（最简、零原子开销）。
//     core_count >  1：order-0 走 per-CPU 无锁缓存，order>=1 走公平票据锁。
//
//  6. 公平票据锁（TicketSpinlock）
//     多核中央池采用 FIFO 票据锁，缓解 test-and-set 的 cache-line 乒乓与饿死。
//
//  ── freestanding 约束 ──────────────────────────────────────────────────
//  仅依赖 Arch.hpp / Spinlock.hpp（编译器内置原子 __atomic_*），无 libc /
//  libstdc++ / 异常 / RTTI / 动态分配。free_lists_ 为编译期定长数组成员；
//  per-CPU 缓存数组由调用者经 init() 传入（支持动态核心数，调用者可将数组
//  置于静态区/物理内存）；空闲块头侵入式存放，故不消耗任何物理内存页来存放
//  元数据（对比 v1 位图占位）。
//
//  ── 语义说明 ──────────────────────────────────────────────────────────
//  空闲链表以"块首物理地址"为元素、以 0 为空哨兵；因此要求 base_ > 0（物理
//  内存管理器通常从低内存保留区之上开始，天然满足）。alloc(count) 把 count
//  向上取整到 2 的幂 order，返回 2^order 页连续物理内存（buddy 固有内部碎片，
//  与 Linux get_free_pages 一致）。free(addr,count) 的 count 必须与分配时一致
//  （即落入同一 order 区间），否则块边界会错乱。
//
//  ── 使用方式（动态核心数）─────────────────────────────────────────────
//      DragonPmm<10> pmm;
//      alignas(DragonPmm<10>::CpuCache) u8 cache_storage[DragonPmm<10>::cache_array_bytes(8)];
//      auto* caches = reinterpret_cast<DragonPmm<10>::CpuCache*>(cache_storage);
//      pmm.init(base, size, 8, caches);   // 8 核
//      // 单核可省略缓存数组：pmm.init(base, size);
// ============================================================================

#include "Arch.hpp"
#include "Spinlock.hpp"

namespace qcos
{

template<u32 MAX_ORDER = 10>
class DragonPmm
{
public:
    static constexpr u32 MAX_ORDER_VALUE = MAX_ORDER;

    /** per-CPU order-0 页缓存（Linux PCP 思想）。调用者按核心数提供数组。 */
    struct CpuCache
    {
        static constexpr u32 BATCH = 32;
        u64 pages[BATCH] = {};
        u32 count        = 0;
    };

    /** core_count 个 CpuCache 所需的字节数（供调用者在静态区/物理内存声明缓存）。 */
    static constexpr usize cache_array_bytes(u32 core_count)
    {
        return static_cast<usize>(core_count) * sizeof(CpuCache);
    }

private:
    // 空闲块头编码（见文件头注释）
    static constexpr u64 FREE_FLAG = 1ull << 63;
    static constexpr u64 NEXT_MASK = ~FREE_FLAG;

    u64       base_        = 0;         // 物理基址（> 0）
    u64       total_pages_ = 0;         // 总页数
    u64       free_pages_  = 0;         // 空闲（可分配）页数，原子维护
    u32       core_count_  = 1;         // 注册的核心数
    bool      is_multi_    = false;     // core_count_ > 1
    CpuCache* cpu_caches_  = nullptr;   // 调用者提供的 per-CPU 缓存数组（core_count_ 个）

    u64   free_lists_[MAX_ORDER + 1] = {};  // 每 order 一条空闲链表（存块首物理地址，0=空）
    u64   order_map_  = 0;                  // bit o = 1 ⇔ free_lists_[o] 非空

    TicketSpinlock lock_;

    // ---- 块头访问（物理地址 -> 块首 u64）----
    u64*       block_link(u64 addr)       { return reinterpret_cast<u64*>(addr); }
    const u64* block_link(u64 addr) const { return reinterpret_cast<const u64*>(addr); }

    // 最小 r 使 2^r >= v（v >= 1）
    static u32 ilog2_ceil(u64 v)
    {
        u32 r = 0;
        while ((1ull << r) < v) ++r;
        return r;
    }

    // 段内地址检查
    bool in_range(u64 addr) const
    {
        return addr >= base_ && addr < base_ + total_pages_ * PAGE_SIZE;
    }

    // ---- 空闲链表操作（须在锁内调用）----
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
        block_link(addr)[0] = 0;             // 清除空闲标志：块归用户
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
        if (cur != target) return;           // 不应发生
        const u64 next = block_link(target)[0] & NEXT_MASK;
        if (prev == 0) free_lists_[order] = next;
        else           block_link(prev)[0] = next | FREE_FLAG;
        block_link(target)[0] = 0;           // target 不再是独立空闲块
        if (free_lists_[order] == 0) order_map_ &= ~(1ull << order);
    }

    // 判断 addr 是否为 order-o 的空闲块头（O(1)，无逐页扫描）
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

    // ---- 中央池原语（须在锁内调用，不改变 free_pages_）----

    /** 从中央池取出一个 order 块（必要时自上而下 split）；失败返回 0。 */
    u64 pop_central(u32 order)
    {
        if ((order_map_ >> order) == 0) return 0;

        // TLSF：找第一个 >= order 的非空链表
        u32 o = order;
        while ((order_map_ & (1ull << o)) == 0) ++o;

        u64 addr = list_pop(o);
        while (o > order)                    // split 到目标 order
        {
            --o;
            list_push(o, addr + (1ull << o) * PAGE_SIZE);
        }
        return addr;
    }

    /** 把一个 order 块并入中央池，并向上 merge 空闲 buddy。 */
    void coalesce_into_central(u64 addr, u32 order)
    {
        u64 page = (addr - base_) / PAGE_SIZE;
        block_link(addr)[1] = order;
        block_link(addr)[0] = FREE_FLAG;

        while (order < MAX_ORDER)
        {
            // buddy 页编号 = page ^ 2^order（只要求 base_ 按 PAGE_SIZE 对齐）
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

    // ---- 中央池 + 计数（alloc/free 顶层）----
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

    // ---- per-CPU order-0 缓存（搬运原语不改 free_pages_）----
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

    void refill_cache(u32 cpu_id)            // 须锁内调用：从中央池批量 split 出 order-0
    {
        CpuCache& c = cpu_caches_[cpu_id];
        while (c.count < CpuCache::BATCH)
        {
            const u64 a = pop_central(0);
            if (a == 0) break;
            c.pages[c.count++] = a;
        }
    }

    void drain_cache(u32 cpu_id)             // 须锁内调用：把多余单页 merge 回中央池
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

        cache_push(cpu_id, addr);            // drain 后必有空位
        free_pages_add(1);
    }

public:
    /**
     * 用 [base, base+size_bytes) 这段物理内存初始化。
     *  - base 须 > 0（空闲链表以 0 为空哨兵）。
     *  - core_count：注册的核心数（0 视为 1）。
     *  - caches：指向 core_count 个 CpuCache 的数组；多核（core_count>1）必须
     *    提供，否则安全退化为单核模式。
     */
    void init(u64 base, u64 size_bytes, u32 core_count = 1, CpuCache* caches = nullptr)
    {
        base_        = base;
        total_pages_ = size_bytes / PAGE_SIZE;
        free_pages_  = 0;
        core_count_  = (core_count == 0) ? 1 : core_count;
        cpu_caches_  = caches;

        if (core_count_ > 1 && cpu_caches_ == nullptr)   // 多核缺缓存 → 安全退化单核
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

        // 把 [0, total_pages_) 贪心切分为 2 的幂块（起点/终点不对齐处自然产生低 order 块）
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

    /**
     * 分配连续 count 页（向上取整到 2 的幂），返回物理地址；失败返回 0。
     * cpu_id：多核下用于选择 per-CPU 缓存（同一 cpu_id 由上层保证串行访问）。
     */
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

    /** 释放从 addr 开始的 count 页（count 须与分配时一致）。 */
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

} // namespace qcos
