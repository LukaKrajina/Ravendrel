#pragma once
#include <vector>
#include <complex>
#include <random>
#include <stdexcept>
#include <cmath>
#include <algorithm>
#include <array>
#include <cstdint>

namespace qhal
{
    namespace mps
    {

        using Complex = std::complex<double>;
        using Mat = std::vector<Complex>;

        inline Mat matmul(const Mat& A, size_t r, size_t k, const Mat& B, size_t c)
        {
            Mat C(r * c, Complex(0.0, 0.0));
            for (size_t i = 0; i < r; ++i)
                for (size_t t = 0; t < k; ++t)
                {
                    const Complex aik = A[i * k + t];
                    if (aik == Complex(0.0, 0.0))
                        continue;
                    for (size_t j = 0; j < c; ++j)
                        C[i * c + j] += aik * B[t * c + j];
                }
            return C;
        }

        inline Mat dagger(const Mat& A, size_t r, size_t c)
        {
            Mat H(c * r);
            for (size_t i = 0; i < r; ++i)
                for (size_t j = 0; j < c; ++j)
                    H[j * r + i] = std::conj(A[i * c + j]);
            return H;
        }

        inline void jacobi_sym(const std::vector<double>& A0, size_t n, std::vector<double>& ev,
                               std::vector<double>& V)
        {
            std::vector<double> A = A0;
            V.assign(n * n, 0.0);
            for (size_t i = 0; i < n; ++i)
                V[i * n + i] = 1.0;

            for (int sweep = 0; sweep < 200; ++sweep)
            {
                double off = 0.0;
                for (size_t p = 0; p < n; ++p)
                    for (size_t q = p + 1; q < n; ++q)
                        off += A[p * n + q] * A[p * n + q];
                if (off < 1e-24)
                    break;

                for (size_t p = 0; p < n - 1; ++p)
                    for (size_t q = p + 1; q < n; ++q)
                    {
                        const double apq = A[p * n + q];
                        if (std::abs(apq) < 1e-300)
                            continue;
                        const double app = A[p * n + p], aqq = A[q * n + q];
                        const double theta = (aqq - app) / (2.0 * apq);
                        const double t = (theta >= 0.0 ? 1.0 : -1.0) /
                                         (std::abs(theta) + std::sqrt(theta * theta + 1.0));
                        const double c = 1.0 / std::sqrt(t * t + 1.0);
                        const double s = t * c;
                        const double tau = s / (1.0 + c);

                        A[p * n + p] = app - t * apq;
                        A[q * n + q] = aqq + t * apq;
                        A[p * n + q] = A[q * n + p] = 0.0;
                        for (size_t k = 0; k < n; ++k)
                        {
                            if (k == p || k == q)
                                continue;
                            const double g = A[k * n + p], h = A[k * n + q];
                            A[k * n + p] = A[p * n + k] = g - s * (h + g * tau);
                            A[k * n + q] = A[q * n + k] = h + s * (g - h * tau);
                        }
                        for (size_t k = 0; k < n; ++k)
                        {
                            const double g = V[k * n + p], h = V[k * n + q];
                            V[k * n + p] = g - s * (h + g * tau);
                            V[k * n + q] = h + s * (g - h * tau);
                        }
                    }
            }

            ev.assign(n, 0.0);
            for (size_t i = 0; i < n; ++i)
                ev[i] = A[i * n + i];
            std::vector<size_t> idx(n);
            for (size_t i = 0; i < n; ++i)
                idx[i] = i;
            std::sort(idx.begin(), idx.end(), [&](size_t a, size_t b) { return ev[a] > ev[b]; });
            std::vector<double> sev(n);
            std::vector<double> sV(n * n);
            for (size_t j = 0; j < n; ++j)
            {
                sev[j] = ev[idx[j]];
                for (size_t i = 0; i < n; ++i)
                    sV[i * n + j] = V[i * n + idx[j]];
            }
            ev = sev;
            V = sV;
        }

        inline void hermitian_eigh(const Mat& H, size_t n, std::vector<double>& ev, Mat& evec)
        {
            std::vector<double> M(4 * n * n, 0.0);
            for (size_t i = 0; i < n; ++i)
                for (size_t j = 0; j < n; ++j)
                {
                    const Complex h = H[i * n + j];
                    M[i * (2 * n) + j] = h.real();
                    M[i * (2 * n) + (j + n)] = -h.imag();
                    M[(i + n) * (2 * n) + j] = h.imag();
                    M[(i + n) * (2 * n) + (j + n)] = h.real();
                }
            std::vector<double> ev2, V2;
            jacobi_sym(M, 2 * n, ev2, V2);

            ev.assign(n, 0.0);
            evec.assign(n * n, Complex(0.0, 0.0));
            for (size_t k = 0; k < n; ++k)
            {
                ev[k] = ev2[2 * k];
                for (size_t i = 0; i < n; ++i)
                    evec[k * n + i] =
                        Complex(V2[i * (2 * n) + 2 * k], V2[(i + n) * (2 * n) + 2 * k]);
            }
        }

