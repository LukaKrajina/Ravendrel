#pragma once
#include <vector>
#include <algorithm>
#include <numeric>
#include <cmath>
#include <iostream>
#include "fault_detection.hpp"

namespace rvdlrsp::hardware
{

    // ─────────────────────────────────────────────────────────────
    // 硬件在环（HIL）测试脚手架
    //
    // 用于真实 EMG/EEG 采集、执行器、编码器闭环验证：
    //   - SignalRecorder：记录「信号 → 命令 → 反馈」时间序列
    //   - LatencyMeter：测量信号采集 → 命令下发的端到端延迟
    //   - ClosedLoopValidator：命令 vs 反馈一致性校验
    //   - HilHarness：串联上述组件，生成 HIL 报告
    // ─────────────────────────────────────────────────────────────

    // HIL 采样记录
    struct HilSample
    {
        double t = 0;                 // 相对时间（秒）
        std::vector<double> signal;   // EMG/EEG 信号
        std::vector<double> command;  // 执行器命令
        std::vector<double> feedback; // 编码器反馈
    };

    // 信号记录器：记录闭环时间序列，供离线分析
    class SignalRecorder
    {
    private:
        std::vector<HilSample> samples_;

    public:
        void start() { samples_.clear(); }

        void record(double t, const std::vector<double> &signal,
                    const std::vector<double> &command,
                    const std::vector<double> &feedback)
        {
            samples_.push_back({t, signal, command, feedback});
        }

        const std::vector<HilSample> &samples() const { return samples_; }
        size_t size() const { return samples_.size(); }
        void clear() { samples_.clear(); }
    };

    // 延迟测量：信号采集 → 命令下发
    class LatencyMeter
    {
    private:
        std::vector<double> latencies_;

    public:
        void measure(double signal_ts, double command_ts)
        {
            latencies_.push_back(command_ts - signal_ts);
        }

        double average() const
        {
            if (latencies_.empty())
                return 0.0;
            return std::accumulate(latencies_.begin(), latencies_.end(), 0.0) / latencies_.size();
        }
        double max() const
        {
            return latencies_.empty() ? 0.0 : *std::max_element(latencies_.begin(), latencies_.end());
        }
        double min() const
        {
            return latencies_.empty() ? 0.0 : *std::min_element(latencies_.begin(), latencies_.end());
        }
        size_t count() const { return latencies_.size(); }
    };

    // 闭环验证：命令 vs 反馈一致性（复用编码器一致性校验）
    class ClosedLoopValidator
    {
    private:
        EncoderConsistencyCheck check_;
        int failure_count_ = 0;

    public:
        explicit ClosedLoopValidator(double max_error = 0.2) : check_(max_error) {}

        void validate(const std::vector<double> &command,
                      const std::vector<double> &feedback)
        {
            size_t n = std::min(command.size(), feedback.size());
            for (size_t i = 0; i < n; ++i)
                if (!check_.validate(command[i], feedback[i]))
                    ++failure_count_;
        }

        int failure_count() const { return failure_count_; }
    };

    // HIL 报告
    struct HilReport
    {
        size_t sample_count = 0;
        double avg_latency = 0;
        double max_latency = 0;
        double min_latency = 0;
        int closed_loop_failures = 0;
        bool passed = false;
    };

    // HIL 测试脚手架：串联记录 / 延迟 / 闭环验证
    class HilHarness
    {
    private:
        SignalRecorder recorder_;
        LatencyMeter latency_;
        ClosedLoopValidator validator_;

    public:
        explicit HilHarness(double max_closed_loop_error = 0.2)
            : validator_(max_closed_loop_error) {}

        // 记录一次闭环采样
        void step(double t, double signal_ts, double command_ts,
                  const std::vector<double> &signal,
                  const std::vector<double> &command,
                  const std::vector<double> &feedback)
        {
            recorder_.record(t, signal, command, feedback);
            latency_.measure(signal_ts, command_ts);
            validator_.validate(command, feedback);
        }

        // 生成 HIL 报告（max_latency_limit 为延迟上限，超限判定不通过）
        HilReport report(double max_latency_limit = 0.5) const
        {
            HilReport r;
            r.sample_count = recorder_.size();
            r.avg_latency = latency_.average();
            r.max_latency = latency_.max();
            r.min_latency = latency_.min();
            r.closed_loop_failures = validator_.failure_count();
            r.passed = (r.closed_loop_failures == 0) && (r.max_latency <= max_latency_limit);
            return r;
        }

        const SignalRecorder &recorder() const { return recorder_; }
        void reset() { recorder_.clear(); }
    };
}