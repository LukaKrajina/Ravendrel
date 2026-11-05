#pragma once
// ============================================================================
//  极简协作式调度器
//
//  任务运行到返回，不主动让出 CPU。抢占式调度与上下文切换需要架构相关的
//  只提供任务登记与轮转执行。
// ============================================================================

#include "Arch.hpp"
#include "Spinlock.hpp"

namespace qcos
{
    using TaskEntry = void (*)(void*);

    struct Task
    {
        TaskEntry entry;
        void*     arg;
    };

    class Scheduler
    {
    private:
        static constexpr usize MAX_TASKS = 64;

        Task      tasks_[MAX_TASKS];
        usize     count_  = 0;
        Spinlock  lock_;

    public:
        bool add(TaskEntry entry, void* arg)
        {
            if (!entry) return false;
            lock_.lock();
            bool ok = false;
            if (count_ < MAX_TASKS)
            {
                tasks_[count_].entry = entry;
                tasks_[count_].arg   = arg;
                ++count_;
                ok = true;
            }
            lock_.unlock();
            return ok;
        }

        usize task_count() const { return count_; }

        /** 轮转运行所有任务一次（协作式：任务返回后轮到下一个）。 */
        void run_all()
        {
            for (usize i = 0; i < count_; ++i)
            {
                Task t = tasks_[i];
                if (t.entry) t.entry(t.arg);
            }
        }
    };
}