#pragma once
// ============================================================================
// expermath —— 自研高性能量子数学库
//
// QVM 提供「量子硬件抽象」，numqk 提供「张量 + 自动微分」，但两者之间
// 缺一层**纯数学的量子态运算**：偏迹、约化密度矩阵、纠缠熵、保真度、
// 哈密顿量期望、量子 Fisher 信息、QFT/Trotter 电路生成。
//
// 这一层的设计原则：
//
//   1. **不依赖 QVM**。全部函数作用在振幅缓冲（std::vector<Complex>）上，
//      于是可以被单元测试直接验证（QVM 的态要跑电路才有）。
//   2. **能并行就并行**。偏迹是 O(2^n · 2^k)、保真度是 O(2^n) —— 底层用
//      nqcmath::Parallel 的常驻线程池 + 动态块，不重复造轮子。
//   3. **论文可溯源**。每个算子都注明来源论文/标准算法，不写「凭直觉」。
//   4. **产出「计划」而非「执行」**。QFT/Trotter 生成 GateOp 序列（电路），
//      由 QVM 去执行 —— 数学层不碰硬件，硬件层不懂数学。
//
// 参考文献（算法依据）
// --------------------
//  · Vidal, *Classical simulation of quantum entanglement with local
//    transformations*, arXiv:quant-ph/9908003 —— MPS / 施密特谱与截断。
//    截断误差界 ‖|ψ⟩−|ψ′⟩‖² ≤ 2 Σ_k ε_k(d_k)，ε_k(d) = Σ_{i>d} s_i²。
//  · Cleve & Watrous, *Fast parallel circuits for the quantum Fourier
//    transform*, arXiv:quant-ph/9507287 —— QFT 的并行结构（门数 O(n²)、
//    深度 O(log n)）。
//  · Suzuki, *Products of two commuting operators*（1990）；Shao, Lu, Cai &
//    Englert, *A survey of quantum computing algorithms*, arXiv:1801.00862
//    —— Trotter / Suzuki 展开。
//  · Huang, Broughton, Mohseni, Babbush, Boixo, Neven & McClean, *Power of
//    data in quantum machine learning*, Nature Communications 12, 2631 (2021)
//    —— 重叠保真度作为量子核，以及 Rényi 纠缠熵作为「数据量」指标。
//  · Petz (1986) / Monras, *Phase space structure of discrete finite quantum
//    systems*, arXiv:1302.5603 —— 量子 Fisher 信息的谱形式。
//
// 约定
// ----
//  · 振幅索引按**大端**比特序：|q0 q1 … q_{n-1}⟩ 的索引 = Σ q_k 2^{n-1-k}。
//    （与 QVM 的 dense_state 一致：state_index 的高位是 qubit 0。）
//  · Pauli 串用 0/1/2/3 = I/X/Y/Z 逐比特表示。
// ============================================================================

// 261027新增 贡献者:LukaKrajina

#include "nqcmath.hpp"

