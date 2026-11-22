#pragma once
#include "Numqk.hpp"
#include <complex>
#include <vector>
#include <cmath>
#include <algorithm>
#include <stdexcept>

namespace numqk
{

    using Complex = std::complex<double>;
    using CTensor = Tensor<Complex>;

    inline double re(Complex z) { return z.real(); }
    inline double im(Complex z) { return z.imag(); }

    template <typename T>
    Tensor<T> dagger(const Tensor<T> &M)
    {
        const auto &s = M.get_shape();
        if (s.size() != 2)
            throw std::invalid_argument("dagger: requires a 2D tensor.");
        Tensor<T> out({s[1], s[0]}, false);
        for (size_t i = 0; i < s[0]; ++i)
            for (size_t j = 0; j < s[1]; ++j)
                out.data()[j * s[0] + i] = std::conj(M.data()[i * s[1] + j]);
        return out;
    }

    template <typename T>
    T trace(const Tensor<T> &M)
    {
        const auto &s = M.get_shape();
        if (s.size() != 2 || s[0] != s[1])
            throw std::invalid_argument("trace: requires a square matrix.");
        T t(0);
        for (size_t i = 0; i < s[0]; ++i)
            t += M.data()[i * s[0] + i];
        return t;
    }

    template <typename T>
    Tensor<T> kron(const Tensor<T> &A, const Tensor<T> &B)
    {
        const auto &a = A.get_shape();
        const auto &b = B.get_shape();
        if (a.empty() || b.empty() || a.size() > 2 || b.size() > 2)
            throw std::invalid_argument("kron: operands must be 1D or 2D.");

        const bool a_vec = (a.size() == 1);
        const bool b_vec = (b.size() == 1);
        const size_t m = a[0];
        const size_t n = a_vec ? 1 : a[1];
        const size_t p = b[0];
        const size_t q = b_vec ? 1 : b[1];

        Tensor<T> out(a_vec && b_vec ? std::vector<size_t>{m * p}
                                     : std::vector<size_t>{m * p, n * q},
                      false);
        for (size_t i = 0; i < m; ++i)
            for (size_t j = 0; j < n; ++j)
                for (size_t k = 0; k < p; ++k)
                    for (size_t l = 0; l < q; ++l)
                        out.data()[(i * p + k) * (n * q) + (j * q + l)] =
                            A.data()[i * n + j] * B.data()[k * q + l];
        return out;
    }

    inline std::vector<size_t> mixed_radix_decode(size_t idx, const std::vector<size_t> &radices)
    {
        std::vector<size_t> digits(radices.size());
        for (size_t i = radices.size(); i-- > 0;)
        {
            digits[i] = idx % radices[i];
            idx /= radices[i];
        }
        return digits;
    }
    inline size_t mixed_radix_encode(const std::vector<size_t> &digits, const std::vector<size_t> &radices)
    {
        size_t idx = 0;
        for (size_t i = 0; i < radices.size(); ++i)
            idx = idx * radices[i] + digits[i];
        return idx;
    }

    template <typename T>
    Tensor<T> partial_trace(const Tensor<T> &rho, const std::vector<size_t> &dims,
                            const std::vector<bool> &keep)
    {
        const size_t nsys = dims.size();
        if (keep.size() != nsys)
            throw std::invalid_argument("partial_trace: keep mask length mismatch.");
        size_t D = 1;
        for (size_t d : dims)
            D *= d;
        const auto &s = rho.get_shape();
        if (s.size() != 2 || s[0] != D || s[1] != D)
            throw std::invalid_argument("partial_trace: rho shape incompatible with dims.");

        std::vector<size_t> kdims;
        for (size_t i = 0; i < nsys; ++i)
            if (keep[i])
                kdims.push_back(dims[i]);
        size_t DK = 1;
        for (size_t d : kdims)
            DK *= d;

        Tensor<T> out({DK, DK}, false);
        for (size_t r = 0; r < D; ++r)
        {
            auto rd = mixed_radix_decode(r, dims);
            for (size_t c = 0; c < D; ++c)
            {
                auto cd = mixed_radix_decode(c, dims);
                bool match = true;
                for (size_t s2 = 0; s2 < nsys; ++s2)
                    if (!keep[s2] && rd[s2] != cd[s2])
                    {
                        match = false;
                        break;
                    }
                if (!match)
                    continue;
                std::vector<size_t> kr, kc;
                for (size_t s2 = 0; s2 < nsys; ++s2)
                    if (keep[s2])
                    {
                        kr.push_back(rd[s2]);
                        kc.push_back(cd[s2]);
                    }
                out.data()[mixed_radix_encode(kr, kdims) * DK + mixed_radix_encode(kc, kdims)] += rho.data()[r * D + c];
            }
        }
        return out;
    }

