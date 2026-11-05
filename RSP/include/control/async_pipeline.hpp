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

    // ─────────────────────────────────────────────────────────────
    // 异步物理线程（物理/渲染分线程）
    //
    // 物理步进在后台线程以固定频率执行，渲染线程（主线程）异步
    // 读取最新物理状态，解耦「固定步长物理」与「可变帧率渲染」，
    // 消除同线程下的互相阻塞。
    //
    // 线程安全：step_fn_ 内部负责对共享物理状态的同步（如 mutex
    // 或原子快照），本类只负责调度。
    // ─────────────────────────────────────────────────────────────

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

    // ─── 双缓冲快照：物理线程写入，渲染线程读取 ─────────────
    // front_ 指向「最新已发布」的缓冲（读侧），写侧写 front 的反面，
    // 写完后 swap 交换 front，读侧 snapshot 读 front。
    template <typename T>
    class DoubleBuffer
    {
    private:
        T buffers_[2];
        std::atomic<int> front_{0};
        mutable std::mutex read_mtx_;

    public:
        // 物理线程：获取后缓冲（front 反面，可安全修改）
        T &acquire_write()
        {
            return buffers_[1 - front_.load()];
        }

        // 物理线程：写完交换（发布新快照）
        void swap()
        {
            front_.store(1 - front_.load());
        }

        // 渲染线程：读取最新快照（复制）
        T snapshot() const
        {
            std::lock_guard<std::mutex> lock(read_mtx_);
            return buffers_[front_.load()];
        }
    };
}