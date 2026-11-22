#pragma once
#include <vector>
#include <cmath>
#include <algorithm>

namespace ga {
    inline double power_iteration_spectral_norm(
            const std::vector<std::vector<double>>& A,
            int max_iter = 200, double tol = 1e-12) {
        size_t m = A.size();
        if (m == 0) return 0.0;
        size_t n = A[0].size();
        if (n == 0) return 0.0;

        std::vector<double> v(n, 1.0 / std::sqrt(static_cast<double>(n)));

        for (int it = 0; it < max_iter; ++it) {
            std::vector<double> u(m, 0.0);
            for (size_t i = 0; i < m; ++i)
                for (size_t j = 0; j < n; ++j) u[i] += A[i][j] * v[j];

            std::vector<double> w(n, 0.0);
            for (size_t j = 0; j < n; ++j)
                for (size_t i = 0; i < m; ++i) w[j] += A[i][j] * u[i];

            double wn = 0.0;
            for (double x : w) wn += x * x;
            wn = std::sqrt(wn);
            if (wn < 1e-30) return 0.0;

            for (size_t j = 0; j < n; ++j) w[j] /= wn;

            double diff = 0.0;
            for (size_t j = 0; j < n; ++j) diff += (w[j] - v[j]) * (w[j] - v[j]);
            v = std::move(w);
            if (std::sqrt(diff) < tol) break;
        }

        double un = 0.0;
        for (size_t i = 0; i < m; ++i) {
            double s = 0.0;
            for (size_t j = 0; j < n; ++j) s += A[i][j] * v[j];
            un += s * s;
        }
        return std::sqrt(un);
    }

    inline std::vector<double> jacobi_eigenvalues(
            std::vector<std::vector<double>> S,
            int max_iter = 1000, double tol = 1e-12) {
        size_t n = S.size();
        std::vector<double> eig(n, 0.0);
        if (n == 0) return eig;
        if (n == 1) { eig[0] = S[0][0]; return eig; }

        for (int it = 0; it < max_iter; ++it) {
            size_t p = 0, q = 1;
            double max_off = 0.0;
            for (size_t i = 0; i < n; ++i)
                for (size_t j = i + 1; j < n; ++j)
                    if (std::abs(S[i][j]) > max_off) { max_off = std::abs(S[i][j]); p = i; q = j; }

            if (max_off < tol) break;

            double app = S[p][p], aqq = S[q][q], apq = S[p][q];
            double phi = 0.5 * std::atan2(2.0 * apq, aqq - app);
            double c = std::cos(phi), s = std::sin(phi);
            for (size_t k = 0; k < n; ++k) {
                if (k == p || k == q) continue;
                double skp = S[k][p], skq = S[k][q];
                S[k][p] = S[p][k] = c * skp - s * skq;
                S[k][q] = S[q][k] = s * skp + c * skq;
            }
            S[p][p] = c * c * app - 2.0 * s * c * apq + s * s * aqq;
            S[q][q] = s * s * app + 2.0 * s * c * apq + c * c * aqq;
            S[p][q] = S[q][p] = 0.0;
        }

        for (size_t i = 0; i < n; ++i) eig[i] = S[i][i];
        return eig;
    }

    inline double nuclear_norm(const std::vector<std::vector<double>>& A) {
        size_t m = A.size();
        if (m == 0) return 0.0;
        size_t n = A[0].size();
        if (n == 0) return 0.0;
        std::vector<std::vector<double>> G;
        if (n <= m) {
            G.assign(n, std::vector<double>(n, 0.0));
            for (size_t i = 0; i < n; ++i)
                for (size_t j = 0; j < n; ++j) {
                    double s = 0.0;
                    for (size_t k = 0; k < m; ++k) s += A[k][i] * A[k][j];
                    G[i][j] = s;
                }
        } else {
            G.assign(m, std::vector<double>(m, 0.0));
            for (size_t i = 0; i < m; ++i)
                for (size_t j = 0; j < m; ++j) {
                    double s = 0.0;
                    for (size_t k = 0; k < n; ++k) s += A[i][k] * A[j][k];
                    G[i][j] = s;
                }
        }

        auto lambda = jacobi_eigenvalues(std::move(G));
        double sum = 0.0;
        for (double l : lambda) sum += std::sqrt(std::max(0.0, l));
        return sum;
    }

    inline double icm_channel_importance(const std::vector<std::vector<double>>& R, size_t f) {
        double full = nuclear_norm(R);
        std::vector<std::vector<double>> Rm = R;
        if (f < Rm.size()) std::fill(Rm[f].begin(), Rm[f].end(), 0.0);
        return full - nuclear_norm(Rm);
    }
}