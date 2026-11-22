#pragma once
#include <vector>
#include <complex>
#include <cmath>
#include <algorithm>
#include <limits>

namespace qhal::health
{
    inline std::vector<double> sym_eigenvalues(std::vector<std::vector<double>> A)
    {
        const int n = static_cast<int>(A.size());
        const double eps = 1e-12;
        for (int sweep = 0; sweep < 50; ++sweep)
        {
            double off = 0.0;
            for (int i = 0; i < n; ++i)
                for (int j = i + 1; j < n; ++j)
                    off += A[i][j] * A[i][j];
            if (std::sqrt(off) < eps)
                break;
            for (int p = 0; p < n - 1; ++p)
                for (int q = p + 1; q < n; ++q)
                {
                    if (std::abs(A[p][q]) < eps)
                        continue;
                    double theta = (A[q][q] - A[p][p]) / (2 * A[p][q]);
                    double t = (theta >= 0 ? 1.0 : -1.0) /
                               (std::abs(theta) + std::sqrt(theta * theta + 1));
                    double c = 1.0 / std::sqrt(t * t + 1), s = t * c;
                    double app = A[p][p], aqq = A[q][q], apq = A[p][q];
                    A[p][p] = c * c * app - 2 * s * c * apq + s * s * aqq;
                    A[q][q] = s * s * app + 2 * s * c * apq + c * c * aqq;
                    A[p][q] = A[q][p] = 0.0;
                    for (int k = 0; k < n; ++k)
                    {
                        if (k == p || k == q)
                            continue;
                        double akp = A[k][p], akq = A[k][q];
                        A[k][p] = A[p][k] = c * akp - s * akq;
                        A[k][q] = A[q][k] = s * akp + c * akq;
                    }
                }
        }
        std::vector<double> eig(n);
        for (int i = 0; i < n; ++i)
            eig[i] = A[i][i];
        std::sort(eig.begin(), eig.end());
        return eig;
    }

    inline double condition_number(const std::vector<std::vector<double>> &A)
    {
        if (A.empty() || A[0].empty())
            return 0.0;
        int m = static_cast<int>(A.size()), n = static_cast<int>(A[0].size());
        std::vector<std::vector<double>> ATA(n, std::vector<double>(n, 0.0));
        for (int i = 0; i < n; ++i)
            for (int j = 0; j < n; ++j)
                for (int k = 0; k < m; ++k)
                    ATA[i][j] += A[k][i] * A[k][j];
        auto eig = sym_eigenvalues(ATA);
        double smax = std::sqrt(std::max(eig.back(), 0.0));
        double smin = std::sqrt(std::max(eig.front(), 0.0));
        if (smin < 1e-15)
            return std::numeric_limits<double>::infinity();
        return smax / smin;
    }

    inline double normalization_residual(const std::vector<std::complex<double>> &state)
    {
        double n2 = 0.0;
        for (const auto &c : state)
            n2 += std::norm(c);
        return std::abs(1.0 - std::sqrt(n2));
    }

    struct NumericHealth
    {
        double normalization_residual = 0.0;
        double condition_number = 1.0;
        bool healthy = true;
    };
}