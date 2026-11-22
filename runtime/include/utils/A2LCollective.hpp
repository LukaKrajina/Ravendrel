#pragma once
#include "utils/A2L.hpp"
#include "utils/A2LNet.hpp"
#include "utils/A2LNetIo.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace a2l
{
    namespace net
    {
        struct AllReduceRound
        {
            uint64_t round = 0;
            size_t expected = 0;
            size_t received = 0;
            bool degraded = false;
            std::vector<double> sum;
            double elapsed_ms = 0.0;
        };






        class GradientAllReduce
        {
        public:
            explicit GradientAllReduce(size_t dimension, size_t expected_nodes = 1)
                : dim_(dimension), expected_(std::max<size_t>(1, expected_nodes)) {}

            size_t dimension() const { return dim_; }
            size_t expected() const { return expected_; }
            void set_expected(size_t n) { expected_ = std::max<size_t>(1, n); }

            bool contribute(uint64_t round, const std::string &from,
                            const std::vector<double> &grad)
            {
                if (grad.size() != dim_) return false;
                for (double x : grad)
                    if (!std::isfinite(x)) return false;
                std::lock_guard<std::mutex> g(mu_);
                if (round < current_round_) return false;
                if (round > current_round_)
                {
                    current_round_ = round;
                    buckets_.clear();
                }
                auto &b = buckets_[round];
                if (b.sum.empty()) b.sum.assign(dim_, 0.0);
                if (b.seen.find(from) != b.seen.end()) return false;
                b.seen.emplace(from, true);
                for (size_t i = 0; i < dim_; ++i) b.sum[i] += grad[i];
                b.received += 1;
                b.round = round;
                b.expected = expected_;
                cv_.notify_all();
                return true;
            }

            AllReduceRound wait(uint64_t round, int timeout_ms)
            {
                const auto t0 = std::chrono::steady_clock::now();
                std::unique_lock<std::mutex> lk(mu_);
                cv_.wait_for(lk, std::chrono::milliseconds(std::max(0, timeout_ms)), [&] {
                    const auto it = buckets_.find(round);
                    return it != buckets_.end() && it->second.received >= expected_;
                });
                AllReduceRound out;
                out.round = round;
                out.expected = expected_;
                auto it = buckets_.find(round);
                if (it != buckets_.end())
                {
                    out.received = it->second.received;
                    out.sum = it->second.sum;
                }
                else out.sum.assign(dim_, 0.0);
                out.degraded = out.received < expected_;
                const double denom = double(std::max<size_t>(1, out.received));
                for (double &x : out.sum) x /= denom;
                out.elapsed_ms = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - t0).count();
                if (it != buckets_.end()) buckets_.erase(it);
                return out;
            }

            static std::string encode(uint64_t round, const std::string &from,
                                      const std::vector<double> &grad)
            {
                std::string s = std::to_string(round) + "|" + from + "|";
                for (size_t i = 0; i < grad.size(); ++i)
                {
                    if (i) s += ",";
                    char buf[32];
                    std::snprintf(buf, sizeof(buf), "%.17g", grad[i]);
                    s += buf;
                }
                return s;
            }
            static bool decode(const std::string &s, uint64_t &round,
                               std::string &from, std::vector<double> &grad)
            {
                const auto p1 = s.find('|');
                const auto p2 = p1 == std::string::npos ? std::string::npos : s.find('|', p1 + 1);
                if (p1 == std::string::npos || p2 == std::string::npos) return false;
                try
                {
                    round = std::stoull(s.substr(0, p1));
                    from = s.substr(p1 + 1, p2 - p1 - 1);
                    grad.clear();
                    const std::string body = s.substr(p2 + 1);
                    size_t pos = 0;
                    while (pos <= body.size())
                    {
                        const size_t comma = body.find(',', pos);
                        const std::string tok = body.substr(pos, comma == std::string::npos
                            ? std::string::npos : comma - pos);
                        if (!tok.empty()) grad.push_back(std::stod(tok));
                        if (comma == std::string::npos) break;
                        pos = comma + 1;
                    }
                }
                catch (...) { return false; }
                return true;
            }

            uint64_t current_round() const
            {
                std::lock_guard<std::mutex> g(mu_);
                return current_round_;
            }
            size_t pending_rounds() const
            {
                std::lock_guard<std::mutex> g(mu_);
                return buckets_.size();
            }
        private:
            struct Bucket
            {
                uint64_t round = 0;
                size_t received = 0, expected = 0;
                std::vector<double> sum;
                std::unordered_map<std::string, bool> seen;
            };
            size_t dim_;
            size_t expected_;
            mutable std::mutex mu_;
            std::condition_variable cv_;
            uint64_t current_round_ = 0;
            std::unordered_map<uint64_t, Bucket> buckets_;
        };

        class ReconnectPolicy
        {
        public:
            struct Config
            {
                uint64_t base_ms = 100;
                uint64_t max_ms = 30000;
                size_t max_attempts = 8;
                uint64_t seed = 42;
            };
            ReconnectPolicy() : cfg_{100, 30000, 8, 42} {}
            explicit ReconnectPolicy(Config cfg) : cfg_(cfg) {}

            uint64_t delay_for(size_t attempt) const
            {
                if (attempt == 0) return 0;
                const size_t k = std::min(attempt - 1, size_t(20));
                const uint64_t raw = std::min(cfg_.max_ms,
                    cfg_.base_ms * (uint64_t(1) << k));
                uint64_t x = cfg_.seed + uint64_t(attempt) * 0x9e3779b97f4a7c15ULL;
                x ^= x << 13; x ^= x >> 7; x ^= x << 17;
                const uint64_t jitter = (raw / 8) ? x % (raw / 8) : 0;
                return raw + jitter;
            }
            bool exhausted(size_t attempt) const { return attempt >= cfg_.max_attempts; }
            const Config &config() const { return cfg_; }
        private:
            Config cfg_;
        };









        template <class Node>
        class DistributedTrainer
        {
        public:
            DistributedTrainer(Node &node, size_t dimension, size_t expected_nodes)
                : node_(node), allreduce_(dimension, expected_nodes),
                  policy_(), running_(false), round_(0) {}

            ~DistributedTrainer() { stop(); }

            void start()
            {
                if (running_.exchange(true)) return;
                node_.on_message([this](const NetMessage &m) { on_message(m); });
                reconnect_thread_ = std::thread([this] { reconnect_loop(); });
            }
            void stop()
            {
                if (!running_.exchange(false)) return;
                if (reconnect_thread_.joinable()) reconnect_thread_.join();
            }

            AllReduceRound all_reduce(const std::vector<double> &local, int timeout_ms = 2000)
            {
                const uint64_t r = round_.fetch_add(1, std::memory_order_relaxed) + 1;
                (void)allreduce_.contribute(r, node_.id(), local);
                (void)node_.broadcast("GRAD",
                    GradientAllReduce::encode(r, node_.id(), local));
                return allreduce_.wait(r, timeout_ms);
            }

            size_t reconnect_attempts() const { return attempts_.load(); }
            size_t successful_reconnects() const { return successes_.load(); }
            uint64_t rounds() const { return round_.load(); }
            GradientAllReduce &reducer() { return allreduce_; }
            ReconnectPolicy &policy() { return policy_; }

        private:
            void on_message(const NetMessage &m)
            {
                if (m.topic != "GRAD") return;
                uint64_t r = 0;
                std::string from;
                std::vector<double> g;
                if (!GradientAllReduce::decode(m.body, r, from, g)) return;
                (void)allreduce_.contribute(r, from.empty() ? m.from : from, g);
            }

            void reconnect_loop()
            {
                size_t attempt = 0;
                while (running_)
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(200));
                    const auto peers = node_.peers();
                    bool any_dead = false;
                    for (const auto &p : peers) if (!p.alive) { any_dead = true; break; }
                    if (!any_dead) { attempt = 0; continue; }
                    if (policy_.exhausted(attempt)) continue;
                    ++attempt;
                    attempts_.fetch_add(1);
                    const uint64_t wait_ms = policy_.delay_for(attempt);
                    (void)wait_ms;
                    for (const auto &p : peers)
                    {
                        if (p.alive || p.port == 0) continue;
                        if (node_.connect_peer(p.host, p.port, p.id))
                            successes_.fetch_add(1);
                    }
                }
            }

            Node &node_;
            GradientAllReduce allreduce_;
            ReconnectPolicy policy_;
            std::atomic<bool> running_;
            std::atomic<uint64_t> round_{0};
            std::atomic<size_t> attempts_{0}, successes_{0};
            std::thread reconnect_thread_;
        };

    }
}