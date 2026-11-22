#pragma once
#include <atomic>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

namespace rvdl
{
    namespace sparse
    {
        namespace res
        {
            using Complex = std::complex<double>;


            class TileScheduler
            {
            public:
                explicit TileScheduler(size_t tiles = 0) { reset(tiles); }
                void reset(size_t tiles) { total_ = tiles; next_.store(0, std::memory_order_relaxed); }
                bool claim(size_t &index)
                {
                    const size_t i = next_.fetch_add(1, std::memory_order_relaxed);
                    if (i >= total_) return false;
                    index = i;
                    return true;
                }
                size_t claimed() const { return std::min(next_.load(std::memory_order_relaxed), total_); }
                size_t total() const { return total_; }
            private:
                size_t total_ = 0;
                std::atomic<size_t> next_{0};
            };

            class StreamPool
            {
            public:
                explicit StreamPool(size_t streams = 1) : streams_(streams ? streams : 1) {}
                size_t acquire() { return cursor_.fetch_add(1, std::memory_order_relaxed) % streams_; }
                size_t size() const { return streams_; }
            private:
                size_t streams_;
                std::atomic<size_t> cursor_{0};
            };





            class TileArena
            {
            public:
                explicit TileArena(size_t slots = 0, size_t tile_len = 0)
                {
                    init(slots, tile_len);
                }
                void init(size_t slots, size_t tile_len)
                {
                    std::lock_guard<std::mutex> g(mu_);
                    tile_len_ = tile_len;
                    pool_.assign(slots, std::vector<Complex>(tile_len, Complex(0, 0)));
                    free_.resize(slots);
                    for (size_t i = 0; i < slots; ++i) free_[i] = i;
                }
                bool acquire(size_t &slot)
                {
                    std::lock_guard<std::mutex> g(mu_);
                    if (free_.empty()) return false;
                    slot = free_.back();
                    free_.pop_back();
                    return true;
                }
                void release(size_t slot)
                {
                    std::lock_guard<std::mutex> g(mu_);
                    if (slot < pool_.size()) free_.push_back(slot);
                }
                std::vector<Complex> &at(size_t slot) { return pool_.at(slot); }
                size_t capacity() const { return pool_.size(); }
                size_t tile_len() const { return tile_len_; }
            private:
                std::mutex mu_;
                size_t tile_len_ = 0;
                std::vector<std::vector<Complex>> pool_;
                std::vector<size_t> free_;
            };

            class VramBudget
            {
            public:
                explicit VramBudget(uint64_t bytes = 0) : left_(bytes), total_(bytes) {}
                bool try_reserve(uint64_t bytes)
                {
                    if (total_ == 0) return true;
                    uint64_t cur = left_.load(std::memory_order_relaxed);
                    while (cur >= bytes)
                    {
                        if (left_.compare_exchange_weak(cur, cur - bytes, std::memory_order_relaxed))
                            return true;
                    }
                    return false;
                }
                void release(uint64_t bytes)
                {
                    if (total_ == 0) return;
                    left_.fetch_add(bytes, std::memory_order_relaxed);
                }
                uint64_t available() const { return left_.load(std::memory_order_relaxed); }
                uint64_t total() const { return total_; }
            private:
                std::atomic<uint64_t> left_;
                uint64_t total_;
            };

            struct QvmResources
            {
                TileScheduler scheduler;
                StreamPool streams;
                TileArena arena;
                VramBudget vram;
                size_t cpu_threads = 1;

                QvmResources(size_t tiles = 0, size_t tile_len = 0, size_t threads = 1,
                             size_t gpu_streams = 1, uint64_t vram_bytes = 0)
                    : scheduler(tiles), streams(gpu_streams), arena(tiles, tile_len),
                      vram(vram_bytes), cpu_threads(threads ? threads : 1) {}
            };


            class QpuNodePool
            {
            public:
                explicit QpuNodePool(size_t nodes = 1) { init(nodes); }
                void init(size_t nodes)
                {
                    std::lock_guard<std::mutex> g(mu_);
                    used_.assign(nodes ? nodes : 1, false);
                }
                bool acquire(size_t &node)
                {
                    std::lock_guard<std::mutex> g(mu_);
                    for (size_t i = 0; i < used_.size(); ++i)
                        if (!used_[i]) { used_[i] = true; node = i; return true; }
                    return false;
                }
                void release(size_t node)
                {
                    std::lock_guard<std::mutex> g(mu_);
                    if (node < used_.size()) used_[node] = false;
                }
                size_t size() const { return used_.size(); }
            private:
                std::mutex mu_;
                std::vector<bool> used_;
            };

            class OramBudget
            {
            public:
                explicit OramBudget(size_t slots = 0) : left_(slots), total_(slots) {}
                bool try_acquire(size_t n = 1)
                {
                    if (total_ == 0) return true;
                    size_t cur = left_.load(std::memory_order_relaxed);
                    while (cur >= n)
                    {
                        if (left_.compare_exchange_weak(cur, cur - n, std::memory_order_relaxed))
                            return true;
                    }
                    return false;
                }
                void release(size_t n = 1)
                {
                    if (total_ == 0) return;
                    left_.fetch_add(n, std::memory_order_relaxed);
                }
                size_t available() const { return left_.load(std::memory_order_relaxed); }
            private:
                std::atomic<size_t> left_;
                size_t total_;
            };

            class QubitLease
            {
            public:
                QubitLease(QpuNodePool &pool, OramBudget &oram, size_t qubits)
                    : pool_(&pool), oram_(&oram), qubits_(qubits)
                {
                    valid_ = pool.acquire(node_) && oram.try_acquire(qubits);
                    if (!valid_ && pool_->size() > 0) { }
                }
                ~QubitLease()
                {
                    if (valid_)
                    {
                        oram_->release(qubits_);
                        pool_->release(node_);
                    }
                }
                QubitLease(const QubitLease &) = delete;
                QubitLease &operator=(const QubitLease &) = delete;
                QubitLease(QubitLease &&o) noexcept
                    : pool_(o.pool_), oram_(o.oram_), node_(o.node_), qubits_(o.qubits_), valid_(o.valid_)
                { o.valid_ = false; }
                bool valid() const { return valid_; }
                size_t node() const { return node_; }
            private:
                QpuNodePool *pool_;
                OramBudget *oram_;
                size_t node_ = 0, qubits_ = 0;
                bool valid_ = false;
            };

            class CoherenceBudget
            {
            public:
                explicit CoherenceBudget(size_t max_gates = 64) : max_(max_gates) {}
                bool try_enter()
                {
                    size_t cur = in_flight_.load(std::memory_order_relaxed);
                    while (cur < max_)
                    {
                        if (in_flight_.compare_exchange_weak(cur, cur + 1, std::memory_order_relaxed))
                            return true;
                    }
                    return false;
                }
                void leave() { in_flight_.fetch_sub(1, std::memory_order_relaxed); }
                size_t in_flight() const { return in_flight_.load(std::memory_order_relaxed); }
            private:
                size_t max_;
                std::atomic<size_t> in_flight_{0};
            };

            struct QmResources
            {
                QpuNodePool nodes;
                OramBudget oram;
                CoherenceBudget coherence;
                QmResources(size_t nodes_n = 1, size_t oram_slots = 0, size_t max_gates = 64)
                    : nodes(nodes_n), oram(oram_slots), coherence(max_gates) {}
            };
        }
    }
}