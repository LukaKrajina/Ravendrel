#pragma once

#include "Arch.hpp"
#include "Idt.hpp"
#include "LibcSubset.hpp"

#if defined(QCOS_ARCH_X86_64)

namespace qcos
{

    static constexpr u64 VTIME_SCALE = 1024;

    enum class ThreadState : u32
    {
        RUNNABLE,
        RUNNING,
        BLOCKED,
        DEAD,
    };

    struct Thread
    {
        u64 kernel_rsp = 0;

        u64 vtime = 0;
        u64 weight = 100;
        u64 slice = 10;
        u64 vdeadline = 0;

        ThreadState state    = ThreadState::RUNNABLE;
        Thread* entangle = nullptr;

        void (*entry)(void*) = nullptr;
        void*  arg           = nullptr;
        u32    id            = 0;
    };

    inline void thread_init(Thread* t, void (*entry)(void*), void* arg, u8* stack_top)
    {
        InterruptFrame* frame = reinterpret_cast<InterruptFrame*>(
            reinterpret_cast<u64>(stack_top) - sizeof(InterruptFrame));
        memset(frame, 0, sizeof(InterruptFrame));
        frame->rip    = reinterpret_cast<u64>(entry);
        frame->cs = 0x08;
        frame->rflags = 0x202;
        frame->rdi    = reinterpret_cast<u64>(arg);

        t->kernel_rsp = reinterpret_cast<u64>(frame);
        t->entry = entry;
        t->arg   = arg;
        t->slice  = t->slice ? t->slice : 1;
        t->weight = t->weight ? t->weight : 1;
        t->vdeadline = (t->slice * VTIME_SCALE) / t->weight;
    }

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

        void entangle(Thread* a, Thread* b)
        {
            if (a && b) a->entangle = b;
        }

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

}

#endif