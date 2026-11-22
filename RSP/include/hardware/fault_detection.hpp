#pragma once
#include <chrono>
#include <functional>
#include <cmath>
#include <utility>
#include <iostream>
#include "observability.hpp"

namespace rvdlrsp::hardware
{


    class HeartbeatMonitor
    {
    private:
        std::chrono::steady_clock::time_point last_beat_;
        double timeout_s_;

    public:
        explicit HeartbeatMonitor(double timeout_s = 1.0) : timeout_s_(timeout_s)
        {
            beat();
        }

        void beat()
        {
            last_beat_ = std::chrono::steady_clock::now();
        }

        double time_since_beat() const
        {
            auto now = std::chrono::steady_clock::now();
            return std::chrono::duration<double>(now - last_beat_).count();
        }

        bool alive() const
        {
            return time_since_beat() < timeout_s_;
        }
    };

    class Watchdog
    {
    private:
        HeartbeatMonitor monitor_;
        std::function<void()> on_timeout_;
        bool triggered_ = false;

    public:
        Watchdog(double timeout_s, std::function<void()> on_timeout)
            : monitor_(timeout_s), on_timeout_(std::move(on_timeout)) {}

        void feed()
        {
            monitor_.beat();
            triggered_ = false;
        }

        bool check()
        {
            if (triggered_)
                return true;
            if (!monitor_.alive())
            {
                triggered_ = true;
                RVDLRSP_WARN("hw") << "Watchdog timeout, triggering callback.";
                if (on_timeout_)
                    on_timeout_();
                return true;
            }
            return false;
        }

        bool triggered() const { return triggered_; }
    };

    class EncoderConsistencyCheck
    {
    private:
        double max_error_;

    public:
        explicit EncoderConsistencyCheck(double max_error = 0.2)
            : max_error_(max_error) {}

        bool validate(double commanded, double measured) const
        {
            return std::fabs(commanded - measured) <= max_error_;
        }

        double max_error() const { return max_error_; }
    };

    class DualChannelRedundancy
    {
    private:
        double max_disagreement_;

    public:
        explicit DualChannelRedundancy(double max_disagreement = 0.1)
            : max_disagreement_(max_disagreement) {}

        bool agree(double channel_a, double channel_b) const
        {
            return std::fabs(channel_a - channel_b) <= max_disagreement_;
        }

        double max_disagreement() const { return max_disagreement_; }
    };
}