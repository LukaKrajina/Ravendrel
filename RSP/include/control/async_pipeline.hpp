#pragma once
#include <thread>
#include <atomic>
#include <functional>
#include <chrono>
#include <utility>
#include <iostream>
#include "hardware/observability.hpp"

namespace rvdlrsp::control
{


    class PhysicsWorker
    {
    private:
        std::atomic<bool> running_{false};
        std::thread thread_;
        std::function<void(double)> step_fn_;
        double frequency_hz_;

    public:
        PhysicsWorker(double frequency_hz, std::function<void(double)> step)
            : step_fn_(std::move(step)), frequency_hz_(frequency_hz) {}

        ~PhysicsWorker() { stop(); }

        void start()
        {
            if (running_.exchange(true))
                return;
            thread_ = std::thread([this] {
                auto period = std::chrono::milliseconds(
                    static_cast<int>(1000.0 / frequency_hz_));
                while (running_.load())
                {
                    auto t0 = std::chrono::steady_clock::now();
                    if (step_fn_)
                        step_fn_(1.0 / frequency_hz_);
                    auto t1 = std::chrono::steady_clock::now();
                    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0);
                    std::this_thread::sleep_for(period > elapsed
                                                    ? period - elapsed
                                                    : std::chrono::milliseconds(0));
                }
            });
            RVDLRSP_INFO("async") << "Physics worker started at "
                                   << frequency_hz_ << " Hz.";
        }

        void stop()
        {
            running_.store(false);
            if (thread_.joinable())
                thread_.join();
        }

        bool running() const { return running_.load(); }
        double frequency_hz() const { return frequency_hz_; }
    };

    template <typename T>
    class DoubleBuffer
    {
    private:
        T buffers_[2];
        std::atomic<int> front_{0};
        mutable std::mutex read_mtx_;

    public:
        T &acquire_write()
        {
            return buffers_[1 - front_.load()];
        }

        void swap()
        {
            front_.store(1 - front_.load());
        }

        T snapshot() const
        {
            std::lock_guard<std::mutex> lock(read_mtx_);
            return buffers_[front_.load()];
        }
    };
}