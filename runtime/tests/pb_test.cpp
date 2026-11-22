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
}

int main()
{
    using namespace numqk::pb;

    std::printf("[1] 平滑信号（低频流形）：低 rank 重建误差小\n");
    {
        std::vector<double> w(256);
        for (size_t i = 0; i < w.size(); ++i)
            w[i] = 3.0 * std::cos(2.0 * M_PI * i / 32.0) + 0.5 * std::cos(2.0 * M_PI * i / 16.0);

        PointParams pp = compress(w, 32, 6, 256);
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
        PointParams pp = compress(w, 64, 8, 256);

        const double ratio = storage_ratio(pp);
        check(ratio > 50.0, "存储压缩比 > 50（省内存）");
        check_near(effective_billions(pp), 100000.0 / 1e9, 1e-15, "有效容量 = 100000 / 1e9 B");
    }

    std::printf("[3] 高频信号：截断误差大（流形压缩的代价，非 bug）\n");
    {
        std::vector<double> w(128);
        unsigned seed = 12345;
        for (size_t i = 0; i < w.size(); ++i)
        {
            seed = seed * 1103515245 + 12345;
            w[i] = static_cast<double>((seed >> 16) & 0x7FFF) / 32767.0;
        }
        PointParams pp = compress(w, 32, 2, 256);
        std::vector<double> back = decompress(pp);
        const double e = rel_err(w, back);
        check(e > 0.1, "白噪声低 rank 重建误差大（高频被截断）");
    }

    std::printf("[4] 1 pB ≡ 1000 B 换算\n");
    {
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