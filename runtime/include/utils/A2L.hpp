#pragma once
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace a2l
{
    struct Capabilities
    {
        static constexpr bool has_thread = true;
        static constexpr bool has_coroutine =
#if defined(__cpp_impl_coroutine)
            true;
#else
            false;
#endif
        static constexpr bool has_std_execution =
#if defined(__cpp_lib_execution)
            true;
#else
            false;
#endif
        static bool iocp_backend() { return false; }
        static bool io_uring_backend() { return false; }
        static bool epoll_backend() { return false; }
    };




    class CoopBudget
    {
    public:
        explicit CoopBudget(uint32_t ops = 16) : remaining_(ops) {}
        bool consume(uint32_t n = 1)
        {
            if (remaining_ == 0) return false;
            remaining_ -= (n < remaining_ ? n : remaining_);
            return true;
        }
        bool exhausted() const { return remaining_ == 0; }
        void reset(uint32_t ops = 16) { remaining_ = ops; }
    private:
        uint32_t remaining_;
    };

    template <class T>
    class MpmcQueue
    {
    public:
        explicit MpmcQueue(size_t capacity) : cap_(capacity), slots_(capacity)
        {
            for (size_t i = 0; i < cap_; ++i)
                slots_[i].seq.store(i, std::memory_order_relaxed);
        }
        bool try_push(T v)
        {
            size_t pos = tail_.load(std::memory_order_relaxed);
            for (;;)
            {
                Slot &s = slots_[pos % cap_];
                const uint64_t seq = s.seq.load(std::memory_order_acquire);
                if (seq == pos)
                {
                    if (tail_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed))
                    {
                        s.value = std::move(v);
                        s.seq.store(pos + 1, std::memory_order_release);
                        return true;
                    }
                }
                else if (seq < pos) return false;
                else pos = tail_.load(std::memory_order_relaxed);
            }
        }
        bool try_pop(T &out)
        {
            size_t pos = head_.load(std::memory_order_relaxed);
            for (;;)
            {
                Slot &s = slots_[pos % cap_];
                const uint64_t seq = s.seq.load(std::memory_order_acquire);
                if (seq == pos + 1)
                {
                    if (head_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed))
                    {
                        out = std::move(s.value);
                        s.seq.store(pos + cap_, std::memory_order_release);
                        return true;
                    }
                }
                else if (seq < pos + 1) return false;
                else pos = head_.load(std::memory_order_relaxed);
            }
        }
        size_t capacity() const { return cap_; }
    private:
        struct Slot
        {
            std::atomic<uint64_t> seq{0};
            T value{};
        };
        size_t cap_;
        std::deque<Slot> slots_;
        std::atomic<size_t> head_{0}, tail_{0};
    };




    class ThreadPool
    {
    public:
        explicit ThreadPool(size_t threads = std::thread::hardware_concurrency(),
                            size_t queue_capacity = 4096)
            : inject_(queue_capacity), stop_(false)
        {
            if (threads == 0) threads = 1;
            workers_.reserve(threads);
            for (size_t i = 0; i < threads; ++i)
                workers_.emplace_back([this, i] { worker_loop(i); });
        }
        ~ThreadPool()
        {
            stop_.store(true, std::memory_order_release);
            cv_.notify_all();
            for (auto &t : workers_) if (t.joinable()) t.join();
        }
        ThreadPool(const ThreadPool &) = delete;
        ThreadPool &operator=(const ThreadPool &) = delete;

        template <class F>
        auto submit(F &&f) -> std::future<std::invoke_result_t<F>>
        {
            using R = std::invoke_result_t<F>;
            auto task = std::make_shared<std::packaged_task<R()>>(std::forward<F>(f));
            auto fut = task->get_future();
            std::function<void()> job = [task] { (*task)(); };
            if (!inject_.try_push(job))
            {
                std::lock_guard<std::mutex> g(fallback_mu_);
                fallback_.push_back(std::move(job));
            }
            cv_.notify_one();
            return fut;
        }

        size_t size() const { return workers_.size(); }
        size_t completed() const { return completed_.load(std::memory_order_relaxed); }

    private:
        void worker_loop(size_t)
        {
            CoopBudget budget;
            for (;;)
            {
                std::function<void()> job;
                if (inject_.try_pop(job) || pop_fallback(job))
                {
                    job();
                    completed_.fetch_add(1, std::memory_order_relaxed);
                    budget.reset();
                    continue;
                }
                if (stop_.load(std::memory_order_acquire)) break;
                if (budget.consume())
                {
                    std::unique_lock<std::mutex> lk(cv_mu_);
                    cv_.wait_for(lk, std::chrono::microseconds(50));
                }
                else { budget.reset(); std::this_thread::yield(); }
            }
        }
        bool pop_fallback(std::function<void()> &job)
        {
            std::lock_guard<std::mutex> g(fallback_mu_);
            if (fallback_.empty()) return false;
            job = std::move(fallback_.front());
            fallback_.pop_front();
            return true;
        }

        MpmcQueue<std::function<void()>> inject_;
        std::vector<std::thread> workers_;
        std::atomic<bool> stop_;
        std::atomic<size_t> completed_{0};
        std::mutex cv_mu_, fallback_mu_;
        std::condition_variable cv_;
        std::deque<std::function<void()>> fallback_;
    };




    class Strand
    {
    public:
        Strand() = default;
        template <class F>
        auto execute(ThreadPool &pool, F &&f) -> std::future<std::invoke_result_t<F>>
        {
            using R = std::invoke_result_t<F>;
            auto task = std::make_shared<std::packaged_task<R()>>(std::forward<F>(f));
            auto fut = task->get_future();
            {
                std::lock_guard<std::mutex> g(mu_);
                pending_.push_back([task] { (*task)(); });
            }
            drain(pool);
            return fut;
        }
        size_t pending() const
        {
            std::lock_guard<std::mutex> g(mu_);
            return pending_.size();
        }
    private:
        void drain(ThreadPool &pool)
        {
            if (running_.exchange(true, std::memory_order_acquire)) return;
            pool.submit([this, &pool]
            {
                for (;;)
                {
                    std::function<void()> job;
                    {
                        std::lock_guard<std::mutex> g(mu_);
                        if (pending_.empty()) { running_.store(false, std::memory_order_release); return; }
                        job = std::move(pending_.front());
                        pending_.pop_front();
                    }
                    job();
                }
            });
        }
        mutable std::mutex mu_;
        std::deque<std::function<void()>> pending_;
        std::atomic<bool> running_{false};
    };

    template <class K, class V>
    class ShardMap
    {
    public:
        explicit ShardMap(size_t shards = 16) : shards_(shards ? shards : 1) {}
        void put(const K &k, V v)
        {
            auto &s = shards_[hash(k) % shards_.size()];
            std::lock_guard<std::mutex> g(s.mu);
            s.map[k] = std::move(v);
        }
        bool get(const K &k, V &out) const
        {
            auto &s = shards_[hash(k) % shards_.size()];
            std::lock_guard<std::mutex> g(s.mu);
            auto it = s.map.find(k);
            if (it == s.map.end()) return false;
            out = it->second;
            return true;
        }
        size_t erase(const K &k)
        {
            auto &s = shards_[hash(k) % shards_.size()];
            std::lock_guard<std::mutex> g(s.mu);
            return s.map.erase(k);
        }
        size_t shards() const { return shards_.size(); }
    private:
        static size_t hash(const K &k) { return std::hash<K>{}(k); }
        struct Shard
        {
            mutable std::mutex mu;
            std::unordered_map<K, V> map;
        };
        std::deque<Shard> shards_;
    };




    class EpochReclaimer
    {
    public:
        size_t enter()
        {
            const size_t e = global_.fetch_add(1, std::memory_order_acq_rel) + 1;
            std::lock_guard<std::mutex> g(mu_);
            active_.push_back(e);
            return e;
        }
        void leave(size_t epoch)
        {
            std::lock_guard<std::mutex> g(mu_);
            active_.erase(std::remove(active_.begin(), active_.end(), epoch), active_.end());
            sweep();
        }
        template <class T>
        void retire(std::unique_ptr<T> ptr)
        {
            std::lock_guard<std::mutex> g(mu_);
            const size_t e = global_.fetch_add(1, std::memory_order_acq_rel) + 1;
            retired_.push_back({e,
                                [p = ptr.release()](void *) { delete static_cast<T *>(p); },
                                nullptr});
            sweep();
        }
        size_t retired_count() const
        {
            std::lock_guard<std::mutex> g(mu_);
            return retired_.size();
        }
    private:
        struct Item
        {
            size_t epoch;
            std::function<void(void *)> dispose;
            void *ptr;
        };
        void sweep()
        {
            const size_t safe = active_.empty()
                ? global_.load(std::memory_order_acquire)
                : *std::min_element(active_.begin(), active_.end());
            auto it = retired_.begin();
            while (it != retired_.end())
            {
                if (it->epoch <= safe) { it->dispose(it->ptr); it = retired_.erase(it); }
                else ++it;
            }
        }
        std::atomic<size_t> global_{0};
        mutable std::mutex mu_;
        std::vector<size_t> active_;
        std::vector<Item> retired_;
    };





    class Reactor
    {
    public:
        using Handler = std::function<void(int events)>;
        void register_fd(int fd, Handler h)
        {
            std::lock_guard<std::mutex> g(mu_);
            handlers_[fd] = std::move(h);
        }
        void unregister(int fd)
        {
            std::lock_guard<std::mutex> g(mu_);
            handlers_.erase(fd);
        }
        size_t poll(int events = 1)
        {
            std::vector<std::pair<int, Handler>> snapshot;
            {
                std::lock_guard<std::mutex> g(mu_);
                for (const auto &kv : handlers_) snapshot.push_back(kv);
            }
            size_t n = 0;
            for (auto &kv : snapshot) { kv.second(events); ++n; }
            return n;
        }
        size_t registered() const
        {
            std::lock_guard<std::mutex> g(mu_);
            return handlers_.size();
        }
    private:
        mutable std::mutex mu_;
        std::unordered_map<int, Handler> handlers_;
    };





    template <class T>
    struct Sender
    {
        std::function<T()> produce;
        T sync() const { return produce(); }
        template <class F>
        auto then(F &&f) const -> Sender<std::invoke_result_t<F, T>>
        {
            using R = std::invoke_result_t<F, T>;
            auto p = produce;
            auto fn = std::forward<F>(f);
            return Sender<R>{[p, fn]() mutable { return fn(p()); }};
        }
        std::future<T> via(ThreadPool &pool) const
        {
            auto p = produce;
            return pool.submit([p] { return p(); });
        }
    };
    template <class F>
    auto just(F &&f) -> Sender<std::invoke_result_t<F>>
    {
        using R = std::invoke_result_t<F>;
        auto fn = std::forward<F>(f);
        return Sender<R>{[fn] { return fn(); }};
    }

    class StructuredScope
    {
    public:
        explicit StructuredScope(ThreadPool &pool) : pool_(&pool) {}
        ~StructuredScope() { wait(); }
        StructuredScope(const StructuredScope &) = delete;
        StructuredScope &operator=(const StructuredScope &) = delete;

        template <class F>
        void spawn(F &&f)
        {
            std::lock_guard<std::mutex> g(mu_);
            futures_.push_back(pool_->submit([fn = std::forward<F>(f)]() mutable
            {
                fn();
                return 0;
            }));
        }
        void wait()
        {
            std::vector<std::future<int>> pending;
            {
                std::lock_guard<std::mutex> g(mu_);
                pending.swap(futures_);
            }
            for (auto &f : pending) if (f.valid()) f.wait();
        }
        size_t pending() const
        {
            std::lock_guard<std::mutex> g(mu_);
            return futures_.size();
        }
    private:
        ThreadPool *pool_;
        mutable std::mutex mu_;
        std::vector<std::future<int>> futures_;
    };




    class DistributedCoordinator
    {
    public:
        DistributedCoordinator(size_t shards, size_t nodes)
            : owners_(shards, 0), lease_(shards), checkpoint_(0)
        {
            for (auto &l : lease_) l.store(0, std::memory_order_relaxed);
            for (size_t i = 0; i < shards; ++i) owners_[i] = i % (nodes ? nodes : 1);
        }
        bool try_lease(size_t shard, size_t node, uint64_t epoch)
        {
            if (shard >= owners_.size() || owners_[shard] != node) return false;
            uint64_t cur = lease_[shard].load(std::memory_order_relaxed);
            if (epoch <= cur) return false;
            return lease_[shard].compare_exchange_strong(cur, epoch, std::memory_order_relaxed);
        }
        size_t commit_checkpoint() { return checkpoint_.fetch_add(1, std::memory_order_relaxed) + 1; }
        size_t checkpoint() const { return checkpoint_.load(std::memory_order_relaxed); }
        size_t owner(size_t shard) const { return shard < owners_.size() ? owners_[shard] : size_t(-1); }
        size_t shards() const { return owners_.size(); }
    private:
        std::vector<size_t> owners_;
        std::deque<std::atomic<uint64_t>> lease_;
        std::atomic<size_t> checkpoint_;
    };

    struct RuntimeResources
    {
        ThreadPool pool;
        Reactor reactor;
        EpochReclaimer reclaimer;
        explicit RuntimeResources(size_t threads = std::thread::hardware_concurrency())
            : pool(threads) {}
    };
}