#pragma once
#include "utils/A2L.hpp"

#include <cstdint>
#include <memory>
#include <string>

namespace a2l
{
    namespace integration
    {
        class Hub
        {
        public:
            static Hub &instance()
            {
                static Hub h;
                return h;
            }

            ThreadPool &pool() { return pool_; }
            Reactor &reactor() { return reactor_; }
            DistributedCoordinator &coordinator() { return coordinator_; }

            void record(const std::string &key)
            {
                std::shared_ptr<std::atomic<uint64_t>> counter;
                {
                    std::lock_guard<std::mutex> g(counters_mu_);
                    auto it = counters_.find(key);
                    if (it == counters_.end())
                    {
                        counter = std::make_shared<std::atomic<uint64_t>>(0);
                        counters_.emplace(key, counter);
                    }
                    else counter = it->second;
                }
                counter->fetch_add(1, std::memory_order_relaxed);
            }
            uint64_t count(const std::string &key) const
            {
                std::lock_guard<std::mutex> g(counters_mu_);
                auto it = counters_.find(key);
                return it == counters_.end() ? 0 : it->second->load(std::memory_order_relaxed);
            }




            template <class F>
            void parallel_rows(size_t rows, F &&fn)
            {
                StructuredScope scope(pool_);
                for (size_t i = 0; i < rows; ++i)
                {
                    scope.spawn([fn, i]() mutable { fn(i); });
                }
                scope.wait();
            }

        private:
            Hub() : pool_(std::thread::hardware_concurrency()), coordinator_(16, 1) {}
            ThreadPool pool_;
            Reactor reactor_;
            DistributedCoordinator coordinator_;
            mutable std::mutex counters_mu_;
            std::unordered_map<std::string, std::shared_ptr<std::atomic<uint64_t>>> counters_;
        };

        inline Hub &hub() { return Hub::instance(); }
        inline void touch(const char *component) { hub().record(std::string("a2l.") + component); }

        inline std::string report()
        {
            return "a2l.threads=" + std::to_string(hub().pool().size()) +
                   " a2l.completed=" + std::to_string(hub().pool().completed()) +
                   " a2l.reactor_fds=" + std::to_string(hub().reactor().registered()) +
                   " a2l.checkpoint=" + std::to_string(hub().coordinator().checkpoint());
        }
    }
}

#define A2L_TOUCH(component) ::a2l::integration::touch(component)