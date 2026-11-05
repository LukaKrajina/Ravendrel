#pragma once

#include <vector>
#include <complex>
#include <cmath>
#include <thread>
#include <future>
#include <algorithm>
#include "../numqk/Numqk.hpp"

namespace qhal
{

    class FastCPUPhaseRetrieval
    {
    private:
        const double PI = 3.14159265358979323846;
        size_t width;
        size_t height;
        size_t num_threads;

        void fft_1d(std::vector<std::complex<double>> &x, bool inverse = false)
        {
            size_t N = x.size();
            if (N <= 1)
                return;

            for (size_t i = 1, j = 0; i < N; i++)
            {
                size_t bit = N >> 1;
                for (; j & bit; bit >>= 1)
                    j ^= bit;
                j ^= bit;
                if (i < j)
                    std::swap(x[i], x[j]);
            }

            for (size_t len = 2; len <= N; len <<= 1)
            {
                double angle = 2 * PI / len * (inverse ? 1 : -1);
                std::complex<double> wlen(std::cos(angle), std::sin(angle));
                for (size_t i = 0; i < N; i += len)
                {
                    std::complex<double> w(1);
                    for (size_t j = 0; j < len / 2; j++)
                    {
                        std::complex<double> u = x[i + j];
                        std::complex<double> v = x[i + j + len / 2] * w;
                        x[i + j] = u + v;
                        x[i + j + len / 2] = u - v;
                        w *= wlen;
                    }
                }
            }

            if (inverse)
            {
                for (auto &val : x)
                    val /= N;
            }
        }

        void parallel_fft_pass(std::vector<std::vector<std::complex<double>>> &matrix, bool inverse)
        {
            size_t rows = matrix.size();
            auto worker = [&](size_t start, size_t end)
            {
                for (size_t i = start; i < end; ++i)
                {
                    fft_1d(matrix[i], inverse);
                }
            };

            std::vector<std::future<void>> futures;
            size_t chunk_size = rows / num_threads;

            for (size_t t = 0; t < num_threads; ++t)
            {
                size_t start = t * chunk_size;
                size_t end = (t == num_threads - 1) ? rows : start + chunk_size;
                futures.push_back(std::async(std::launch::async, worker, start, end));
            }

            for (auto &f : futures)
            {
                f.get();
            }
        }

        void transpose(std::vector<std::vector<std::complex<double>>> &matrix)
        {
            std::vector<std::vector<std::complex<double>>> transposed(width, std::vector<std::complex<double>>(height));
            for (size_t i = 0; i < height; ++i)
            {
                for (size_t j = 0; j < width; ++j)
                {
                    transposed[j][i] = matrix[i][j];
                }
            }
            matrix = std::move(transposed);
        }

        void fft_2d(std::vector<std::vector<std::complex<double>>> &matrix, bool inverse = false)
        {
            parallel_fft_pass(matrix, inverse);
            transpose(matrix);
            parallel_fft_pass(matrix, inverse);
            transpose(matrix);
        }

    public:
        FastCPUPhaseRetrieval(size_t w, size_t h) : width(w), height(h)
        {
            num_threads = std::thread::hardware_concurrency();
            if (num_threads == 0)
                num_threads = 8;
        }

        std::vector<std::vector<double>> compute_slm_phase_mask(
            const std::vector<std::vector<double>> &target_amplitude,
            size_t iterations = 15)
        {
            std::vector<std::vector<std::complex<double>>> field(height, std::vector<std::complex<double>>(width, 1.0));

            for (size_t i = 0; i < height; ++i)
            {
                for (size_t j = 0; j < width; ++j)
                {
                    double rand_phase = (rand() / (double)RAND_MAX) * 2 * PI;
                    field[i][j] = std::polar(1.0, rand_phase);
                }
            }

            for (size_t iter = 0; iter < iterations; ++iter)
            {
                fft_2d(field, false);

                for (size_t i = 0; i < height; ++i)
                {
                    for (size_t j = 0; j < width; ++j)
                    {
                        double current_phase = std::arg(field[i][j]);
                        field[i][j] = std::polar(target_amplitude[i][j], current_phase);
                    }
                }

                fft_2d(field, true);

                for (size_t i = 0; i < height; ++i)
                {
                    for (size_t j = 0; j < width; ++j)
                    {
                        double current_phase = std::arg(field[i][j]);
                        field[i][j] = std::polar(1.0, current_phase);
                    }
                }
            }

            std::vector<std::vector<double>> slm_phase(height, std::vector<double>(width));
            for (size_t i = 0; i < height; ++i)
            {
                for (size_t j = 0; j < width; ++j)
                {
                    slm_phase[i][j] = std::arg(field[i][j]);
                }
            }

            return slm_phase;
        }
    };
}