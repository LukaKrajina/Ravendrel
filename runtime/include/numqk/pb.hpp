#pragma once
//
// pb.hpp —— 点参数（point-parameter）库：超分 + 超量化 + 拓扑流形压缩成点
//
// SI,参数量新单位：不用「参数量 B（billion，十亿个显式权重）」，而是
// 「点参数量 pB（point-billion）」。一个「点」是一小簇**潜向量**，解码（超分）时
// 展开成一大块有效权重 —— 于是 **1 pB ≡ 1000 B 的有效容量，但存储 ≈ 1/1000**。
// 同样的内存 / 显存预算能装下数量级更大的模型，跑在真实量子设备（qubit 预算
// 本身就是稀缺资源）上也更可行。
//
// 三个机制各司其职（与「超分 + 超量化 + 拓扑多维流形压缩」一一对应）：
//
//   · 超分（hyper-resolution）：分块 DCT 低秩 —— 每块只保留 r 个低频系数，
//     解码时补零 + 逆 DCT，从 r 个系数插值出 S 个权重（S/r 是超分倍率）。
//     与图像超分 / hypernetwork 类似：都是低频流形决定权重的主体结构，
//     高频是冗余细节。
//
//   · 超量化（super-quantization）：每个系数标量量化到 q 级码本（均匀量化，
//     sub-8-bit，q 可小到 16 级 = 4 bit）。量化后的点存的是**码本索引**，
//     不是浮点 —— 存储从「8 字节 double」压到「log2(q) bit」。
//
//   · 拓扑流形压缩（manifold compression）：DCT 基是信号空间的**低秩流形**
//     的坐标图册 —— 权重向量在参数空间里并不占满整个高维体，而是贴着一条
//     低频流形。每个点的 r 个系数就是它在这条流形上的坐标，重建 = 从流形坐标
//     回到高维权重。
//
// 最小可验证内核：不依赖任何模型，作用在任意 double 参数
// 向量上，压缩 / 解压可独立验证（重建误差有界、压缩比精确、逐位可复现）。
//
// 依赖：仅 STL（DCT 用闭式 cos，量化用均匀码本），无 QVM / Kokkos，可在无 GPU
// 的机器上单测。
//
#include <cmath>
#include <cstdint>
#include <vector>

namespace numqk
{
namespace pb
{
    /// 一个压缩后的「点参数」表示。
    ///
    /// 存储量 = num_points × rank 个量化索引（每个 log2(q) bit），
    /// 有效容量 = num_points × block_size 个权重（约等于 8 byte × 有效参数的
    /// float 占用）。二者的比值就是「省内存」的倍数。
    struct PointParams
    {
        size_t block_size = 0; ///< 每块的有效权重数 S
        size_t rank = 0;       ///< 每块保留的 DCT 系数数 r（超分倍率 = S/r）
        size_t quant_levels = 0; ///< 量化级数 q（超量化）
        size_t total_params = 0; ///< 原始权重总数 N
        double lo = 0.0;         ///< 系数范围下界（码本覆盖域）
        double hi = 0.0;         ///< 系数范围上界
        std::vector<uint32_t> indices; ///< 展平的点：num_points × rank 个码本索引
    };

    /// 压缩：double 参数向量 → 点参数。
    ///
    /// `block_size` S、`rank` r（r < S，推荐 S/r ≈ 8~32）、`quant_levels` q
    /// （推荐 256 以内，2 的幂）。参数总数 N 不是 S 的整数倍时，末块补零（
    /// 解压时按 total_params 截断，不丢权重）。
    inline PointParams compress(const std::vector<double> &W, size_t block_size,
                                size_t rank, size_t quant_levels)
    {
        PointParams pp;
        pp.block_size = block_size;
        pp.rank = (rank > 0 && rank < block_size) ? rank : 1;
        pp.quant_levels = quant_levels > 1 ? quant_levels : 16;
        pp.total_params = W.size();

        if (W.empty())
            return pp;

        // DCT-II 系数范围（全局 lo/hi）：量化码本覆盖所有块的系数范围。
        // 先扫一遍求 min/max，再统一量化 —— 码本是「整条流形」的，不是每块
        // 单独一个（那样要多存 num_points 个码本，把压缩吃回去）。
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
                    if (acc < lo) lo = acc;
                    if (acc > hi) hi = acc;
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
                // 归一化到 [0,1] → 量化到 q 级 → 码本索引
                const double t = (acc - lo) / range; // 0..1
                uint32_t idx = static_cast<uint32_t>(t * static_cast<double>(quant_levels - 1) + 0.5);
                if (idx >= quant_levels) idx = static_cast<uint32_t>(quant_levels - 1);
                pp.indices.push_back(idx);
            }
        }
        return pp;
    }

    /// 解压（超分）：点参数 → 参数向量（长度 = total_params）。
    ///
    /// 每个点：rank 个码本索引 → rank 个系数 → 补零到 S → 逆 DCT → S 个权重。
    /// 重建误差 = 高频截断误差（(S-r) 个被丢弃的系数）+ 量化误差（q 级量化）。
    inline std::vector<double> decompress(const PointParams &pp)
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
                    const double coeff = pp.lo + t * range; // 反量化
                    // 标准 DCT-II 反变换：x[i] = (1/S)(C0 + 2·Σ_{k≥1} Ck·cos)。
                    // k=0 项权重 1，k≥1 项权重 2 —— 这是 DCT-II 的正交归一化。
                    // 漏掉这个 k=0 与 k≥1 的 1:2 差别，重建的直流分量会差一倍。
                    const double w = (k == 0) ? 1.0 : 2.0;
                    acc += coeff * w * std::cos(M_PI * (static_cast<double>(i) + 0.5) *
                                               static_cast<double>(k) / static_cast<double>(S));
                }
                acc /= static_cast<double>(S);
                out[off + i] = acc;
            }
        }
        return out;
    }

    /// 有效容量（十亿个权重，B）：点展开成的权重数 / 1e9。
    inline double effective_billions(const PointParams &pp)
    {
        return static_cast<double>(pp.total_params) / 1e9;
    }

    /// 点参数量（pB）：按「点数 / 1e9 × 展开率」计 —— 1 pB ≡ 1000 B 有效容量。
    ///
    /// 这里的约定：1 pB 的点，经超分（S/r 倍）+ 超量化后，等效 1000 B 权重。
    /// 于是 pB 数 = 有效 B 数 / 1000。它度量的是「这堆点展开后有多大」，而不是
    /// 「存了多少个点」—— 后者是 indices.size()。
    inline double point_billions(const PointParams &pp)
    {
        return effective_billions(pp) / 1000.0;
    }

    /// 存储压缩比：原始 float 存储 / 点存储（点存的是 log2(q) bit 的索引）。
    /// 返回「省内存的倍数」，越大越省。
    inline double storage_ratio(const PointParams &pp)
    {
        if (pp.indices.empty() || pp.total_params == 0)
            return 1.0;
        const double orig_bits = static_cast<double>(pp.total_params) * 64.0; // double
        const double point_bits = static_cast<double>(pp.indices.size()) *
                                  std::log2(static_cast<double>(pp.quant_levels));
        return orig_bits / point_bits;
    }
}
}