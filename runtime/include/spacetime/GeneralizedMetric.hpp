#pragma once
#include <vector>
#include <cmath>
#include <algorithm>

namespace rvdl::spacetime
{

    // 广义度规双空间结构设想于快子
    struct GeneralizedMetric
    {
        int D = 1;
        std::vector<double> g;   // D×D 度规
        std::vector<double> B;   // D×D 反对称 B 场
        std::vector<double> H;   // 2D×2D 广义度规
        std::vector<double> eta; // 2D×2D O(D,D) 不变度规

        GeneralizedMetric(int dim) : D(dim)
        {
            g.assign(static_cast<size_t>(D) * D, 0.0);
            B.assign(static_cast<size_t>(D) * D, 0.0);
            eta.assign(static_cast<size_t>(2 * D) * 2 * D, 0.0);
            for (int i = 0; i < D; ++i)
            {
                eta[static_cast<size_t>(i) * 2 * D + (D + i)] = 1.0;
                eta[static_cast<size_t>(D + i) * 2 * D + i] = 1.0;
            }
        }

        // 设置度规为对角形式（diag 为 D 个对角元），B = 0
        void set_diagonal(const std::vector<double> &diag)
        {
            for (int i = 0; i < D; ++i)
                g[static_cast<size_t>(i) * D + i] = diag[i];
        }

        // 构造广义度规 H（需要 g 可逆）
        void build()
        {
            std::vector<double> ginv(static_cast<size_t>(D) * D, 0.0);
            invert(g, ginv);

            H.assign(static_cast<size_t>(2 * D) * 2 * D, 0.0);

            for (int i = 0; i < D; ++i)
                for (int j = 0; j < D; ++j)
                {
                    double bg = 0.0;
                    double gb = 0.0;
                    double bgb = 0.0;
                    for (int k = 0; k < D; ++k)
                    {
                        bg += B[static_cast<size_t>(i) * D + k] * ginv[static_cast<size_t>(k) * D + j];
                        gb += ginv[static_cast<size_t>(i) * D + k] * B[static_cast<size_t>(k) * D + j];
                        for (int l = 0; l < D; ++l)
                            bgb += B[static_cast<size_t>(i) * D + k] * ginv[static_cast<size_t>(k) * D + l] *
                                   B[static_cast<size_t>(l) * D + j];
                    }

                    H[static_cast<size_t>(i) * 2 * D + j] = g[static_cast<size_t>(i) * D + j] - bgb;
                    H[static_cast<size_t>(i) * 2 * D + (D + j)] = bg;
                    H[static_cast<size_t>(D + i) * 2 * D + j] = -gb;
                    H[static_cast<size_t>(D + i) * 2 * D + (D + j)] = ginv[static_cast<size_t>(i) * D + j];
                }
        }

        // O(D,D) 约束残差
        // ‖H η H - η‖（Frobenius），它应 ≈ 0
        double o_dd_residual() const
        {
            // 计算 H η H
            std::vector<double> Heta(static_cast<size_t>(2 * D) * 2 * D, 0.0);
            std::vector<double> HetaH(static_cast<size_t>(2 * D) * 2 * D, 0.0);
            matmul(H, eta, Heta);
            matmul(Heta, H, HetaH);
            double s = 0.0;
            for (size_t p = 0; p < eta.size(); ++p)
            {
                double d = HetaH[p] - eta[p];
                s += d * d;
            }
            return std::sqrt(s);
        }

        // section condition 残差
        // 对一个双坐标场 Φ(x, x̃)，数值验证 η^{MN} ∂_M ∂_N Φ = 0。
        // 这里演示：假设 Φ 只依赖 x 不依赖 x̃，那么 section condition 自动满足。
        double section_condition_residual(const std::vector<double> &dM,
                                          const std::vector<double> &dN) const
        {
            double s = 0.0;
            for (int M = 0; M < 2 * D; ++M)
                for (int N = 0; N < 2 * D; ++N)
                    s += eta[static_cast<size_t>(M) * 2 * D + N] * dM[M] * dN[N];
            return std::abs(s);
        }

        // T-对偶演示：环面半径 R → α'/R（取 α'=1），给出对偶半径与对偶度规。
        double dual_radius(double R) const { return 1.0 / R; }

    private:
        // D×D 矩阵求逆（Gauss–Jordan，D 较小**）
        static void invert(const std::vector<double> &m, std::vector<double> &out)
        {
            int n = static_cast<int>(std::sqrt(static_cast<double>(m.size())));
            std::vector<double> a = m;
            out.assign(static_cast<size_t>(n) * n, 0.0);
            for (int i = 0; i < n; ++i)
                out[static_cast<size_t>(i) * n + i] = 1.0;

            for (int col = 0; col < n; ++col)
            {
                int piv = col;
                for (int r = col + 1; r < n; ++r)
                    if (std::abs(a[static_cast<size_t>(r) * n + col]) >
                        std::abs(a[static_cast<size_t>(piv) * n + col]))
                        piv = r;
                for (int c = 0; c < n; ++c)
                {
                    std::swap(a[static_cast<size_t>(col) * n + c], a[static_cast<size_t>(piv) * n + c]);
                    std::swap(out[static_cast<size_t>(col) * n + c], out[static_cast<size_t>(piv) * n + c]);
                }
                double pv = a[static_cast<size_t>(col) * n + col];
                for (int c = 0; c < n; ++c)
                {
                    a[static_cast<size_t>(col) * n + c] /= pv;
                    out[static_cast<size_t>(col) * n + c] /= pv;
                }
                for (int r = 0; r < n; ++r)
                {
                    if (r == col)
                        continue;
                    double factor = a[static_cast<size_t>(r) * n + col];
                    for (int c = 0; c < n; ++c)
                    {
                        a[static_cast<size_t>(r) * n + c] -= factor * a[static_cast<size_t>(col) * n + c];
                        out[static_cast<size_t>(r) * n + c] -= factor * out[static_cast<size_t>(col) * n + c];
                    }
                }
            }
        }

        static void matmul(const std::vector<double> &A, const std::vector<double> &B,
                           std::vector<double> &C)
        {
            int n = static_cast<int>(std::sqrt(static_cast<double>(A.size())));
            C.assign(static_cast<size_t>(n) * n, 0.0);
            for (int i = 0; i < n; ++i)
                for (int j = 0; j < n; ++j)
                {
                    double s = 0.0;
                    for (int k = 0; k < n; ++k)
                        s += A[static_cast<size_t>(i) * n + k] * B[static_cast<size_t>(k) * n + j];
                    C[static_cast<size_t>(i) * n + j] = s;
                }
        }
    };
}