#pragma once

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