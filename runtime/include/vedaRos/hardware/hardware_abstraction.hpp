#pragma once
#include <string>
#include <vector>
#include <functional>
#include <thread>
#include <atomic>
#include <chrono>
#include <utility>
#include <iostream>

namespace vedaros::hardware
{

    class HardwareInterface
    {
    public:
        virtual ~HardwareInterface() = default;
        virtual bool init() { return true; }
        virtual void read() = 0;
        virtual void write() = 0;
        virtual const char *name() const = 0;
    };

    class ControllerLoop
    {
    private:
        std::vector<HardwareInterface *> interfaces_;
        std::function<void(double)> update_fn_;
        std::atomic<bool> running_{false};
        std::thread thread_;
        double frequency_hz_ = 100.0;

    public:
        ControllerLoop(double frequency_hz, std::function<void(double)> update)
            : frequency_hz_(frequency_hz), update_fn_(std::move(update)) {}

        void add_interface(HardwareInterface *hw) { interfaces_.push_back(hw); }

        void start()
        {
            if (running_.exchange(true))
                return;
            thread_ = std::thread([this]
                                  {
                auto period = std::chrono::milliseconds(
                    static_cast<int>(1000.0 / frequency_hz_));
                while (running_.load())
                {
                    auto t0 = std::chrono::steady_clock::now();
                    for (auto *hw : interfaces_) hw->read();
                    double dt = 1.0 / frequency_hz_;
                    if (update_fn_) update_fn_(dt);
                    for (auto *hw : interfaces_) hw->write();
                    auto t1 = std::chrono::steady_clock::now();
                    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0);
                    std::this_thread::sleep_for(period > elapsed ? period - elapsed
                                                                  : std::chrono::milliseconds(0));
                } });
            std::cout << "[vedaRos.hw] Controller loop started at "
                      << frequency_hz_ << " Hz.\n";
        }

        void stop()
        {
            running_.store(false);
            if (thread_.joinable())
                thread_.join();
        }
    };
}