    inline CTensor partial_trace_pure(const CTensor &ket, const std::vector<size_t> &dims,
                                      const std::vector<bool> &keep)
    {
        const size_t nsys = dims.size();
        if (keep.size() != nsys)
            throw std::invalid_argument("partial_trace_pure: keep mask length mismatch.");
        size_t D = 1;
        for (size_t d : dims)
            D *= d;
        if (ket.size() != D)
            throw std::invalid_argument("partial_trace_pure: ket size incompatible with dims.");

        std::vector<size_t> kdims, tdims;
        for (size_t i = 0; i < nsys; ++i)
            (keep[i] ? kdims : tdims).push_back(dims[i]);
        size_t DK = 1;
        for (size_t d : kdims)
            DK *= d;
        size_t DT = D / DK;

        CTensor rho({DK, DK}, false);
        for (size_t b = 0; b < DT; ++b)
        {
            auto bd = mixed_radix_decode(b, tdims);
            std::vector<Complex> v(DK);
            for (size_t a = 0; a < DK; ++a)
            {
                auto ad = mixed_radix_decode(a, kdims);
                std::vector<size_t> gd(nsys);
                size_t ai = 0, bi = 0;
                for (size_t i = 0; i < nsys; ++i)
                    gd[i] = keep[i] ? ad[ai++] : bd[bi++];
                v[a] = ket.data()[mixed_radix_encode(gd, dims)];
            }
            for (size_t a = 0; a < DK; ++a)
                for (size_t a2 = 0; a2 < DK; ++a2)
                    rho.data()[a * DK + a2] += v[a] * std::conj(v[a2]);
        }
        return rho;
    }

    inline Complex inner(const CTensor &a, const CTensor &b)
    {
        if (a.size() != b.size())
            throw std::invalid_argument("inner: vector size mismatch.");
        Complex s(0, 0);
        for (size_t i = 0; i < a.size(); ++i)
            s += std::conj(a.data()[i]) * b.data()[i];
        return s;
    }

    inline CTensor outer(const CTensor &a, const CTensor &b)
    {
        const size_t m = a.size(), n = b.size();
        CTensor out({m, n}, false);
        for (size_t i = 0; i < m; ++i)
            for (size_t j = 0; j < n; ++j)
                out.data()[i * n + j] = a.data()[i] * std::conj(b.data()[j]);
        return out;
    }

    inline Complex expectation(const CTensor &O, const CTensor &psi)
    {
        const auto &s = O.get_shape();
        if (s.size() != 2 || s[0] != s[1] || s[0] != psi.size())
            throw std::invalid_argument("expectation: O must be n×n and match psi size.");
        const size_t n = s[0];
        Complex acc(0, 0);
        for (size_t i = 0; i < n; ++i)
        {
            Complex Opsi(0, 0);
            for (size_t j = 0; j < n; ++j)
                Opsi += O.data()[i * n + j] * psi.data()[j];
            acc += std::conj(psi.data()[i]) * Opsi;
        }
        return acc;
    }

