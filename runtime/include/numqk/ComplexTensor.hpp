#pragma once
//
// ComplexTensor.hpp —— 量子态空间线性代数（TQNF 拓扑/纠缠支柱的底层原语）
//
// 建立在 numqk/Numqk.hpp 的 Tensor<T> 之上，专为复数（量子振幅 / 密度矩阵）提供
// 量子态空间所需的线性代数：Kronecker 积、部分迹、共轭转置、bra-ket 内积/外积、
// 密度矩阵的纯度/熵/保真度/迹距离、Hermitian 特征分解、矩阵平方根与 exp(-iθH)。
//
// 全部为纯线性代数，不依赖任何量子后端（与 Numqk.hpp 同风格）。
// 对应 TQNF 设计法则的「拓扑-表征分离律」与「场-储备池统一律」。
//
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

    // ─────────────────────────────────────────────────────────────
    // 基础复数工具
    // ─────────────────────────────────────────────────────────────
    inline double re(Complex z) { return z.real(); }
    inline double im(Complex z) { return z.imag(); }

    // ─────────────────────────────────────────────────────────────
    // 共轭转置（dagger）：M†_{i,j} = conj(M_{j,i})
    // ─────────────────────────────────────────────────────────────
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

    // ─────────────────────────────────────────────────────────────
    // 迹（方阵对角线求和）
    // ─────────────────────────────────────────────────────────────
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

    // ─────────────────────────────────────────────────────────────
    // Kronecker 积（TQNF 的 coord 并置原语）：(A ⊗ B)[i·p+k][j·q+l] = A[i][j]·B[k][l]
    // 支持 1D（列向量/ket）与 2D（矩阵）输入；两输入同为 1D 时返回 1D。
    // ─────────────────────────────────────────────────────────────
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

    // ─────────────────────────────────────────────────────────────
    // 混合进制解码/编码（部分迹用）
    // ─────────────────────────────────────────────────────────────
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

    // ─────────────────────────────────────────────────────────────
    // 部分迹（TQNF 的 thread 约化原语）：
    //   rho 为复合系统密度矩阵，dims 为各子系统维度，keep[i]=true 保留子系统 i。
    //   对「被迹掉」的子系统的行/列指标做相等约束求和。
    // ─────────────────────────────────────────────────────────────
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

        Tensor<T> out({DK, DK}, false); // 已零初始化
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

    // ─────────────────────────────────────────────────────────────
    // 纯态部分迹（不显式构造全局密度矩阵，避免 2^N × 2^N 内存）：
    //   ρ_A[a,a'] = Σ_b ψ[a,b] · ψ*[a',b]
    // dims 为各子系统维度，keep[i]=true 保留子系统 i。
    // 时间复杂度 O(D_T · D_K²)，内存 O(D_K²)。
    // ─────────────────────────────────────────────────────────────
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

        CTensor rho({DK, DK}, false); // 已零初始化
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

    // ─────────────────────────────────────────────────────────────
    // bra-ket：⟨a|b⟩ = Σ conj(a_i) b_i
    // ─────────────────────────────────────────────────────────────
    inline Complex inner(const CTensor &a, const CTensor &b)
    {
        if (a.size() != b.size())
            throw std::invalid_argument("inner: vector size mismatch.");
        Complex s(0, 0);
        for (size_t i = 0; i < a.size(); ++i)
            s += std::conj(a.data()[i]) * b.data()[i];
        return s;
    }

    // ─────────────────────────────────────────────────────────────
    // 外积：|a⟩⟨b|，out[i][j] = a_i · conj(b_j)
    // ─────────────────────────────────────────────────────────────
    inline CTensor outer(const CTensor &a, const CTensor &b)
    {
        const size_t m = a.size(), n = b.size();
        CTensor out({m, n}, false);
        for (size_t i = 0; i < m; ++i)
            for (size_t j = 0; j < n; ++j)
                out.data()[i * n + j] = a.data()[i] * std::conj(b.data()[j]);
        return out;
    }

    // ─────────────────────────────────────────────────────────────
    // 期望值：⟨ψ|O|ψ⟩（O 为 n×n 方阵，ψ 为 n 元列向量）
    // ─────────────────────────────────────────────────────────────
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

    // ─────────────────────────────────────────────────────────────
    // 实对称 Jacobi 特征分解（返回升序本征值 + 列本征向量 V）
    // 仅依赖实对称矩阵的经典算法，数值稳定，供 Hermitian 实数化使用。
    // ─────────────────────────────────────────────────────────────
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
                    // 标准 Jacobi（Numerical Recipes / Golub-Van Loan）：
                    //   t = sign(θ)/(|θ|+√(θ²+1))，θ=(aqq-app)/(2·apq)
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

    // ─────────────────────────────────────────────────────────────
    // Hermitian 特征分解（实数化技巧）：
    //   H = A + iB（A 实对称、B 实反对称）→ 映射到 2n×2n 实对称矩阵
    //   M = [[A, -B],[B, A]]，其本征值为 H 的本征值（各二重）。
    //   取每个二重对的「第一支」x + iy 作为复本征向量。
    // ─────────────────────────────────────────────────────────────
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
                M[i * 2 * n + j] = hr;               // A
                M[i * 2 * n + (j + n)] = -hi;        // -B
                M[(i + n) * 2 * n + j] = hi;         // B
                M[(i + n) * 2 * n + (j + n)] = hr;   // A
            }

        std::vector<double> ev2, V;
        jacobi_sym(M, 2 * n, ev2, V);

        // 按本征值排序（保持 V 列同步）
        std::vector<size_t> idx(2 * n);
        for (size_t i = 0; i < 2 * n; ++i)
            idx[i] = i;
        std::sort(idx.begin(), idx.end(),
                  [&](size_t a, size_t b) { return ev2[a] < ev2[b]; });

        evals.resize(n);
        evecs.assign(n * n, Complex(0, 0));
        for (size_t k = 0; k < n; ++k)
        {
            const size_t e = idx[2 * k]; // 二重对的「第一支」
            evals[k] = ev2[e];
            for (size_t r = 0; r < n; ++r)
            {
                const double x = V[r * 2 * n + e];
                const double y = V[(r + n) * 2 * n + e];
                evecs[k * n + r] = Complex(x, y); // z_k[r]
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

    // ─────────────────────────────────────────────────────────────
    // 密度矩阵常用量
    // ─────────────────────────────────────────────────────────────
    // 纯度 Tr(ρ²)
    inline double purity(const CTensor &rho)
    {
        CTensor r2 = rho.matmul(rho);
        return trace(r2).real();
    }

    // von Neumann 熵 -Tr(ρ log ρ) = -Σ λ log λ（λ>0）
    inline double von_neumann_entropy(const CTensor &rho)
    {
        auto ev = hermitian_eigenvalues(rho);
        double S = 0.0;
        for (double l : ev)
            if (l > 1e-12)
                S -= l * std::log(l);
        return S;
    }

    // 迹距离 T(ρ,σ) = 0.5 Tr|ρ-σ| = 0.5 Σ |λ_i(ρ-σ)|
    inline double trace_distance(const CTensor &rho, const CTensor &sigma)
    {
        CTensor r = rho; // 复制为非常量以调用非 const 的 sub
        CTensor d = r.sub(sigma);
        auto ev = hermitian_eigenvalues(d);
        double t = 0.0;
        for (double l : ev)
            t += std::abs(l);
        return 0.5 * t;
    }

    // 半正定矩阵平方根 √ρ = Σ √λ_k z_k z_k†
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

    // 保真度 F(ρ,σ) = (Tr √(√ρ σ √ρ))²
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

    // ─────────────────────────────────────────────────────────────
    // exp(-iθH)：Hermitian 生成元 H 的李群指数（TQNF 的 time 相干流）
    //   U = Σ exp(-iθλ_k) z_k z_k†
    // ─────────────────────────────────────────────────────────────
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
            const Complex ph = std::polar(1.0, -theta * ev[k]); // exp(-iθλ)
            for (size_t i = 0; i < n; ++i)
                for (size_t j = 0; j < n; ++j)
                    U.data()[i * n + j] += ph * evecs[k * n + i] * std::conj(evecs[k * n + j]);
        }
        return U;
    }

    // ─────────────────────────────────────────────────────────────
    // 便捷构造：单位阵 / 纯态密度矩阵 |ψ⟩⟨ψ|
    // ─────────────────────────────────────────────────────────────
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