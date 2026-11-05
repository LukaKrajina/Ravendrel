#include "../include/qhal/r/AtomicVaporORAM.hpp"

namespace qhal
{
    namespace oram_detail
    {
        bool invert_matrix(std::vector<std::vector<double>> a,
                        std::vector<std::vector<double>> &inv)
        {
            size_t n = a.size();
            if (n == 0 || a[0].size() != n)
                return false;

            inv.assign(n, std::vector<double>(n, 0.0));
            for (size_t i = 0; i < n; ++i)
                inv[i][i] = 1.0;

            for (size_t col = 0; col < n; ++col)
            {
                size_t piv = col;
                double maxv = std::abs(a[col][col]);
                for (size_t r = col + 1; r < n; ++r)
                {
                    if (std::abs(a[r][col]) > maxv)
                    {
                        maxv = std::abs(a[r][col]);
                        piv = r;
                    }
                }
                if (maxv < 1e-12)
                    return false;

                std::swap(a[col], a[piv]);
                std::swap(inv[col], inv[piv]);

                double div = a[col][col];
                for (size_t j = 0; j < n; ++j)
                {
                    a[col][j] /= div;
                    inv[col][j] /= div;
                }

                for (size_t r = 0; r < n; ++r)
                {
                    if (r == col)
                        continue;
                    double factor = a[r][col];
                    if (factor == 0.0)
                        continue;
                    for (size_t j = 0; j < n; ++j)
                    {
                        a[r][j] -= factor * a[col][j];
                        inv[r][j] -= factor * inv[col][j];
                    }
                }
            }
            return true;
        }

        std::vector<double> jacobi_eigenvalues(std::vector<std::vector<double>> A,
                                            size_t max_iter,
                                            double tol)
        {
            size_t n = A.size();
            if (n == 0)
                return {};

            for (size_t iter = 0; iter < max_iter; ++iter)
            {
                size_t p = 0, q = 1;
                double max_off = 0.0;
                for (size_t i = 0; i < n; ++i)
                {
                    for (size_t j = i + 1; j < n; ++j)
                    {
                        double v = std::abs(A[i][j]);
                        if (v > max_off)
                        {
                            max_off = v;
                            p = i;
                            q = j;
                        }
                    }
                }
                if (max_off < tol)
                    break;

                double app = A[p][p], aqq = A[q][q], apq = A[p][q];
                double theta = 0.5 * std::atan2(2.0 * apq, aqq - app);
                double c = std::cos(theta), s = std::sin(theta);

                for (size_t k = 0; k < n; ++k)
                {
                    if (k == p || k == q)
                        continue;
                    double akp = A[k][p], akq = A[k][q];
                    A[k][p] = A[p][k] = c * akp - s * akq;
                    A[k][q] = A[q][k] = s * akp + c * akq;
                }
                A[p][p] = c * c * app - 2.0 * s * c * apq + s * s * aqq;
                A[q][q] = s * s * app + 2.0 * s * c * apq + c * c * aqq;
                A[p][q] = A[q][p] = 0.0;
            }

            std::vector<double> eigs(n);
            for (size_t i = 0; i < n; ++i)
                eigs[i] = A[i][i];
            std::sort(eigs.begin(), eigs.end(), std::greater<double>());
            return eigs;
        }
    }

    std::vector<double> AtomicVaporORAM::ridge_regression(
        const std::vector<std::vector<double>> &S,
        const std::vector<double> &y,
        double lambda_reg)
    {
        size_t n_rows = S.size();
        if (n_rows == 0 || S[0].size() == 0 || y.size() != n_rows)
            return {};
        size_t n_cols = S[0].size();

        std::vector<std::vector<double>> C(n_cols, std::vector<double>(n_cols, 0.0));
        std::vector<double> b(n_cols, 0.0);
        for (size_t i = 0; i < n_cols; ++i)
        {
            for (size_t k = 0; k < n_rows; ++k)
                b[i] += S[k][i] * y[k];
            for (size_t j = 0; j < n_cols; ++j)
            {
                double s = 0.0;
                for (size_t k = 0; k < n_rows; ++k)
                    s += S[k][i] * S[k][j];
                C[i][j] = s + ((i == j) ? lambda_reg : 0.0);
            }
        }

        std::vector<std::vector<double>> Cinv;
        if (!oram_detail::invert_matrix(C, Cinv))
            return {};

        std::vector<double> w(n_cols, 0.0);
        for (size_t i = 0; i < n_cols; ++i)
            for (size_t j = 0; j < n_cols; ++j)
                w[i] += Cinv[i][j] * b[j];
        return w;
    }

    double AtomicVaporORAM::linear_memory_capacity(
        const std::vector<std::vector<double>> &S,
        const std::vector<double> &u,
        size_t n_lim,
        double lambda_reg)
    {
        size_t N = S.size();
        double mc = 0.0;
        for (size_t d = 1; d <= n_lim; ++d)
        {
            std::vector<double> y(N, 0.0);
            for (size_t t = d; t < N; ++t)
                y[t] = u[t - d];

            std::vector<double> w = ridge_regression(S, y, lambda_reg);
            if (w.empty())
                continue;

            double mean = 0.0;
            for (double v : y)
                mean += v;
            mean /= static_cast<double>(N);

            double var = 0.0, sse = 0.0;
            for (size_t t = 0; t < N; ++t)
            {
                double pred = 0.0;
                for (size_t c = 0; c < w.size(); ++c)
                    pred += S[t][c] * w[c];
                double e = y[t] - pred;
                sse += e * e;
                var += (y[t] - mean) * (y[t] - mean);
            }

            if (var > 1e-12)
                mc += std::max(0.0, 1.0 - sse / var);
        }
        return mc;
    }

    double AtomicVaporORAM::kernel_rank(
        const std::vector<std::vector<double>> &S,
        double threshold_frac)
    {
        size_t n_rows = S.size();
        if (n_rows == 0 || S[0].size() == 0)
            return 0.0;
        size_t n_cols = S[0].size();

        std::vector<std::vector<double>> C(n_cols, std::vector<double>(n_cols, 0.0));
        for (size_t i = 0; i < n_cols; ++i)
        {
            for (size_t j = 0; j < n_cols; ++j)
            {
                double s = 0.0;
                for (size_t k = 0; k < n_rows; ++k)
                    s += S[k][i] * S[k][j];
                C[i][j] = s;
            }
        }

        auto eigs = oram_detail::jacobi_eigenvalues(C);
        double max_sv = 0.0;
        std::vector<double> sv;
        sv.reserve(eigs.size());
        for (double e : eigs)
        {
            double v = std::sqrt(std::max(0.0, e));
            sv.push_back(v);
            max_sv = std::max(max_sv, v);
        }

        double threshold = threshold_frac * max_sv;
        double rank = 0.0;
        for (double v : sv)
            if (v > threshold)
                rank += 1.0;
        return rank;
    }

    double AtomicVaporORAM::xor_bit_error_rate(
        const std::vector<double> &predictions,
        const std::vector<int> &truth)
    {
        size_t n = std::min(predictions.size(), truth.size());
        if (n == 0)
            return 1.0;
        size_t wrong = 0;
        for (size_t i = 0; i < n; ++i)
        {
            int pred = (predictions[i] >= 0.5) ? 1 : 0;
            if (pred != truth[i])
                ++wrong;
        }
        return static_cast<double>(wrong) / static_cast<double>(n);
    }
}
