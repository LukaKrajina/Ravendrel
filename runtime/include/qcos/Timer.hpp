#pragma once
// ============================================================================
//  qcos/Timer.hpp —— 定时器子系统（驯龙·龙之轮 DragonWheel）
//
//  吸收与创新：
//   - 「惰性级联时间轮」（Lazy Cascade Wheel）：4 级 × 64 槽的分级时间轮，
//     侵入式定时器节点（延续 DragonPmm 的侵入式风格）。只有轮盘指针回绕时才
//     把上一级槽位的定时器级联（cascade）到下一级，摊销每次 tick 的搬移成本，
//     使 add / tick 均摊 O(1)。这是 hierarchical timing wheel（Varghese & Lauck）
//     的惰性变体，吸收了 DPDK htimer（2023 RFC）「数十万并发定时器、降低
//     add/cancel 开销」的目标与 lock-free 单写者（tick 在中断上下文串行）设计。
//   - PIT 8253 周期 tick 源：中断向量 0x20（IRQ0）驱动时间轮推进。
//
//  freestanding：仅依赖 Arch.hpp / Serial.hpp（端口 I/O），无 libc/libstdc++。
// ============================================================================

#include "Arch.hpp"
#include "Serial.hpp"

namespace qcos
{

    // ---- 定时器节点（侵入式，供 CascadeWheel 使用）----
    struct TimerNode
    {
        TimerNode* next    = nullptr;
        u64        expires = 0;                 // 绝对到期 tick
        void     (*cb)(TimerNode* self) = nullptr;
    };

    // ---------------------------------------------------------------------------
    //  CascadeWheel —— 惰性级联时间轮
    //    WHEELS 级 × 64 槽；级 l 的槽宽为 64^l 个 tick。
    //    add 根据到期延迟把节点放入合适层级；tick 只扫级 0 当前槽并惰性级联。
    // ---------------------------------------------------------------------------
    template<u32 WHEELS = 4>
    class CascadeWheel
    {
        static constexpr u32 SLOTS     = 64;
        static constexpr u32 SLOT_BITS = 6;     // log2(64)

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

        // 把 level 级当前槽的定时器按剩余时间重新下沉到更低级
        void cascade(u32 level)
        {
            const u32 s = slot_of(now_, level);
            TimerNode* node = wheel_[level][s];
            wheel_[level][s] = nullptr;
            while (node)
            {
                TimerNode* next = node->next;
                const u64 remaining = node->expires - now_;
                add(node, remaining);          // 重新插入（剩余时间不变）
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

        // 在 now+delay 时刻触发 node（delay 为 tick 数）
        void add(TimerNode* node, u64 delay)
        {
            node->next = nullptr;
            const u64 expires = now_ + delay;

            // 按延迟大小选层级：delay < 64 入级 0，否则逐级上移
            u32  level = 0;
            u64  span  = SLOTS;
            while (level + 1 < WHEELS && delay >= span)
            {
                span <<= SLOT_BITS;            // span *= 64
                ++level;
            }
            push(level, expires, node);
        }

        // 推进一个 tick，触发到期的定时器；返回本次触发的回调数
        u32 tick()
        {
            ++now_;

            // 惰性级联：now_ 为 64^level 的倍数时，把 level 级当前槽下沉
            for (u32 l = 1; l < WHEELS; ++l)
            {
                if ((now_ & ((1ull << (l * SLOT_BITS)) - 1)) == 0)
                    cascade(l);
            }

            // 扫级 0 当前槽
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
                    push(0, node->expires, node);   // 理论不会发生，保险链回
                }
                node = next;
            }
            return fired;
        }

        // 从轮盘移除 target（O(WHEELS×SLOTS) 最坏，内核中 cancel 不频繁）
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

    // ---- PIT 8253：周期 tick 源（channel 0，方波）----
    //   控制字 0x36 = channel0 | lobyte/hibyte | mode3(方波) | binary。
    inline void pit_init(u32 hz)
    {
        const u32 divisor = 1193182u / hz;
        outb(0x43, 0x36);
        outb(0x40, static_cast<u8>(divisor & 0xFFu));
        outb(0x40, static_cast<u8>((divisor >> 8) & 0xFFu));
    }

} // namespace qcos
