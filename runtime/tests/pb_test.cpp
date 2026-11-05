// ============================================================================
// pb_test.cpp —— 点参数（pB）库：超分 + 超量化 + 拓扑流形压缩成点
//
// 断言的是「性质」，不是「跑一遍不崩」：
//   · 压缩比精确：点存的是 log2(q) bit 的索引，不是 8 字节 double；
//   · 低频流形假设成立时重建误差有界（平滑信号 → 低 rank 就能重建）；
//   · 高频信号截断误差大（这正是「流形压缩」的代价 —— 不是 bug）；
//   · 1 pB ≡ 1000 B 的换算口径一致。
// ============================================================================

#include "numqk/pb.hpp"

#include <cmath>
#include <cstdio>
#include <vector>

namespace
{
    int g_fail = 0;
    void check(bool ok, const char *what)
    {
        std::printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
        if (!ok)
            ++g_fail;
    }
    void check_near(double a, double b, double tol, const char *what)
    {
        const bool ok = std::fabs(a - b) <= tol;
        std::printf("  %s %s (%.6g vs %.6g)\n", ok ? "ok  " : "FAIL", what, a, b);
        if (!ok)
            ++g_fail;
    }
    double rel_err(const std::vector<double> &a, const std::vector<double> &b)
    {
        double n = 0.0, d = 0.0;
        for (size_t i = 0; i < a.size(); ++i)
        {
            n += (a[i] - b[i]) * (a[i] - b[i]);
            d += a[i] * a[i];
        }
        return d > 0.0 ? std::sqrt(n / d) : 0.0;
    }
} // namespace

int main()
{
    using namespace numqk::pb;

    std::printf("[1] 平滑信号（低频流形）：低 rank 重建误差小\n");
    {
        // 缓变余弦（DCT 是偶对称基，cos 在其上**稀疏**）—— 能量集中在低频
        // k=2、k=4，rank=6（k=0..5）正好覆盖，这正是「低频流形假设成立」的样子。
        // （用 sin 的话，sin 是奇函数、在 DCT 基上不稀疏，截断误差会很大 ——
        //   那测的是「相位不匹配」而不是「流形压缩」。）
        std::vector<double> w(256);
        for (size_t i = 0; i < w.size(); ++i)
            w[i] = 3.0 * std::cos(2.0 * M_PI * i / 32.0) + 0.5 * std::cos(2.0 * M_PI * i / 16.0);

        PointParams pp = compress(w, /*block=*/32, /*rank=*/6, /*quant=*/256);
        std::vector<double> back = decompress(pp);

        check(back.size() == w.size(), "解压长度 = 原始长度");
        const double e = rel_err(w, back);
        check(e < 0.05, "平滑信号低 rank 重建相对误差 < 5%");
        check(pp.total_params == 256, "total_params = 256");
        check(pp.indices.size() == (256 / 32) * 6, "点数 × rank = 8 块 × 6 = 48 个索引");
    }

    std::printf("[2] 压缩比：点存索引，不是 double\n");
    {
        std::vector<double> w(100000, 1.0);
        for (size_t i = 0; i < w.size(); ++i)
            w[i] = std::sin(i * 0.01);
        PointParams pp = compress(w, /*block=*/64, /*rank=*/8, /*quant=*/256);

        // 原始：100000 × 64 bit = 6.4e6 bit；点：(100000/64)×8 索引 × log2(256)=8 bit
        // = 1250 × 8 × 8 = 8e4 bit。压缩比 = 6.4e6 / 8e4 = 80。
        const double ratio = storage_ratio(pp);
        check(ratio > 50.0, "存储压缩比 > 50（省内存）");
        check_near(effective_billions(pp), 100000.0 / 1e9, 1e-15, "有效容量 = 100000 / 1e9 B");
    }

    std::printf("[3] 高频信号：截断误差大（流形压缩的代价，非 bug）\n");
    {
        // 纯白噪声 —— 没有低频流形，低 rank 必然丢高频
        std::vector<double> w(128);
        unsigned seed = 12345;
        for (size_t i = 0; i < w.size(); ++i)
        {
            seed = seed * 1103515245 + 12345;
            w[i] = static_cast<double>((seed >> 16) & 0x7FFF) / 32767.0;
        }
        PointParams pp = compress(w, /*block=*/32, /*rank=*/2, /*quant=*/256);
        std::vector<double> back = decompress(pp);
        const double e = rel_err(w, back);
        check(e > 0.1, "白噪声低 rank 重建误差大（高频被截断）");
    }

    std::printf("[4] 1 pB ≡ 1000 B 换算\n");
    {
        // 造一个「1 pB」规模的点集：1e9 个点 × 展开率，这里只验换算口径
        // （真实 1e9 点太占内存，用公式验证而非真跑）。
        std::vector<double> w(1024);
        for (size_t i = 0; i < w.size(); ++i)
            w[i] = i;
        PointParams pp = compress(w, 64, 8, 256);
        const double pb = point_billions(pp);
        check_near(pb, effective_billions(pp) / 1000.0, 1e-15, "pB = 有效 B / 1000");
    }

    std::printf("[5] rank 越高重建越准（超分倍率 = block/rank）\n");
    {
        std::vector<double> w(256);
        for (size_t i = 0; i < w.size(); ++i)
            w[i] = std::sin(2.0 * M_PI * i / 64.0) + 0.2 * std::cos(2.0 * M_PI * i / 4.0);

        double prev = 1e9;
        for (size_t r : {2u, 4u, 8u, 16u})
        {
            PointParams pp = compress(w, 32, r, 256);
            const double e = rel_err(w, decompress(pp));
            check(e <= prev + 1e-12, "rank 越大误差单调不增");
            prev = e;
        }
    }

    std::printf("\n%s（失败 %d 项）\n", g_fail == 0 ? "PB: 全部通过" : "PB: 失败", g_fail);
    return g_fail == 0 ? 0 : 1;
}