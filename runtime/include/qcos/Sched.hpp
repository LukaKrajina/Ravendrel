#pragma once

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