        inline void svd(const Mat& M, size_t m, size_t n, Mat& U, std::vector<double>& s, Mat& Vt)
        {
            const size_t k = std::min(m, n);
            Mat A = M;
            Mat V(n * n, Complex(0.0, 0.0));
            for (size_t i = 0; i < n; ++i)
                V[i * n + i] = Complex(1.0, 0.0);

            for (int sweep = 0; sweep < 120; ++sweep)
            {
                double off = 0.0;
                for (size_t p = 0; p + 1 < n; ++p)
                    for (size_t q = p + 1; q < n; ++q)
                    {
                        Complex app(0, 0), aqq(0, 0), apq(0, 0);
                        for (size_t i = 0; i < m; ++i)
                        {
                            const Complex ap = A[i * n + p], aq = A[i * n + q];
                            app += std::conj(ap) * ap;
                            aqq += std::conj(aq) * aq;
                            apq += std::conj(ap) * aq;
                        }
                        off += std::norm(apq);
                        if (std::norm(apq) < 1e-30)
                            continue;

                        const double theta = std::arg(apq);
                        const Complex e = Complex(std::cos(theta), -std::sin(theta));
                        const double phi =
                            0.5 * std::atan2(2.0 * std::abs(apq), app.real() - aqq.real());
                        const double c = std::cos(phi), sn = std::sin(phi);

                        for (size_t i = 0; i < m; ++i)
                        {
                            const Complex ap = A[i * n + p], aq = A[i * n + q];
                            A[i * n + p] = ap * c + aq * e * sn;
                            A[i * n + q] = -ap * std::conj(e) * sn + aq * c;
                        }
                        for (size_t i = 0; i < n; ++i)
                        {
                            const Complex vp = V[i * n + p], vq = V[i * n + q];
                            V[i * n + p] = vp * c + vq * e * sn;
                            V[i * n + q] = -vp * std::conj(e) * sn + vq * c;
                        }
                    }
                if (off < 1e-24)
                    break;
            }

            std::vector<double> sv(n, 0.0);
            for (size_t j = 0; j < n; ++j)
            {
                double acc = 0.0;
                for (size_t i = 0; i < m; ++i)
                    acc += std::norm(A[i * n + j]);
                sv[j] = std::sqrt(acc);
            }

            std::vector<size_t> idx(n);
            for (size_t i = 0; i < n; ++i)
                idx[i] = i;
            std::sort(idx.begin(), idx.end(), [&](size_t a, size_t b) { return sv[a] > sv[b]; });

            s.assign(k, 0.0);
            U.assign(m * m, Complex(0.0, 0.0));
            Mat Vsorted(n * n, Complex(0.0, 0.0));
            for (size_t j = 0; j < n; ++j)
            {
                const size_t src = idx[j];
                if (j < k)
                    s[j] = sv[src];
                for (size_t i = 0; i < n; ++i)
                    Vsorted[i * n + j] = V[i * n + src];
                if (sv[src] > 1e-15)
                    for (size_t i = 0; i < m; ++i)
                        U[i * m + j] = A[i * n + src] / sv[src];
            }
            Vt = dagger(Vsorted, n, n);
        }

        class MpsState
        {
            size_t n_ = 0;
            size_t chi_ = 16;
            std::vector<std::array<Mat, 2>> A_;
            std::vector<size_t> dims_;

          public:
            MpsState() = default;
            MpsState(size_t n, size_t chi) : n_(n), chi_(chi)
            {
                dims_.assign(n + 1, 1);
                if (n == 0)
                    return;
                A_.resize(n);
                for (size_t q = 0; q < n; ++q)
                {
                    A_[q][0] = Mat(1, Complex(1.0, 0.0));
                    A_[q][1] = Mat(1, Complex(0.0, 0.0));
                }
            }

            size_t num_qubits() const
            {
                return n_;
            }

            void append_qubits(size_t m)
            {
                for (size_t i = 0; i < m; ++i)
                {
                    A_.push_back({Mat(1, Complex(1.0, 0.0)), Mat(1, Complex(0.0, 0.0))});
                    dims_.push_back(1);
                }
                n_ += m;
            }

