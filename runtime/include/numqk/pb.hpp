#pragma once
#include <cmath>
#include <cstdint>
#include <vector>

namespace numqk
{
    namespace pb
    {
        struct PointParams
        {
            size_t block_size = 0;
            size_t rank = 0;
            size_t quant_levels = 0;
            size_t total_params = 0;
            double lo = 0.0;
            double hi = 0.0;
            std::vector<uint32_t> indices;
        };

        inline PointParams compress(const std::vector<double>& W, size_t block_size, size_t rank,
                                    size_t quant_levels)
        {
            PointParams pp;
            pp.block_size = block_size;
            pp.rank = (rank > 0 && rank < block_size) ? rank : 1;
            pp.quant_levels = quant_levels > 1 ? quant_levels : 16;
            pp.total_params = W.size();

            if (W.empty())
                return pp;

            double lo = 0.0, hi = 0.0;
            {
                const size_t S = block_size;
                const size_t nblocks = (W.size() + S - 1) / S;
                for (size_t b = 0; b < nblocks; ++b)
                {
                    const size_t off = b * S;
                    for (size_t k = 0; k < pp.rank; ++k)
                    {
                        double acc = 0.0;
                        for (size_t i = 0; i < S; ++i)
                        {
                            const double x = (off + i < W.size()) ? W[off + i] : 0.0;
                            acc += x * std::cos(M_PI * (static_cast<double>(i) + 0.5) *
                                                static_cast<double>(k) / static_cast<double>(S));
                        }
                        if (acc < lo)
                            lo = acc;
                        if (acc > hi)
                            hi = acc;
                    }
                }
            }
            pp.lo = lo;
            pp.hi = hi;

            const double range = (hi - lo) > 0.0 ? (hi - lo) : 1.0;
            const size_t S = block_size;
            const size_t nblocks = (W.size() + S - 1) / S;
            pp.indices.reserve(nblocks * pp.rank);

            for (size_t b = 0; b < nblocks; ++b)
            {
                const size_t off = b * S;
                for (size_t k = 0; k < pp.rank; ++k)
                {
                    double acc = 0.0;
                    for (size_t i = 0; i < S; ++i)
                    {
                        const double x = (off + i < W.size()) ? W[off + i] : 0.0;
                        acc += x * std::cos(M_PI * (static_cast<double>(i) + 0.5) *
                                            static_cast<double>(k) / static_cast<double>(S));
                    }
                    const double t = (acc - lo) / range;
                    uint32_t idx =
                        static_cast<uint32_t>(t * static_cast<double>(quant_levels - 1) + 0.5);
                    if (idx >= quant_levels)
                        idx = static_cast<uint32_t>(quant_levels - 1);
                    pp.indices.push_back(idx);
                }
            }
            return pp;
        }

        inline std::vector<double> decompress(const PointParams& pp)
        {
            std::vector<double> out(pp.total_params, 0.0);
            if (pp.total_params == 0 || pp.indices.empty())
                return out;

            const size_t S = pp.block_size;
            const size_t r = pp.rank;
            const size_t q = pp.quant_levels;
            const double range = (pp.hi - pp.lo) > 0.0 ? (pp.hi - pp.lo) : 1.0;
            const size_t nblocks = (pp.total_params + S - 1) / S;

            for (size_t b = 0; b < nblocks; ++b)
            {
                const size_t off = b * S;
                const size_t base = b * r;
                for (size_t i = 0; i < S && off + i < pp.total_params; ++i)
                {
                    double acc = 0.0;
                    for (size_t k = 0; k < r; ++k)
                    {
                        const uint32_t idx = pp.indices[base + k];
                        const double t = static_cast<double>(idx) / static_cast<double>(q - 1);
                        const double coeff = pp.lo + t * range;
                        const double w = (k == 0) ? 1.0 : 2.0;
                        acc += coeff * w *
                               std::cos(M_PI * (static_cast<double>(i) + 0.5) *
                                        static_cast<double>(k) / static_cast<double>(S));
                    }
                    acc /= static_cast<double>(S);
                    out[off + i] = acc;
                }
            }
            return out;
        }

        inline double effective_billions(const PointParams& pp)
        {
            return static_cast<double>(pp.total_params) / 1e9;
        }

        inline double point_billions(const PointParams& pp)
        {
            return effective_billions(pp) / 1000.0;
        }

        inline double storage_ratio(const PointParams& pp)
        {
            if (pp.indices.empty() || pp.total_params == 0)
                return 1.0;
            const double orig_bits = static_cast<double>(pp.total_params) * 64.0;
            const double point_bits = static_cast<double>(pp.indices.size()) *
                                      std::log2(static_cast<double>(pp.quant_levels));
            return orig_bits / point_bits;
        }
    }
}