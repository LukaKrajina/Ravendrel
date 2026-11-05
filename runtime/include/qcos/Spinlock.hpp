#pragma once
// ============================================================================
//  自旋锁族
//
//  基于编译器原子内置（__atomic_*），不依赖 <atomic>/<mutex>，freestanding 可用。
//  x86_64 下在自旋循环中插入 pause 以降低总线争用。
//
//  提供两种实现：
//   1. Spinlock        —— test-and-set 自旋锁（简单但易饥饿）
//   2. TicketSpinlock  —— 公平票据锁
//
//  TicketSpinlock 通过"取号 + 等叫号"保证 FIFO 公平性，显著缓解多核高争用下
//  的 cache-line 乒乓与饿死问题，是"驯龙系统"PMM 多核路径默认采用的锁。
// ============================================================================

#include "Arch.hpp"

namespace qcos
{
    class Spinlock
    {
    private:
        int locked_ = 0;

    public:
        void lock()
        {
            while (__atomic_exchange_n(&locked_, 1, __ATOMIC_ACQUIRE) != 0)
            {
                cpu_relax();
            }
        }

        void unlock()
        {
            __atomic_store_n(&locked_, 0, __ATOMIC_RELEASE);
        }

        bool try_lock()
        {
            return __atomic_exchange_n(&locked_, 1, __ATOMIC_ACQUIRE) == 0;
        }
    };

    /**
     * 公平票据自旋锁。
     *
     * lock()   原子自增 next_ticket_ 取到自己的号，然后自旋等待 now_serving_
     *          轮到自己；unlock() 原子自增 now_serving_ 叫下一个号。
     * try_lock() 仅当无等待者（serving == next）且 CAS 取号成功时返回 true。
     *
     * 相比 test-and-set，票据锁把"争用"转化为"排队"，等待者不反复抢占同一
     * cache line，多核扩展性更好。
     */
    class TicketSpinlock
    {
    private:
        u32 next_ticket_ = 0;
        u32 now_serving_ = 0;

    public:
        void lock()
        {
            const u32 ticket = __atomic_fetch_add(&next_ticket_, 1u, __ATOMIC_RELAXED);
            while (__atomic_load_n(&now_serving_, __ATOMIC_ACQUIRE) != ticket)
            {
                cpu_relax();
            }
        }

        void unlock()
        {
            __atomic_fetch_add(&now_serving_, 1u, __ATOMIC_RELEASE);
        }

        bool try_lock()
        {
            const u32 serving = __atomic_load_n(&now_serving_, __ATOMIC_RELAXED);
            const u32 next    = __atomic_load_n(&next_ticket_, __ATOMIC_RELAXED);
            if (serving != next) return false;

            u32 expected = next;
            return __atomic_compare_exchange_n(&next_ticket_, &expected, next + 1u,
                                               false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
        }
    };
}