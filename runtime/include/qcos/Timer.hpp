#pragma once

#include "Arch.hpp"
#include "Serial.hpp"

namespace qcos
{

    struct TimerNode
    {
        TimerNode* next    = nullptr;
        u64 expires = 0;
        void     (*cb)(TimerNode* self) = nullptr;
    };

    template<u32 WHEELS = 4>
    class CascadeWheel
    {
        static constexpr u32 SLOTS     = 64;
        static constexpr u32 SLOT_BITS = 6;

    private:
        TimerNode* wheel_[WHEELS][SLOTS];
        u64        now_ = 0;

        static u32 slot_of(u64 t, u32 level)
        {
            return static_cast<u32>((t >> (level * SLOT_BITS)) & (SLOTS - 1));
        }

        void push(u32 level, u64 expires, TimerNode* node)
        {
            const u32 s = slot_of(expires, level);
            node->expires = expires;
            node->next    = wheel_[level][s];
            wheel_[level][s] = node;
        }

        void cascade(u32 level)
        {
            const u32 s = slot_of(now_, level);
            TimerNode* node = wheel_[level][s];
            wheel_[level][s] = nullptr;
            while (node)
            {
                TimerNode* next = node->next;
                const u64 remaining = node->expires - now_;
                add(node, remaining);
                node = next;
            }
        }

    public:
        CascadeWheel()
        {
            for (u32 l = 0; l < WHEELS; ++l)
                for (u32 s = 0; s < SLOTS; ++s)
                    wheel_[l][s] = nullptr;
        }

        u64 now() const { return now_; }

        void add(TimerNode* node, u64 delay)
        {
            node->next = nullptr;
            const u64 expires = now_ + delay;

            u32  level = 0;
            u64  span  = SLOTS;
            while (level + 1 < WHEELS && delay >= span)
            {
                span <<= SLOT_BITS;
                ++level;
            }
            push(level, expires, node);
        }

        u32 tick()
        {
            ++now_;

            for (u32 l = 1; l < WHEELS; ++l)
            {
                if ((now_ & ((1ull << (l * SLOT_BITS)) - 1)) == 0)
                    cascade(l);
            }

            u32 fired = 0;
            const u32 s = slot_of(now_, 0);
            TimerNode* node = wheel_[0][s];
            wheel_[0][s] = nullptr;
            while (node)
            {
                TimerNode* next = node->next;
                if (node->expires <= now_)
                {
                    if (node->cb) node->cb(node);
                    ++fired;
                }
                else
                {
                    push(0, node->expires, node);
                }
                node = next;
            }
            return fired;
        }

        void cancel(TimerNode* target)
        {
            for (u32 l = 0; l < WHEELS; ++l)
            {
                for (u32 s = 0; s < SLOTS; ++s)
                {
                    TimerNode** p = &wheel_[l][s];
                    while (*p)
                    {
                        if (*p == target)
                        {
                            *p = (*p)->next;
                            target->next = nullptr;
                            return;
                        }
                        p = &(*p)->next;
                    }
                }
            }
        }
    };

    inline void pit_init(u32 hz)
    {
        const u32 divisor = 1193182u / hz;
        outb(0x43, 0x36);
        outb(0x40, static_cast<u8>(divisor & 0xFFu));
        outb(0x40, static_cast<u8>((divisor >> 8) & 0xFFu));
    }

}