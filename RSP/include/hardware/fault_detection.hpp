#pragma once
#include <chrono>
#include <functional>
#include <cmath>
#include <utility>
#include <iostream>
#include "observability.hpp"

namespace rvdlrsp::hardware
{

    // ─────────────────────────────────────────────────────────────
    // 故障检测与冗余（安全认证前提）
    //
    // 心跳监测、看门狗、编码器一致性校验、双通道冗余，
    // 是医疗级义肢/义眼安全认证（FDA/CE）的硬性前提。
    // ─────────────────────────────────────────────────────────────

    // ─── 心跳监测：检测组件是否定期报告存活 ─────────────────
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

    // ─── 看门狗：超时触发急停回调 ───────────────────────────
    class Watchdog
    {
    private:
        HeartbeatMonitor monitor_;
        std::function<void()> on_timeout_;
        bool triggered_ = false;

    public:
        Watchdog(double timeout_s, std::function<void()> on_timeout)
            : monitor_(timeout_s), on_timeout_(std::move(on_timeout)) {}

        // 喂狗（主循环周期性调用）
        void feed()
        {
            monitor_.beat();
            triggered_ = false;
        }

        // 检查是否超时；超时则触发一次回调
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

    // ─── 编码器一致性校验：命令 vs 反馈偏差超限 ──────────────
    // 若执行器实际位置偏离命令位置超过阈值，说明驱动/编码器故障。
    class EncoderConsistencyCheck
    {
    private:
        double max_error_;

    public:
        explicit EncoderConsistencyCheck(double max_error = 0.2)
            : max_error_(max_error) {}

        // 返回是否一致（偏差在阈值内）
        bool validate(double commanded, double measured) const
        {
            return std::fabs(commanded - measured) <= max_error_;
        }

        double max_error() const { return max_error_; }
    };

    // ─── 双通道冗余：两个传感器读数一致性 ───────────────────
    // 冗余传感器（如双编码器）读数分歧超限，说明至少一个通道故障。
    class DualChannelRedundancy
    {
    private:
        double max_disagreement_;

    public:
        explicit DualChannelRedundancy(double max_disagreement = 0.1)
            : max_disagreement_(max_disagreement) {}

        // 返回两通道是否一致
        bool agree(double channel_a, double channel_b) const
        {
            return std::fabs(channel_a - channel_b) <= max_disagreement_;
        }

        double max_disagreement() const { return max_disagreement_; }
    };
}