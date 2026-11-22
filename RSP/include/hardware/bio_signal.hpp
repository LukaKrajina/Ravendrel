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


    enum class BioSignalKind
    {
        EMG,
        EEG
    };

    class IBioSignalSource
    {
    public:
        virtual ~IBioSignalSource() = default;
        virtual std::vector<double> sample() = 0;
        virtual std::vector<int> sample_bits(double threshold = 0.5) = 0;
        virtual size_t channel_count() const = 0;
        virtual bool is_connected() const = 0;
        virtual std::string name() const = 0;
    };

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
                v = dist_(rng_) * amplitude_;
            return s;
        }

        std::vector<int> sample_bits(double threshold) override
        {
            auto s = sample();
            std::vector<int> bits(channels_);
            for (size_t i = 0; i < channels_; ++i)
                bits[i] = (std::fabs(s[i]) > threshold) ? 1 : 0;
            return bits;
        }

        size_t channel_count() const override { return channels_; }
        bool is_connected() const override { return true; }
        std::string name() const override { return "SimEmgSource"; }
    };

    class SimEegSource : public IBioSignalSource
    {
    private:
        size_t channels_;
        double amplitude_;
        double baseline_;
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
                v = baseline_ + dist_(rng_) * amplitude_;
            return s;
        }

        std::vector<int> sample_bits(double threshold) override
        {
            auto s = sample();
            std::vector<int> bits(channels_);
            for (size_t i = 0; i < channels_; ++i)
                bits[i] = (s[i] > threshold) ? 1 : 0;
            return bits;
        }

        size_t channel_count() const override { return channels_; }
        bool is_connected() const override { return true; }
        std::string name() const override { return "SimEegSource"; }
    };

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

        void push_signal(const std::vector<double> &s)
        {
            std::lock_guard<std::mutex> lock(mtx_);
            latest_ = s;
        }

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