            void drop_qubits(size_t m)
            {
                while (m > 0 && n_ > 0)
                {
                    const size_t q = n_ - 1;
                    if (q > 0)
                    {
                        const Mat& v = A_[q][0];
                        const size_t dl = dims_[q - 1];
                        const size_t dr = dims_[q];
                        for (int s = 0; s < 2; ++s)
                        {
                            Mat& B = A_[q - 1][s];
                            Mat C(dl, Complex(0.0, 0.0));
                            for (size_t a = 0; a < dl; ++a)
                            {
                                Complex acc(0.0, 0.0);
                                for (size_t b = 0; b < dr; ++b)
                                    acc += B[a * dr + b] * v[b];
                                C[a] = acc;
                            }
                            B = std::move(C);
                        }
                        dims_[q] = 1;
                    }
                    A_.pop_back();
                    dims_.pop_back();
                    n_ -= 1;
                    --m;
                }
            }

            static MpsState from_amps(const std::vector<Complex>& amps, size_t n, size_t chi)
            {
                MpsState m;
                m.n_ = n;
                m.chi_ = chi;
                m.dims_.assign(n + 1, 1);
                m.A_.reserve(n);

                std::vector<Complex> cur = amps;
                size_t left = 1;
                size_t remaining = size_t(1) << n;

                for (size_t q = 0; q < n; ++q)
                {
                    const size_t half = remaining >> 1;
                    Mat M(left * 2 * half, Complex(0.0, 0.0));
                    for (size_t s = 0; s < 2; ++s)
                        for (size_t a = 0; a < left; ++a)
                            for (size_t b = 0; b < half; ++b)
                                M[(s * left + a) * half + b] = cur[a * remaining + b * 2 + s];

                    Mat U, Vt;
                    std::vector<double> sv;
                    svd(M, left * 2, half, U, sv, Vt);
                    const size_t k = std::min<size_t>(chi, sv.size());

                    Mat A0(left * k, Complex(0.0, 0.0)), A1(left * k, Complex(0.0, 0.0));
                    for (size_t c = 0; c < k; ++c)
                    {
                        for (size_t a = 0; a < left; ++a)
                        {
                            A0[a * k + c] = U[(0 * left + a) * (left * 2) + c];
                            A1[a * k + c] = U[(1 * left + a) * (left * 2) + c];
                        }
                    }
                    m.A_.push_back({std::move(A0), std::move(A1)});
                    m.dims_[q + 1] = k;

                    Mat nxt(k * half, Complex(0.0, 0.0));
                    for (size_t c = 0; c < k; ++c)
                        for (size_t b = 0; b < half; ++b)
                            nxt[c * half + b] = Vt[c * half + b] * sv[c];
                    cur = std::move(nxt);
                    left = k;
                    remaining = half;
                }
                return m;
            }

            void apply_single(size_t q, const Complex U[4])
            {
                if (q >= n_ || q + 1 >= dims_.size())
                    throw std::out_of_range("apply_single: qubit site out of range");
                const size_t dl = dims_[q], dr = dims_[q + 1];
                Mat a0 = A_[q][0], a1 = A_[q][1];
                Mat b0(dl * dr, Complex(0.0, 0.0)), b1(dl * dr, Complex(0.0, 0.0));
                for (size_t i = 0; i < dl * dr; ++i)
                {
                    b0[i] = U[0] * a0[i] + U[2] * a1[i];
                    b1[i] = U[1] * a0[i] + U[3] * a1[i];
                }
                A_[q][0] = std::move(b0);
                A_[q][1] = std::move(b1);
            }

            void apply_two(size_t q, const Complex U[16])
            {
                const size_t dl = dims_[q], dm = dims_[q + 1], dr = dims_[q + 2];
                const size_t R = 2 * dl, C = 2 * dr;
                Mat Theta(R * C, Complex(0.0, 0.0));
                for (size_t s1 = 0; s1 < 2; ++s1)
                    for (size_t s2 = 0; s2 < 2; ++s2)
                    {
                        const Mat& L = A_[q][s1];
                        const Mat& Rg = A_[q + 1][s2];
                        for (size_t a = 0; a < dl; ++a)
                            for (size_t b = 0; b < dr; ++b)
                            {
                                Complex acc(0.0, 0.0);
                                for (size_t c = 0; c < dm; ++c)
                                    acc += L[a * dm + c] * Rg[c * dr + b];
                                Theta[(s1 * dl + a) * C + (s2 * dr + b)] = acc;
                            }
                    }

                Mat Theta2(R * C, Complex(0.0, 0.0));
                for (size_t s1p = 0; s1p < 2; ++s1p)
                    for (size_t s2p = 0; s2p < 2; ++s2p)
                        for (size_t s1 = 0; s1 < 2; ++s1)
                            for (size_t s2 = 0; s2 < 2; ++s2)
                            {
                                const Complex u = U[(s1p * 2 + s2p) * 4 + (s1 * 2 + s2)];
                                if (u == Complex(0.0, 0.0))
                                    continue;
                                for (size_t a = 0; a < dl; ++a)
                                    for (size_t b = 0; b < dr; ++b)
                                        Theta2[(s1p * dl + a) * C + (s2p * dr + b)] +=
                                            u * Theta[(s1 * dl + a) * C + (s2 * dr + b)];
                            }

                Mat Uu, Vt;
                std::vector<double> sv;
                svd(Theta2, R, C, Uu, sv, Vt);
                size_t newchi =
                    std::min<size_t>({chi_, sv.size(),
                                      (size_t)std::count_if(sv.begin(), sv.end(),
                                                            [](double x) { return x > 1e-12; })});

                Mat L0(dl * newchi, Complex(0.0, 0.0)), L1(dl * newchi, Complex(0.0, 0.0));
                Mat R0(newchi * dr, Complex(0.0, 0.0)), R1(newchi * dr, Complex(0.0, 0.0));
                for (size_t c = 0; c < newchi; ++c)
                {
                    const double sc = sv[c];
                    for (size_t a = 0; a < dl; ++a)
                    {
                        L0[a * newchi + c] = Uu[(0 * dl + a) * R + c] * sc;
                        L1[a * newchi + c] = Uu[(1 * dl + a) * R + c] * sc;
                    }
                    for (size_t b = 0; b < dr; ++b)
                    {
                        R0[c * dr + b] = Vt[c * C + (0 * dr + b)];
                        R1[c * dr + b] = Vt[c * C + (1 * dr + b)];
                    }
                }
                A_[q][0] = std::move(L0);
                A_[q][1] = std::move(L1);
                A_[q + 1][0] = std::move(R0);
                A_[q + 1][1] = std::move(R1);
                dims_[q + 1] = newchi;
            }