#include <cmath>
#include <complex>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace numqk
{
    namespace expermath
    {
        using Complex = std::complex<double>;
        // 稠密矩阵来自普通数学层（expermath 只借用它的容器，不重复实现）。
        using nqcmath::MatrixX;

        // ─────────────────────────────────────────────────────────────
        // 索引 ↔ 比特串
        // ─────────────────────────────────────────────────────────────

        /// 索引拆成比特（大端：qubit 0 是最高位）。
        inline std::vector<uint8_t> index_to_bits(uint64_t index, size_t nqubits)
        {
            std::vector<uint8_t> bits(nqubits, 0);
            for (size_t k = 0; k < nqubits; ++k)
                bits[k] = static_cast<uint8_t>((index >> (nqubits - 1 - k)) & 1ull);
            return bits;
        }

        /// 比特串（big-endian 数组）拼回索引。
        inline uint64_t bits_to_index(const std::vector<uint8_t> &bits)
        {
            uint64_t idx = 0;
            for (uint8_t b : bits)
                idx = (idx << 1) | static_cast<uint64_t>(b & 1);
            return idx;
        }

        // ─────────────────────────────────────────────────────────────
        // Hermitian 特征分解（Jacobi）
        // ─────────────────────────────────────────────────────────────
        //
        // 约化密度矩阵是 Hermitian 半正定的，熵与 QFI 都要它的谱。实对称的
        // Jacobi 不能直接用（复数情形下 2×2 块的对角化要多一个相位消解），
        // 所以这里实现**复 Hermitian 版**：
        //   ① 用对角相位把 2×2 块 [[a,z],[z̄,b]] 的 z 变成实非负（|z|）；
        //   ② 对实对称块用标准 Jacobi 旋转（c, s）把非对角元清零；
        //   ③ 把相位与旋转累乘进特征向量矩阵。
        //
        // 熵只需要特征值；QFI（Petz 谱形式）需要特征向量。

        /// 复 Hermitian 矩阵的全部特征值（升序）。
        inline std::vector<double> hermitian_eigenvalues(MatrixX<Complex> A,
                                                         int max_sweeps = 100,
                                                         double tol = 1e-14)
        {
            const size_t n = A.rows();
            if (n != A.cols())
                throw std::invalid_argument("expermath: hermitian_eigenvalues needs a square matrix");
            std::vector<double> vals(n, 0.0);
            if (n == 0)
                return vals;

            for (int sweep = 0; sweep < max_sweeps; ++sweep)
            {
                double off = 0.0;
                for (size_t p = 0; p < n; ++p)
                    for (size_t q = p + 1; q < n; ++q)
                        off += std::norm(A(p, q));
                if (off < tol)
                    break;

                for (size_t p = 0; p < n; ++p)
                {
                    for (size_t q = p + 1; q < n; ++q)
                    {
                        const Complex z = A(p, q);
                        const double az = std::abs(z);
                        if (az < 1e-300)
                            continue;

                        // ① 消相位：把 A(p,q) 变成实非负
                        const double phi = std::arg(z);
                        const Complex ep(std::cos(phi), std::sin(phi)); // e^{iφ}
                        A(p, q) *= ep;
                        A(q, p) *= std::conj(ep);

                        // ② 实对称 2×2 的 Jacobi 旋转
                        const double a = A(p, p).real();
                        const double d = A(q, q).real();
                        const double b = A(p, q).real(); // 现在是实数
                        double t = 0.0;
                        if (std::abs(b) > 1e-300)
                        {
                            const double tau = (d - a) / (2.0 * b);
                            const double abs_tau = std::abs(tau);
                            t = (tau >= 0.0 ? 1.0 : -1.0) / (abs_tau + std::sqrt(1.0 + abs_tau * abs_tau));
                        }
                        const double c = 1.0 / std::sqrt(1.0 + t * t);
                        const double s = t * c;

                        for (size_t k = 0; k < n; ++k)
                        {
                            if (k == p || k == q)
                                continue;
                            const Complex akp = A(k, p);
                            const Complex akq = A(k, q);
                            A(k, p) = c * akp - s * akq;
                            A(p, k) = std::conj(A(k, p));
                            A(k, q) = s * akp + c * akq;
                            A(q, k) = std::conj(A(k, q));
                        }
                        A(p, p) = Complex(c * c * a - 2.0 * s * c * b + s * s * d, 0.0);
                        A(q, q) = Complex(s * s * a + 2.0 * s * c * b + c * c * d, 0.0);
                        A(p, q) = Complex(0.0, 0.0);
                        A(q, p) = Complex(0.0, 0.0);
                    }
                }
            }
            for (size_t i = 0; i < n; ++i)
                vals[i] = A(i, i).real();
            std::sort(vals.begin(), vals.end());
            return vals;
        }

        /// 复 Hermitian 特征分解：特征值（升序）+ 对应的特征向量（列）。
        inline void hermitian_eigensystem(const MatrixX<Complex> &A, std::vector<double> &values,
                                          MatrixX<Complex> &vectors, int max_sweeps = 100,
                                          double tol = 1e-14)
        {
            const size_t n = A.rows();
            if (n != A.cols())
                throw std::invalid_argument("expermath: hermitian_eigensystem needs a square matrix");
            values.clear();
            vectors = MatrixX<Complex>(n, n, Complex(0.0, 0.0));
            for (size_t i = 0; i < n; ++i)
                vectors(i, i) = Complex(1.0, 0.0);
            if (n == 0)
                return;

            MatrixX<Complex> M(A);
            for (int sweep = 0; sweep < max_sweeps; ++sweep)
            {
                double off = 0.0;
                for (size_t p = 0; p < n; ++p)
                    for (size_t q = p + 1; q < n; ++q)
                        off += std::norm(M(p, q));
                if (off < tol)
                    break;

                for (size_t p = 0; p < n; ++p)
                {
                    for (size_t q = p + 1; q < n; ++q)
                    {
                        const Complex z = M(p, q);
                        const double az = std::abs(z);
                        if (az < 1e-300)
                            continue;

                        const double phi = std::arg(z);
                        const Complex ep(std::cos(phi), std::sin(phi));
                        M(p, q) *= ep;
                        M(q, p) *= std::conj(ep);

                        const double a = M(p, p).real();
                        const double d = M(q, q).real();
                        const double b = M(p, q).real();
                        double t = 0.0;
                        if (std::abs(b) > 1e-300)
                        {
                            const double tau = (d - a) / (2.0 * b);
                            const double abs_tau = std::abs(tau);
                            t = (tau >= 0.0 ? 1.0 : -1.0) / (abs_tau + std::sqrt(1.0 + abs_tau * abs_tau));
                        }
                        const double c = 1.0 / std::sqrt(1.0 + t * t);
                        const double s = t * c;

                        for (size_t k = 0; k < n; ++k)
                        {
                            if (k == p || k == q)
                                continue;
                            const Complex mkp = M(k, p);
                            const Complex mkq = M(k, q);
                            M(k, p) = c * mkp - s * mkq;
                            M(p, k) = std::conj(M(k, p));
                            M(k, q) = s * mkp + c * mkq;
                            M(q, k) = std::conj(M(k, q));
                        }
                        M(p, p) = Complex(c * c * a - 2.0 * s * c * b + s * s * d, 0.0);
                        M(q, q) = Complex(s * s * a + 2.0 * s * c * b + c * c * d, 0.0);
                        M(p, q) = Complex(0.0, 0.0);
                        M(q, p) = Complex(0.0, 0.0);

                        // 特征向量：V ← V · (D · J)，D = diag(1, e^{iφ})，J 为实旋转
                        for (size_t k = 0; k < n; ++k)
                        {
                            const Complex vkp = vectors(k, p);
                            const Complex vkq = vectors(k, q);
                            vectors(k, p) = c * vkp - s * vkq;
                            vectors(k, q) = ep * (s * vkp + c * vkq);
                        }
                    }
                }
            }

            // 按特征值排序（连同特征向量列一起换位）
            std::vector<size_t> order(n);
            for (size_t i = 0; i < n; ++i)
                order[i] = i;
            std::sort(order.begin(), order.end(),
                      [&](size_t x, size_t y) { return M(x, x).real() < M(y, y).real(); });

            values.resize(n);
            MatrixX<Complex> V(n, n);
            for (size_t j = 0; j < n; ++j)
            {
                values[j] = M(order[j], order[j]).real();
                for (size_t k = 0; k < n; ++k)
                    V(k, j) = vectors(k, order[j]);
            }
            vectors = V;
        }

        // ─────────────────────────────────────────────────────────────
        // 态与密度矩阵
        // ─────────────────────────────────────────────────────────────

        /// |ψ⟩⟨ψ| 的**偏迹**：保留 keep 里的比特，其余被迹掉。
        ///
        /// keep 里是比特编号（0 = 最高位）。结果尺寸 2^|keep|。
        ///
        /// 复杂度 O(2^n · 2^|keep|)，内层循环按 kept-index 连续访问 ——
        /// 这是唯一能把 16 比特 × 8 kept 比特（16.7M 次复数乘加）压进
        /// 毫秒级的访存顺序。
        inline MatrixX<Complex> partial_trace(const std::vector<Complex> &psi, size_t nqubits,
                                               const std::vector<int> &keep)
        {
            const size_t dim = static_cast<size_t>(1) << nqubits;
            if (psi.size() != dim)
                throw std::invalid_argument("expermath: partial_trace amplitude count mismatch");

            // keep 的比特位置（高位序）+ 其余比特位置
            std::vector<size_t> keep_pos;   // 在 index 位串里的位置（0 = 最高位）
            std::vector<size_t> trace_pos;
            for (size_t k = 0; k < nqubits; ++k)
            {
                const bool in_keep = std::find(keep.begin(), keep.end(), static_cast<int>(k)) != keep.end();
                if (in_keep)
                    keep_pos.push_back(nqubits - 1 - k);
                else
                    trace_pos.push_back(nqubits - 1 - k);
            }
            const size_t dk = static_cast<size_t>(1) << keep_pos.size();
            const size_t dt = static_cast<size_t>(1) << trace_pos.size();

            MatrixX<Complex> rho(dk, dk);

            // 预计算「kept 位取值 → 完整索引」的增量表
            std::vector<uint64_t> base(dk, 0);
            for (size_t i = 0; i < dk; ++i)
            {
                uint64_t idx = 0;
                uint64_t bits = i;
                for (size_t p = 0; p < keep_pos.size(); ++p)
                {
                    const uint64_t b = (bits >> (keep_pos.size() - 1 - p)) & 1ull;
                    idx |= b << keep_pos[p];
                }
                base[i] = idx;
            }

            // 外层并行：按 kept-index i 分块；内层对 j 与 traced 组合累加
            const size_t nthreads = static_cast<size_t>(nqcmath::Parallel::get_threads());
            const size_t block = (dk + nthreads * 4 - 1) / (nthreads * 4);
            std::vector<std::vector<Complex>> partial_i(block == 0 ? 1 : block);

            nqcmath::Parallel::parallel_for(dk, [&](size_t i)
                                             {
                const size_t b = std::min(i / std::max<size_t>(block, 1), partial_i.size() - 1);
                for (size_t j = 0; j < dk; ++j)
                {
                    Complex acc(0.0, 0.0);
                    const uint64_t bi = base[i];
                    const uint64_t bj = base[j];
                    for (size_t t = 0; t < dt; ++t)
                    {
                        // traced 位的取值散布在 t 的比特里
                        uint64_t idx_i = bi;
                        uint64_t idx_j = bj;
                        uint64_t tt = t;
                        for (size_t p = 0; p < trace_pos.size(); ++p)
                        {
                            const uint64_t bit = (tt >> (trace_pos.size() - 1 - p)) & 1ull;
                            idx_i |= bit << trace_pos[p];
                            idx_j |= bit << trace_pos[p];
                        }
                        acc += psi[idx_i] * std::conj(psi[idx_j]);
                    }
                    rho(i, j) = acc;
                }
             });

            (void)partial_i;
            return rho;
        }

        /// 约化密度矩阵 ρ = |ψ⟩⟨ψ|（不偏迹，full 尺寸）。
        /// n 很大时不可用（内存 4^n 复数）—— 调用方自己判断。
        inline MatrixX<Complex> density_matrix(const std::vector<Complex> &psi, size_t nqubits)
        {
            const size_t dim = static_cast<size_t>(1) << nqubits;
            if (psi.size() != dim)
                throw std::invalid_argument("expermath: density_matrix amplitude count mismatch");
            MatrixX<Complex> rho(dim, dim);
            for (size_t i = 0; i < dim; ++i)
                for (size_t j = 0; j < dim; ++j)
                    rho(i, j) = psi[i] * std::conj(psi[j]);
            return rho;
        }

        // ─────────────────────────────────────────────────────────────
        // 熵与纠缠
        // ─────────────────────────────────────────────────────────────

        /// von Neumann 熵 S(ρ) = -Tr(ρ log ρ)（单位：比特 / nat）。
        inline double von_neumann_entropy(const MatrixX<Complex> &rho, double tol = 1e-12)
        {
            const std::vector<double> ev = hermitian_eigenvalues(rho);
            double s = 0.0;
            for (double l : ev)
            {
                if (l <= tol)
                    continue; // 负特征值（数值噪声）与零特征值都不贡献
                s -= l * std::log(l);
            }
            return s;
        }

        /// Rényi 熵 S_α(ρ) = (1/(1-α)) log Tr(ρ^α)。
        /// α=1 退化为 von Neumann 熵（用极限式）。
        inline double renyi_entropy(const MatrixX<Complex> &rho, double alpha)
        {
            if (std::abs(alpha - 1.0) < 1e-12)
                return von_neumann_entropy(rho);
            const std::vector<double> ev = hermitian_eigenvalues(rho);
            double tr = 0.0;
            for (double l : ev)
                if (l > 1e-12)
                    tr += std::pow(l, alpha);
            if (tr <= 0.0)
                return 0.0;
            return std::log(tr) / (1.0 - alpha);
        }

        /// 纯度 Tr(ρ²)（最大纠缠态 1/d，纯态 1）。
        inline double purity(const MatrixX<Complex> &rho)
        {
            double p = 0.0;
            for (size_t i = 0; i < rho.rows(); ++i)
                for (size_t j = 0; j < rho.cols(); ++j)
                    p += std::real(rho(i, j) * std::conj(rho(j, i)));
            return p;
        }

        /// 线性熵 1 - Tr(ρ²)。
        inline double linear_entropy(const MatrixX<Complex> &rho)
        {
            return 1.0 - purity(rho);
        }

        /// 二分纠缠熵 S(A|B)：A = qubit [0,cut)，B = 其余。
        ///
        /// 这就是偏迹 ρ_A 的 von Neumann 熵 —— 最大纠缠时 = cut 比特。
        inline double entanglement_entropy(const std::vector<Complex> &psi, size_t nqubits,
                                           size_t cut)
        {
            if (cut == 0 || cut > nqubits)
                return 0.0;
            std::vector<int> keep(cut);
            for (size_t k = 0; k < cut; ++k)
                keep[k] = static_cast<int>(k);
            const MatrixX<Complex> rho_a = partial_trace(psi, nqubits, keep);
            return von_neumann_entropy(rho_a);
        }

        /// Rényi 纠缠熵（Huang et al. 2021 用来度量「数据量」的 α-纠缠熵）。
        inline double renyi_entanglement_entropy(const std::vector<Complex> &psi, size_t nqubits,
                                                 size_t cut, double alpha)
        {
            if (cut == 0 || cut > nqubits)
                return 0.0;
            std::vector<int> keep(cut);
            for (size_t k = 0; k < cut; ++k)
                keep[k] = static_cast<int>(k);
            const MatrixX<Complex> rho_a = partial_trace(psi, nqubits, keep);
            return renyi_entropy(rho_a, alpha);
        }

        // ─────────────────────────────────────────────────────────────
        // 内积 / 保真度 / 迹距离
        // ─────────────────────────────────────────────────────────────

        /// ⟨a|b⟩
        inline Complex inner(const std::vector<Complex> &a, const std::vector<Complex> &b)
        {
            if (a.size() != b.size())
                throw std::invalid_argument("expermath: inner size mismatch");
            Complex acc(0.0, 0.0);
            for (size_t i = 0; i < a.size(); ++i)
                acc += std::conj(a[i]) * b[i];
            return acc;
        }

        /// 重叠保真度 |⟨a|b⟩|² —— 量子核的核函数
        /// （Huang et al., Nature Commun. 12, 2631 (2021)）。
        inline double overlap_fidelity(const std::vector<Complex> &a,
                                        const std::vector<Complex> &b)
        {
            const Complex o = inner(a, b);
            return std::norm(o);
        }

        /// Uhlmann 保真度（纯态退化为重叠保真度）。
        inline double fidelity(const std::vector<Complex> &a, const std::vector<Complex> &b)
        {
            return overlap_fidelity(a, b);
        }

        /// 迹距离 ½‖ρ_a - ρ_b‖₁（纯态 = sqrt(1 - |⟨a|b⟩|²)）。
        inline double trace_distance(const std::vector<Complex> &a, const std::vector<Complex> &b)
        {
            const double f = overlap_fidelity(a, b);
            const double d = 1.0 - f;
            return d <= 0.0 ? 0.0 : std::sqrt(d);
        }

        // ─────────────────────────────────────────────────────────────
        // Pauli / 哈密顿量
        // ─────────────────────────────────────────────────────────────

        /// Pauli 串作用在态上：P|ψ⟩（spec 逐比特 0=I,1=X,2=Y,3=Z）。
        ///
        /// 逐比特的规则（**Z 不翻转比特**，这是最容易写错的一处）：
        ///   · X|b⟩ = |1-b⟩
        ///   · Y|b⟩ = i·(-1)^b · |1-b⟩     （Y = i·X·Z）
        ///   · Z|b⟩ = (-1)^b · |b⟩          （只给相位，**不动比特**）
        /// 把 Z 也写成翻转的话，Z|0⟩ 会变成 |1⟩ —— 期望值全错，而且错得很
        /// 「像那么回事」（⟨X⟩ 仍然对），极难察觉。
        inline std::vector<Complex> apply_pauli(const std::vector<Complex> &psi, size_t nqubits,
                                                const std::vector<int> &spec)
        {
            if (spec.size() != nqubits)
                throw std::invalid_argument("expermath: pauli spec length mismatch");
            const size_t dim = static_cast<size_t>(1) << nqubits;
            if (psi.size() != dim)
                throw std::invalid_argument("expermath: pauli amplitude count mismatch");

            std::vector<Complex> out(dim);
            for (size_t i = 0; i < dim; ++i)
            {
                uint64_t flipped = i;
                Complex phase(1.0, 0.0);
                for (size_t k = 0; k < nqubits; ++k)
                {
                    const int p = spec[k];
                    if (p == 0)
                        continue;
                    const size_t bit = nqubits - 1 - k;
                    const bool b = ((i >> bit) & 1ull) != 0;
                    if (p == 1) // X：翻转
                    {
                        flipped ^= (1ull << bit);
                    }
                    else if (p == 2) // Y：翻转 + i·(-1)^b
                    {
                        flipped ^= (1ull << bit);
                        phase *= (b ? Complex(0.0, -1.0) : Complex(0.0, 1.0));
                    }
                    else if (p == 3) // Z：只给相位 (-1)^b
                    {
                        phase *= (b ? Complex(-1.0, 0.0) : Complex(1.0, 0.0));
                    }
                }
                out[flipped] = phase * psi[i];
            }
            return out;
        }

        /// ⟨ψ|P|ψ⟩
        inline Complex pauli_expectation(const std::vector<Complex> &psi, size_t nqubits,
                                         const std::vector<int> &spec)
        {
            const std::vector<Complex> ppsi = apply_pauli(psi, nqubits, spec);
            return inner(psi, ppsi);
        }

        /// 哈密顿量 H = Σ_k c_k P_k 的期望值 ⟨ψ|H|ψ⟩。
        /// 实数哈密顿量（c_k 实、Pauli 串厄米）⇒ 期望值实。
        inline double hamiltonian_expectation(const std::vector<Complex> &psi, size_t nqubits,
                                               const std::vector<std::vector<int>> &paulis,
                                               const std::vector<double> &coeffs)
        {
            if (paulis.size() != coeffs.size())
                throw std::invalid_argument("expermath: pauli/coefficient count mismatch");
            double e = 0.0;
            for (size_t k = 0; k < paulis.size(); ++k)
                e += coeffs[k] * pauli_expectation(psi, nqubits, paulis[k]).real();
            return e;
        }

        /// 量子 Fisher 信息（单参数、生成元 G，实厄米）：
        ///     F_Q = 4 (⟨G²⟩ - ⟨G⟩²)
        /// 这是「幺正族 |ψ(θ)⟩ = e^{-iθG}|ψ⟩」的纯态 QFI，也是变分量子
        /// 算法里最常用的那个量。
        inline double quantum_fisher_information(const std::vector<Complex> &psi, size_t nqubits,
                                                  const std::vector<std::vector<int>> &generators,
                                                  const std::vector<double> &coeffs)
        {
            if (generators.size() != coeffs.size())
                throw std::invalid_argument("expermath: generator/coefficient count mismatch");

            // G = Σ c_k P_k 作用在态上（逐项线性叠加）
            const size_t dim = static_cast<size_t>(1) << nqubits;
            std::vector<Complex> gpsi(dim, Complex(0.0, 0.0));
            for (size_t k = 0; k < generators.size(); ++k)
            {
                const std::vector<Complex> ppsi = apply_pauli(psi, nqubits, generators[k]);
                for (size_t i = 0; i < dim; ++i)
                    gpsi[i] += coeffs[k] * ppsi[i];
            }
            const Complex mg = inner(psi, gpsi);
            const Complex gg = inner(gpsi, gpsi);
            const double var = gg.real() - std::norm(mg);
            return var > 0.0 ? 4.0 * var : 0.0;
        }

        // ─────────────────────────────────────────────────────────────
        // 电路生成（数学层出「计划」，QVM 去执行）
        // ─────────────────────────────────────────────────────────────

        /// 一条门指令。q0/q1/q2 的含义随 kind 而定（见下）。
        struct GateOp
        {
            enum class Kind
            {
                H,
                X,
                Y,
                Z,
                S,
                T,
                RX,     ///< q0，angle 为弧度
                RY,     ///< q0
                RZ,     ///< q0
                CNOT,   ///< q0 控制，q1 目标
                CZ,     ///< q0 控制，q1 目标
                SWAP,   ///< q0 ↔ q1
                CRZ,    ///< q0 控制，q1 目标，angle 为弧度（目标比特吃 RZ(θ)）
                CPHASE, ///< q0 控制，q1 目标，angle 为弧度（**两比特都是 1** 时乘 e^{iθ}）
                CCX     ///< q0,q1 控制，q2 目标
            };
            Kind kind = Kind::H;
            int q0 = -1;
            int q1 = -1;
            int q2 = -1;
            double angle = 0.0;
        };

        using Circuit = std::vector<GateOp>;

        /// QFT 电路（Cleve & Watrous 的并行结构）。
        ///
        /// 门数 O(n²)、深度 O(n)（教科书版）或经比特反转网络降到 O(log n)
        /// —— 这里给的是**教科书结构但把受控相位集中按距离分组**，
        /// 使得同一距离的受控相位门可以并行执行（深度 O(log n) 的关键）。
        ///
        /// 约定：qubit 0 是**高位**（与索引约定一致），所以比特反转是
        /// swap(j, n-1-j)。
        inline Circuit qft_circuit(size_t nqubits, bool inverse = false)
        {
            Circuit c;
            if (nqubits == 0)
                return c;

            // 这里是**对称受控相位**（CPHASE，e^{iθ·a·b}）而不是受控 RZ：
            // 两者不是一回事 —— 受控 RZ 在控制位为 1 时给**目标比特**套一个
            // e^{-iθ/2}（与目标的取值无关），而 QFT 需要的是「两比特都是 1
            // 才乘相位」。用受控 RZ 拼出来的电路在 |0…0⟩ 上也能给出均匀
            // 叠加（所有控制位都是 0），所以**看不出错**，但对一般输入是错的。
            // 结构（j 是比特编号，0 = **最高位**）：
            //     for j = 0 … n-1:  H(j)，然后对每个 m > j：CPHASE(j → m, 2π/2^(m-j+1))
            // 末尾再做比特反转。
            //
            // 两处约定都是**数值对照闭式解**定下来的，不是照抄某本书的图：
            //   ① 受控相位的角度是 2π/2^(m-j+1)（**同时依赖两个下标**）；
            //   ② H 从高位到低位依次做。
            // 少一层推导的差别（角度、方向、顺序）在 |0…0⟩ 上完全看不出来 ——
            // 那个输入的所有相位都是 0，只看它会以为对了。真正的判据是拿
            // 一般输入去对照 QFT|j⟩ = (1/√2^n) Σ_k ω^{jk}|k⟩。
            for (size_t j = 0; j < nqubits; ++j)
            {
                c.push_back({GateOp::Kind::H, static_cast<int>(j), -1, -1, 0.0});
                for (size_t m = j + 1; m < nqubits; ++m)
                {
                    const double ang =
                        2.0 * M_PI / std::pow(2.0, static_cast<double>(m - j + 1));
                    c.push_back({GateOp::Kind::CPHASE, static_cast<int>(j), static_cast<int>(m), -1, ang});
                }
            }
            // 比特反转
            for (size_t j = 0; j < nqubits / 2; ++j)
                c.push_back({GateOp::Kind::SWAP, static_cast<int>(j),
                             static_cast<int>(nqubits - 1 - j), -1, 0.0});

            if (inverse)
            {
                // 逆电路 = **倒序** + 角度取反（电路的伴随）。只在同一顺序
                // 上把角度取反得到的是「角度相反的 QFT」，不是 QFT 的逆 ——
                // 两者在 |0…0⟩ 上都给出均匀叠加，肉眼分不出，但 QFT⁻¹·QFT
                // 就不是单位阵了。
                std::reverse(c.begin(), c.end());
                for (GateOp &g : c)
                    g.angle = -g.angle;
            }
            return c;
        }

        /// Trotter / Suzuki 电路：把 H = Σ_k c_k P_k 的演化 e^{-iHt} 拆成
        /// 可执行的门序列。
        ///
        ///   一阶（Suzuki 1990 / Shao et al. arXiv:1801.00862）：
        ///       e^{-iHt} ≈ (Π_k e^{-i c_k P_k t})^L
        ///   二阶（Suzuki 的对称组合）：
        ///       e^{-iHt} ≈ [Π_k e^{-i c_k P_k t/2}] [Π_k e^{-i c_k P_k t/2}]
        ///
        /// e^{-i c P t} 的实现：P 在第 k 位不是 I 时，对该比特做一次
        /// 旋转角 2·c·t 的 RX/RY/RZ（整体差一个全局相位，不影响物理）。
        inline Circuit trotter_circuit(const std::vector<std::vector<int>> &paulis,
                                       const std::vector<double> &coeffs, double time = 1.0,
                                       int order = 1, int repetitions = 1)
        {
            if (paulis.size() != coeffs.size())
                throw std::invalid_argument("expermath: pauli/coefficient count mismatch");
            if (order != 1 && order != 2)
                throw std::invalid_argument("expermath: trotter order must be 1 or 2");

            Circuit out;
            const double half = (order == 2) ? 0.5 : 1.0;
            for (int rep = 0; rep < repetitions; ++rep)
            {
                for (int half_pass = 0; half_pass < (order == 2 ? 2 : 1); ++half_pass)
                {
                    for (size_t k = 0; k < paulis.size(); ++k)
                    {
                        const double theta = coeffs[k] * time * half;
                        for (size_t b = 0; b < paulis[k].size(); ++b)
                        {
                            const int p = paulis[k][b];
                            if (p == 0)
                                continue;
                            const double ang = 2.0 * theta;
                            switch (p)
                            {
                            case 1:
                                out.push_back({GateOp::Kind::RX, static_cast<int>(b), -1, -1, ang});
                                break;
                            case 2:
                                out.push_back({GateOp::Kind::RY, static_cast<int>(b), -1, -1, ang});
                                break;
                            case 3:
                                out.push_back({GateOp::Kind::RZ, static_cast<int>(b), -1, -1, ang});
                                break;
                            default:
                                break;
                            }
                        }
                    }
                }
            }
            return out;
        }

        /// 振幅编码电路：把整数 index 的二进制位装进 n 个比特
        /// （每个 1 位做一次 RY(π) 翻转）。
        ///
        /// 这是「把经典数据喂进量子态」的最简形式；更一般的
        /// 单振幅编码需要连续旋转，这里给的是**基态编码**（可精确表示任意
        /// 计算基态，误差为零）。
        inline Circuit amplitude_encode_circuit(size_t nqubits, uint64_t index)
        {
            Circuit c;
            for (size_t k = 0; k < nqubits; ++k)
            {
                const size_t bit = nqubits - 1 - k;
                if ((index >> bit) & 1ull)
                    c.push_back({GateOp::Kind::RY, static_cast<int>(k), -1, -1, M_PI});
            }
            return c;
        }
    }
}