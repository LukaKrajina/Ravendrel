#pragma once
#include <vector>
#include <complex>
#include "Spectral.hpp"

namespace rvdl::spacetime
{

    struct EEGBandPower
    {
        double delta = 0.0; // 0.5–4 Hz
        double theta = 0.0; // 4–8 Hz
        double alpha = 0.0; // 8–13 Hz
        double beta = 0.0;  // 13–30 Hz
        double gamma = 0.0; // 30–100 Hz
        double total_power = 0.0;
        std::vector<double> freqs; // 频率轴
        std::vector<double> psd;   // 单边功率谱密度
    };

    // 从 LFP/EEG 时间序列提取功率谱（FFT），积分各频段
    inline EEGBandPower spectral_analysis(const std::vector<double> &signal,
                                          double sampling_rate_hz)
    {
        EEGBandPower out;
        const size_t n = signal.size();
        if (n < 2)
            return out;

        double mean = 0.0;
        for (double v : signal)
            mean += v;
        mean /= static_cast<double>(n);

        size_t N = 1;
        while (N < n)
            N <<= 1; // 补零到 2 的幂
        std::vector<std::complex<double>> a(N, {0.0, 0.0});
        for (size_t i = 0; i < n; ++i)
            a[i] = {signal[i] - mean, 0.0};

        spectral::fft_radix2(a, 0, 1, N, false);

        out.freqs.assign(N / 2, 0.0);
        out.psd.assign(N / 2, 0.0);
        for (size_t k = 1; k < N / 2; ++k) // 跳过 DC
        {
            out.freqs[k] = k * sampling_rate_hz / static_cast<double>(N);
            out.psd[k] = 2.0 * std::norm(a[k]) / (static_cast<double>(N) * N);
        }

        auto band = [&](double f0, double f1)
        {
            double p = 0.0;
            for (size_t k = 1; k < N / 2; ++k)
                if (out.freqs[k] >= f0 && out.freqs[k] < f1)
                    p += out.psd[k];
            return p;
        };
        out.delta = band(0.5, 4.0);
        out.theta = band(4.0, 8.0);
        out.alpha = band(8.0, 13.0);
        out.beta = band(13.0, 30.0);
        out.gamma = band(30.0, 100.0);
        out.total_power = out.delta + out.theta + out.alpha + out.beta + out.gamma;
        return out;
    }
}