    inline void jacobi_sym(const std::vector<double> &A0, size_t n,
                           std::vector<double> &evals, std::vector<double> &V)
    {
        std::vector<double> A = A0;
        V.assign(n * n, 0.0);
        for (size_t i = 0; i < n; ++i)
            V[i * n + i] = 1.0;

        const int max_sweeps = 100;
        for (int sweep = 0; sweep < max_sweeps; ++sweep)
        {
            double off = 0.0;
            for (size_t p = 0; p < n; ++p)
                for (size_t q = p + 1; q < n; ++q)
                    off += A[p * n + q] * A[p * n + q];
            if (off < 1e-30)
                break;

            for (size_t p = 0; p < n; ++p)
            {
                for (size_t q = p + 1; q < n; ++q)
                {
                    const double apq = A[p * n + q];
                    if (std::abs(apq) < 1e-14)
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
        }

        evals.resize(n);
        for (size_t i = 0; i < n; ++i)
            evals[i] = A[i * n + i];
    }

    inline void hermitian_eig(const CTensor &H, std::vector<double> &evals, std::vector<Complex> &evecs)
    {
        const auto &s = H.get_shape();
        if (s.size() != 2 || s[0] != s[1])
            throw std::invalid_argument("hermitian_eig: requires a square matrix.");
        const size_t n = s[0];

        std::vector<double> M(4 * n * n, 0.0);
        for (size_t i = 0; i < n; ++i)
            for (size_t j = 0; j < n; ++j)
            {
                const Complex h = H.data()[i * n + j];
                const double hr = h.real(), hi = h.imag();
                M[i * 2 * n + j] = hr;
                M[i * 2 * n + (j + n)] = -hi;
                M[(i + n) * 2 * n + j] = hi;
                M[(i + n) * 2 * n + (j + n)] = hr;
            }

        std::vector<double> ev2, V;
        jacobi_sym(M, 2 * n, ev2, V);

        std::vector<size_t> idx(2 * n);
        for (size_t i = 0; i < 2 * n; ++i)
            idx[i] = i;
        std::sort(idx.begin(), idx.end(),
                  [&](size_t a, size_t b) { return ev2[a] < ev2[b]; });

        evals.resize(n);
        evecs.assign(n * n, Complex(0, 0));
        for (size_t k = 0; k < n; ++k)
        {
            const size_t e = idx[2 * k];
            evals[k] = ev2[e];
            for (size_t r = 0; r < n; ++r)
            {
                const double x = V[r * 2 * n + e];
                const double y = V[(r + n) * 2 * n + e];
                evecs[k * n + r] = Complex(x, y);
            }
        }
    }

    inline std::vector<double> hermitian_eigenvalues(const CTensor &H)
    {
        std::vector<double> ev;
        std::vector<Complex> evc;
        hermitian_eig(H, ev, evc);
        return ev;
    }

    inline double purity(const CTensor &rho)
    {
        CTensor r2 = rho.matmul(rho);
        return trace(r2).real();
    }

    inline double von_neumann_entropy(const CTensor &rho)
    {
        auto ev = hermitian_eigenvalues(rho);
        double S = 0.0;
        for (double l : ev)
            if (l > 1e-12)
                S -= l * std::log(l);
        return S;
    }

    inline double trace_distance(const CTensor &rho, const CTensor &sigma)
    {
        CTensor r = rho;
        CTensor d = r.sub(sigma);
        auto ev = hermitian_eigenvalues(d);
        double t = 0.0;
        for (double l : ev)
            t += std::abs(l);
        return 0.5 * t;
    }

    inline CTensor matrix_sqrt_psd(const CTensor &rho)
    {
        const auto &s = rho.get_shape();
        if (s.size() != 2 || s[0] != s[1])
            throw std::invalid_argument("matrix_sqrt_psd: requires a square matrix.");
        const size_t n = s[0];
        std::vector<double> ev;
        std::vector<Complex> evecs;
        hermitian_eig(rho, ev, evecs);

        CTensor out({n, n}, false);
        for (size_t k = 0; k < n; ++k)
        {
            const double sq = std::sqrt(std::max(ev[k], 0.0));
            for (size_t i = 0; i < n; ++i)
                for (size_t j = 0; j < n; ++j)
                    out.data()[i * n + j] += sq * evecs[k * n + i] * std::conj(evecs[k * n + j]);
        }
        return out;
    }

    inline double fidelity(const CTensor &rho, const CTensor &sigma)
    {
        CTensor sr = matrix_sqrt_psd(rho);
        CTensor m = sr.matmul(sigma);
        m = m.matmul(sr);
        auto ev = hermitian_eigenvalues(m);
        double tr = 0.0;
        for (double mu : ev)
            tr += std::sqrt(std::max(mu, 0.0));
        return tr * tr;
    }

    inline CTensor expm_hermitian(const CTensor &H, double theta)
    {
        const auto &s = H.get_shape();
        if (s.size() != 2 || s[0] != s[1])
            throw std::invalid_argument("expm_hermitian: requires a square matrix.");
        const size_t n = s[0];
        std::vector<double> ev;
        std::vector<Complex> evecs;
        hermitian_eig(H, ev, evecs);

        CTensor U({n, n}, false);
        for (size_t k = 0; k < n; ++k)
        {
            const Complex ph = std::polar(1.0, -theta * ev[k]);
            for (size_t i = 0; i < n; ++i)
                for (size_t j = 0; j < n; ++j)
                    U.data()[i * n + j] += ph * evecs[k * n + i] * std::conj(evecs[k * n + j]);
        }
        return U;
    }

    inline CTensor identity_matrix(size_t n)
    {
        CTensor I({n, n}, false);
        for (size_t i = 0; i < n; ++i)
            I.data()[i * n + i] = Complex(1, 0);
        return I;
    }

    inline CTensor pure_state_density(const CTensor &psi)
    {
        return outer(psi, psi);
    }
}