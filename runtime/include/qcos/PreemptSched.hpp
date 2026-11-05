#pragma once
// ============================================================================
//  qcos/PreemptSched.hpp —— 抢占式调度器（驯龙·龙之调度 DragonSched）
//
//  吸收与创新：纠缠虚拟截止时间优先（E²VDF，Entangled EEVDF）
//   - EEVDF 内核（Linux 6.6，2023）：每个任务持有虚拟运行时间 vtime、权重
//     weight、时间片 slice，虚拟截止时间 vdeadline = vtime + slice/weight；
//     调度器始终选 vdeadline 最小（最早截止）的就绪任务，兼顾公平与延迟上界。
//   - 纠缠预算（量子启发，延续项目 entangle 范式 + QSRA 2024 量子资源关联）：
//     entangle(a, b) 让 a、b 共享「虚拟时间命运」——a 运行推进虚拟时间时，其
//     纠缠伙伴 b 的 vtime / vdeadline 同步推进。
//   - 可插拔策略（sched_ext 2023/24 机制/策略分离）：调度决策收敛于
//     pick_next()/advance_vtime() 两个原语。
//
//  上下文切换采用「中断帧 + iretq」方案：线程上下文 = 线程内核栈上保存的
//  InterruptFrame（见 Idt.hpp）。线程首次运行由 interrupt.S 的 start_scheduler
//  经 iretq 进入；抢占由中断 stub 保存/切换栈指针后 iretq 完成。本头文件只
//  负责 E²VDF 调度决策，不涉及汇编。
//
//  freestanding：仅依赖 Arch.hpp / Idt.hpp / LibcSubset.hpp。
// ============================================================================

#include "Arch.hpp"
#include "Idt.hpp"
#include "LibcSubset.hpp"

#if defined(QCOS_ARCH_X86_64)

namespace qcos
{

    // 虚拟时间定点精度：避免 slice/weight 整数截断导致虚拟截止时间不推进
    static constexpr u64 VTIME_SCALE = 1024;

    enum class ThreadState : u32
    {
        RUNNABLE,
        RUNNING,
        BLOCKED,
        DEAD,
    };

    // ---- 线程控制块（含 EEVDF 状态与纠缠链）----
    struct Thread
    {
        u64 kernel_rsp = 0;                 // 内核栈指针（指向保存的 InterruptFrame）

        // EEVDF 字段
        u64 vtime     = 0;                  // 虚拟运行时间
        u64 weight    = 100;                // 权重（越大分得越多 CPU）
        u64 slice     = 10;                 // 时间片（tick）
        u64 vdeadline = 0;                  // 虚拟截止时间

        ThreadState state    = ThreadState::RUNNABLE;
        Thread*     entangle = nullptr;     // 纠缠伙伴（共享虚拟时间命运）

        void (*entry)(void*) = nullptr;     // 线程入口
        void*  arg           = nullptr;
        u32    id            = 0;
    };

    // 初始化线程：在栈顶布置中断帧，iretq 进入时 rip=entry、rdi=arg、开中断。
    inline void thread_init(Thread* t, void (*entry)(void*), void* arg, u8* stack_top)
    {
        InterruptFrame* frame = reinterpret_cast<InterruptFrame*>(
            reinterpret_cast<u64>(stack_top) - sizeof(InterruptFrame));
        memset(frame, 0, sizeof(InterruptFrame));
        frame->rip    = reinterpret_cast<u64>(entry);
        frame->cs     = 0x08;               // 内核 64 位代码段
        frame->rflags = 0x202;              // IF 置位（开中断）
        frame->rdi    = reinterpret_cast<u64>(arg);

        t->kernel_rsp = reinterpret_cast<u64>(frame);
        t->entry = entry;
        t->arg   = arg;
        t->slice  = t->slice ? t->slice : 1;
        t->weight = t->weight ? t->weight : 1;
        t->vdeadline = (t->slice * VTIME_SCALE) / t->weight;
    }

    // ---------------------------------------------------------------------------
    //  DragonSched —— E²VDF 抢占式调度器
    //    tick()   时间片递减；耗尽则推进虚拟时间并重选（返回 true 表示发生抢占）
    //    yield()  主动让出 CPU
    //    entangle 纠缠两个线程，使其共享虚拟时间命运
    // ---------------------------------------------------------------------------
    class DragonSched
    {
        static constexpr usize MAX_THREADS = 64;

    private:
        Thread* threads_[MAX_THREADS]{};
        usize   count_     = 0;
        Thread* current_   = nullptr;
        u64     time_left_ = 0;

    public:
        void add(Thread* t)
        {
            if (!t || count_ >= MAX_THREADS) return;
            threads_[count_++] = t;
            t->state = ThreadState::RUNNABLE;
        }

        // 纠缠 a、b：共享虚拟时间命运（量子启发预算关联）
        void entangle(Thread* a, Thread* b)
        {
            if (a && b) a->entangle = b;
        }

        // 选 vdeadline 最小（最早截止）的就绪线程
        Thread* pick_next()
        {
            Thread* best = nullptr;
            for (usize i = 0; i < count_; ++i)
            {
                Thread* t = threads_[i];
                if (t->state != ThreadState::RUNNABLE) continue;
                if (!best || t->vdeadline < best->vdeadline) best = t;
            }
            return best;
        }

        // 虚拟时间推进：vtime += ran_ticks/weight（定点），vdeadline = vtime + slice/weight；
        // 纠缠伙伴同步推进（共享命运）。
        void advance_vtime(Thread* t, u64 ran_ticks)
        {
            const u64 w = t->weight ? t->weight : 1;
            const u64 s = t->slice  ? t->slice  : 1;
            t->vtime += (ran_ticks * VTIME_SCALE) / w;
            t->vdeadline = t->vtime + (s * VTIME_SCALE) / w;
            if (t->entangle)
            {
                Thread* e = t->entangle;
                const u64 ew = e->weight ? e->weight : 1;
                const u64 es = e->slice  ? e->slice  : 1;
                e->vtime += (ran_ticks * VTIME_SCALE) / ew;
                e->vdeadline = e->vtime + (es * VTIME_SCALE) / ew;
            }
        }

        // 时间片 tick；耗尽时推进虚拟时间并重选线程，返回是否发生抢占。
        bool tick()
        {
            if (!current_)
            {
                current_ = pick_next();
                if (current_)
                {
                    current_->state = ThreadState::RUNNING;
                    time_left_ = current_->slice;
                }
                return current_ != nullptr;
            }

            if (time_left_ > 0) --time_left_;
            if (time_left_ != 0) return false;

            // 时间片耗尽：推进虚拟时间，重选最早截止线程
            advance_vtime(current_, current_->slice);
            current_->state = ThreadState::RUNNABLE;
            Thread* next = pick_next();
            if (next)
            {
                next->state = ThreadState::RUNNING;
                time_left_  = next->slice;
            }
            const bool preempted = (next != current_);
            current_ = next;
            return preempted;
        }

        void yield()
        {
            if (current_)
            {
                current_->state = ThreadState::RUNNABLE;
                current_ = nullptr;
                time_left_ = 0;
            }
        }

        void block(Thread* t)
        {
            if (t) t->state = ThreadState::BLOCKED;
        }

        void unblock(Thread* t)
        {
            if (t && t->state == ThreadState::BLOCKED) t->state = ThreadState::RUNNABLE;
        }

        Thread* current() const { return current_; }
        usize   count()  const { return count_; }
    };

} // namespace qcos

#endif // QCOS_ARCH_X86_64