            double expectation_z(size_t q) const
            {
                return 1.0 - 2.0 * prob_one(q);
            }

            double prob_one(size_t q) const
            {
                Mat L(1, Complex(1.0, 0.0));
                for (size_t i = 0; i < q; ++i)
                {
                    const size_t dl = dims_[i], dr = dims_[i + 1];
                    Mat N(dr, Complex(0.0, 0.0));
                    for (size_t s = 0; s < 2; ++s)
                    {
                        const Mat& A = A_[i][s];
                        for (size_t a = 0; a < dl; ++a)
                            for (size_t b = 0; b < dr; ++b)
                                N[b] += L[a] * A[a * dr + b];
                    }
                    L = std::move(N);
                }
                Mat R(dims_[n_], Complex(1.0, 0.0));
                for (size_t i = n_; i-- > q + 1;)
                {
                    const size_t dl = dims_[i], dr = dims_[i + 1];
                    Mat N(dl, Complex(0.0, 0.0));
                    for (size_t s = 0; s < 2; ++s)
                    {
                        const Mat& A = A_[i][s];
                        for (size_t a = 0; a < dl; ++a)
                            for (size_t b = 0; b < dr; ++b)
                                N[a] += A[a * dr + b] * R[b];
                    }
                    R = std::move(N);
                }
                const size_t dl = dims_[q], dr = dims_[q + 1];
                const Mat& A1 = A_[q][1];
                Complex acc(0.0, 0.0);
                for (size_t a = 0; a < dl; ++a)
                    for (size_t b = 0; b < dr; ++b)
                        acc += L[a] * A1[a * dr + b] * R[b];
                return std::norm(acc);
            }

            int measure(size_t q, std::mt19937& rng)
            {
                const double p1 = prob_one(q);
                std::uniform_real_distribution<double> dist(0.0, 1.0);
                const int outcome = (dist(rng) < p1) ? 1 : 0;
                const double norm = outcome ? std::sqrt(p1) : std::sqrt(1.0 - p1);
                if (norm < 1e-15)
                {
                    return outcome;
                }
                for (size_t i = 0; i < A_[q][1 - outcome].size(); ++i)
                    A_[q][1 - outcome][i] = Complex(0.0, 0.0);
                for (size_t i = 0; i < A_[q][outcome].size(); ++i)
                    A_[q][outcome][i] /= norm;
                return outcome;
            }

            std::vector<Complex> get_state_vector() const
            {
                const size_t N = size_t(1) << n_;
                std::vector<Complex> out(N, Complex(0.0, 0.0));
                for (size_t idx = 0; idx < N; ++idx)
                {
                    Mat cur(1, Complex(1.0, 0.0));
                    size_t cols = 1;
                    for (size_t q = 0; q < n_; ++q)
                    {
                        const int s = (idx >> q) & 1;
                        const Mat& A = A_[q][s];
                        const size_t dr = dims_[q + 1];
                        Mat nxt(dr, Complex(0.0, 0.0));
                        for (size_t a = 0; a < cols; ++a)
                            for (size_t b = 0; b < dr; ++b)
                                nxt[b] += cur[a] * A[a * dr + b];
                        cur = std::move(nxt);
                        cols = dr;
                    }
                    out[idx] = cur[0];
                }
                return out;
            }
        };
    }
}