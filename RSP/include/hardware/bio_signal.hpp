#pragma once
#include <vector>
#include <string>
#include <memory>
#include <mutex>
#include <cmath>
#include <random>
#include <utility>
#include <iostream>
#include "observability.hpp"

namespace rvdlrsp::hardware
{

    // ─────────────────────────────────────────────────────────────
    // 生物信号源抽象（真实硬件闭环）
    //
    // EMG（肌电）→ 义肢关节意图；EEG（脑电）→ 义眼注视意图。
    // 采用「接口 + 仿真桩 + 外部 SDK 回调注入」三层结构，
    // 沿用 qcdrc::ExternalMocap 的成熟模式：
    //   - Sim*      ：无硬件时确定性仿真
    //   - External* ：真实设备 SDK 在回调中 push 信号，update 时取出
    // ─────────────────────────────────────────────────────────────

    // 生物信号类型
    enum class BioSignalKind
    {
        EMG, // 肌电（义肢）
        EEG  // 脑电（义眼）
    };

    // ─── 生物信号源接口 ──────────────────────────────────────
    class IBioSignalSource
    {
    public:
        virtual ~IBioSignalSource() = default;
        // 原始归一化信号（[-1,1]），每通道一个元素
        virtual std::vector<double> sample() = 0;
        // 阈值化 bits（0/1），可直接喂给 ConsciousnessController
        virtual std::vector<int> sample_bits(double threshold = 0.5) = 0;
        virtual size_t channel_count() const = 0;
        virtual bool is_connected() const = 0;
        virtual std::string name() const = 0;
    };

    // ─── 仿真 EMG 源（义肢肌电信号）──────────────────────────
    class SimEmgSource : public IBioSignalSource
    {
    private:
        size_t channels_;
        double amplitude_;
        mutable std::mt19937 rng_;
        mutable std::uniform_real_distribution<double> dist_;

    public:
        explicit SimEmgSource(size_t channels = 4, double amplitude = 0.3)
            : channels_(channels), amplitude_(amplitude),
              rng_(std::random_device{}()), dist_(-1.0, 1.0)
        {
            RVDLRSP_INFO("hw") << "Sim EMG source online (" << channels_
                                << " channels).";
        }

        std::vector<double> sample() override
        {
            std::vector<double> s(channels_);
            for (auto &v : s)
                v = dist_(rng_) * amplitude_; // 噪声肌电包络
            return s;
        }

        std::vector<int> sample_bits(double threshold) override
        {
            auto s = sample();
            std::vector<int> bits(channels_);
            for (size_t i = 0; i < channels_; ++i)
                bits[i] = (std::fabs(s[i]) > threshold) ? 1 : 0; // 包络阈值检测
            return bits;
        }

        size_t channel_count() const override { return channels_; }
        bool is_connected() const override { return true; }
        std::string name() const override { return "SimEmgSource"; }
    };

    // ─── 仿真 EEG 源（义眼脑电信号）──────────────────────────
    class SimEegSource : public IBioSignalSource
    {
    private:
        size_t channels_;
        double amplitude_;
        double baseline_; // 基线（兴奋度中枢）
        mutable std::mt19937 rng_;
        mutable std::uniform_real_distribution<double> dist_;

    public:
        explicit SimEegSource(size_t channels = 8, double amplitude = 0.2,
                              double baseline = 0.5)
            : channels_(channels), amplitude_(amplitude), baseline_(baseline),
              rng_(std::random_device{}()), dist_(-1.0, 1.0)
        {
            RVDLRSP_INFO("hw") << "Sim EEG source online (" << channels_
                                << " channels).";
        }

        std::vector<double> sample() override
        {
            std::vector<double> s(channels_);
            for (auto &v : s)
                v = baseline_ + dist_(rng_) * amplitude_; // 基线 + 波动
            return s;
        }

        std::vector<int> sample_bits(double threshold) override
        {
            auto s = sample();
            std::vector<int> bits(channels_);
            for (size_t i = 0; i < channels_; ++i)
                bits[i] = (s[i] > threshold) ? 1 : 0; // 兴奋度阈值
            return bits;
        }

        size_t channel_count() const override { return channels_; }
        bool is_connected() const override { return true; }
        std::string name() const override { return "SimEegSource"; }
    };

    // ─── 外部生物信号源（真实硬件 SDK 回调注入）──────────────
    // 真实 EMG/EEG 采集设备在 SDK 回调中调用 push_signal()/push_bits()
    // 注入最新信号；sample() 返回最新注入值。与具体 SDK 解耦。
    class ExternalBioSignalSource : public IBioSignalSource
    {
    private:
        size_t channels_;
        std::vector<double> latest_;
        std::vector<int> latest_bits_;
        mutable std::mutex mtx_;
        std::string name_;

    public:
        explicit ExternalBioSignalSource(size_t channels,
                                         std::string name = "ExternalBioSignalSource")
            : channels_(channels), latest_(channels, 0.0), latest_bits_(channels, 0),
              name_(std::move(name)) {}

        // 真实设备 SDK 回调：注入最新原始信号
        void push_signal(const std::vector<double> &s)
        {
            std::lock_guard<std::mutex> lock(mtx_);
            latest_ = s;
        }

        // 真实设备 SDK 回调：注入最新阈值化 bits
        void push_bits(const std::vector<int> &b)
        {
            std::lock_guard<std::mutex> lock(mtx_);
            latest_bits_ = b;
        }

        std::vector<double> sample() override
        {
            std::lock_guard<std::mutex> lock(mtx_);
            return latest_;
        }

        std::vector<int> sample_bits(double) override
        {
            std::lock_guard<std::mutex> lock(mtx_);
            return latest_bits_;
        }

        size_t channel_count() const override { return channels_; }
        bool is_connected() const override { return true; }
        std::string name() const override { return name_; }
    };

    // ─── 生物信号源工厂 ──────────────────────────────────────
    inline std::shared_ptr<IBioSignalSource> make_bio_signal_source(
        BioSignalKind kind, size_t channels = 4, bool real = false)
    {
        if (real)
            return std::make_shared<ExternalBioSignalSource>(
                channels, (kind == BioSignalKind::EMG) ? "ExternalEmgSource"
                                                       : "ExternalEegSource");
        if (kind == BioSignalKind::EMG)
            return std::make_shared<SimEmgSource>(channels);
        return std::make_shared<SimEegSource>(channels);
    }
}