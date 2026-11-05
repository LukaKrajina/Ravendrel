// ============================================================================
// numqk_math_test.cpp —— nqcmath（普通数学 + 并行层）与 expermath（量子数学）
// 的已知答案测试
//
// 为什么要「已知答案」而不是「跑一遍看看」
// ----------------------------------------
// 这一层的每个算子都有一个**闭式解**可以对：Bell 态的偏迹必须是 I/2、
// 最大纠缠的二分熵必须是 1 比特、QFT|0…0⟩ 必须是均匀叠加、H = Z 在 |0⟩ 上
// 的期望必须是 1。「跑一遍不崩」证明不了任何事 —— 一个把偏迹算成恒等阵的
// 实现同样「不崩」。
//
// 测试里带一个**极小的态模拟器**（只支持 QFT/Trotter 用到的门），因为
// expermath 的设计是「出电路计划、由 QVM 执行」；要验证电路正确，就得有个
// 执行者。模拟器在这里只服务于测试（生产路径不走它）。
// ============================================================================

#include "numqk/Numqk.hpp"

#include <cmath>
#include <complex>
#include <cstdio>
#include <vector>

using Complex = std::complex<double>;
using numqk::nqcmath::Matrix;
using numqk::nqcmath::MatrixX;
using numqk::expermath::GateOp;

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
        std::printf("  %s %s (%.12g vs %.12g)\n", ok ? "ok  " : "FAIL", what, a, b);
        if (!ok)
            ++g_fail;
    }

    // ── 测试用的极小态模拟器（只支持 QFT/Trotter 的门）──────────────
    //
    // 比特约定与 expermath 一致：qubit 0 是**最高位**。
    void apply_gate(std::vector<Complex> &psi, const GateOp &g)
    {
        const size_t n = psi.size();
        const size_t logn = static_cast<size_t>(std::llround(std::log2(static_cast<double>(n))));
        auto bit_of = [&](uint64_t idx, int q) { return (idx >> (logn - 1 - static_cast<size_t>(q))) & 1ull; };
        auto flip = [&](uint64_t idx, int q) { return idx ^ (1ull << (logn - 1 - static_cast<size_t>(q))); };

        std::vector<Complex> out(n);
        for (uint64_t i = 0; i < n; ++i)
        {
            switch (g.kind)
            {
            case GateOp::Kind::H:
            {
                // H|b⟩ = (|0⟩ + (−1)^b|1⟩)/√2，对目标下标 i 写成矩阵乘：
                //     (Hψ)[i] = (ψ[flip(i)] + (−1)^{b_i}·ψ[i]) / √2
                // 两项的顺序反了就会得到 −|1⟩ 分量（H|0⟩ 变成 (|0⟩−|1⟩)/√2）——
                // 归一化仍然成立，于是「看起来对」，只有相位对不对。
                const double b = bit_of(i, g.q0);
                const Complex other = psi[flip(i, g.q0)];
                out[i] = (other + (b ? -psi[i] : psi[i])) / std::sqrt(2.0);
                break;
            }
            case GateOp::Kind::X:
                out[i] = psi[flip(i, g.q0)];
                break;
            case GateOp::Kind::Z:
                out[i] = bit_of(i, g.q0) ? -psi[i] : psi[i];
                break;
            case GateOp::Kind::RX:
            {
                const Complex c = std::cos(g.angle / 2.0), s = std::sin(g.angle / 2.0);
                const Complex a = psi[i], b = psi[flip(i, g.q0)];
                out[i] = c * a - Complex(0, 1) * s * b;
                out[flip(i, g.q0)] = c * b - Complex(0, 1) * s * a;
                break;
            }
            case GateOp::Kind::RY:
            {
                const Complex c = std::cos(g.angle / 2.0), s = std::sin(g.angle / 2.0);
                const Complex a = psi[i], b = psi[flip(i, g.q0)];
                out[i] = c * a - s * b;
                out[flip(i, g.q0)] = c * b + s * a;
                break;
            }
            case GateOp::Kind::RZ:
            {
                const Complex e = std::exp(Complex(0, -g.angle / 2.0));
                const Complex ei = std::exp(Complex(0, g.angle / 2.0));
                out[i] = bit_of(i, g.q0) ? psi[i] * ei : psi[i] * e;
                break;
            }
            case GateOp::Kind::CNOT:
            {
                const uint64_t j = bit_of(i, g.q0) ? flip(i, g.q1) : i;
                out[j] = psi[i];
                break;
            }
            case GateOp::Kind::CRZ:
            {
                // 控制位为 1 时，目标比特吃 RZ(θ) = diag(e^{-iθ/2}, e^{+iθ/2})；
                // 控制位为 0 时**什么也不做**（不是 e^{-iθ/2}）。
                const uint64_t j = bit_of(i, g.q0) ? flip(i, g.q1) : i;
                if (bit_of(i, g.q0))
                {
                    const double h = g.angle / 2.0;
                    out[j] = bit_of(i, g.q1) ? psi[i] * std::exp(Complex(0.0, h))
                                            : psi[i] * std::exp(Complex(0.0, -h));
                }
                else
                {
                    out[j] = psi[i];
                }
                break;
            }
            case GateOp::Kind::CPHASE:
            {
                // 对称受控相位：两比特都是 1 时乘 e^{iθ}
                const uint64_t j = (bit_of(i, g.q0) && bit_of(i, g.q1)) ? i : i;
                out[j] = (bit_of(i, g.q0) && bit_of(i, g.q1))
                              ? psi[i] * std::exp(Complex(0.0, g.angle))
                              : psi[i];
                break;
            }
            case GateOp::Kind::SWAP:
            {
                // 交换两个比特的值：两比特**不同**时才把两位都翻（相同则不动）。
                // 「先翻 q1 再翻 q0」那个写法在两比特不同时等于翻了两次 = 没翻 ——
                // 交换成了恒等，于是「比特反转」这步静默失效。
                const uint64_t ba = bit_of(i, g.q0);
                const uint64_t bb = bit_of(i, g.q1);
                const uint64_t src = (ba == bb)
                                         ? i
                                         : (i ^ (1ull << (logn - 1 - static_cast<size_t>(g.q0))) ^
                                            (1ull << (logn - 1 - static_cast<size_t>(g.q1))));
                out[src] = psi[i];
                break;
            }
            default:
                out[i] = psi[i];
                break;
            }
        }
        psi.swap(out);
    }

    void run_circuit(std::vector<Complex> &psi, const std::vector<GateOp> &c)
    {
        for (const GateOp &g : c)
            apply_gate(psi, g);
    }

    std::vector<Complex> zero_state(size_t n)
    {
        std::vector<Complex> psi(static_cast<size_t>(1) << n, Complex(0.0, 0.0));
        psi[0] = Complex(1.0, 0.0);
        return psi;
    }
} // namespace

int main()
{
    // ═══════════════════════════════════════════════════════════════════
    // nqcmath：定长矩阵
    // ═══════════════════════════════════════════════════════════════════
    std::printf("[1] nqcmath::Matrix（定长，编译期维度）\n");
    {
        Matrix<double, 2, 3> a;
        for (int r = 0; r < 2; ++r)
            for (int c = 0; c < 3; ++c)
                a(r, c) = r * 3 + c + 1;   // [1 2 3; 4 5 6]
        check_near(a(1, 2), 6.0, 1e-12, "按 (r,c) 访问");

        Matrix<double, 3, 2> b;
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 2; ++c)
                b(r, c) = (r == c) ? 1.0 : 0.0;   // 单位阵
        const auto ab = a * b;                       // 2×2
        check_near(ab(0, 0), 1.0, 1e-12, "乘法取对角");
        check_near(ab(1, 1), 5.0, 1e-12, "乘法取对角 2（b 是单位阵，结果即 a）");

        const auto at = a.transpose();
        check_near(at(2, 1), 6.0, 1e-12, "转置");
        check_near(a.sum(), 21.0, 1e-12, "求和");
    }

    // ═══════════════════════════════════════════════════════════════════
    // nqcmath：动态矩阵 + 并行 matmul
    // ═══════════════════════════════════════════════════════════════════
    std::printf("[2] nqcmath::MatrixX + Parallel::matmul\n");
    {
        const size_t M = 137, K = 91, N = 73;   // 非 2 的幂，故意不对齐
        MatrixX<double> A(M, K), B(K, N), C(M, N);
        for (size_t i = 0; i < M; ++i)
            for (size_t j = 0; j < K; ++j)
                A(i, j) = 0.5 * static_cast<double>((i * 7 + j * 3) % 11) - 2.0;
        for (size_t i = 0; i < K; ++i)
            for (size_t j = 0; j < N; ++j)
                B(i, j) = 0.25 * static_cast<double>((i * 5 + j * 2) % 7) - 0.5;

        C = A * B;   // 走并行分块路径

        // 与朴素实现逐元素比对
        double max_err = 0.0;
        for (size_t i = 0; i < M; ++i)
            for (size_t j = 0; j < N; ++j)
            {
                double acc = 0.0;
                for (size_t k = 0; k < K; ++k)
                    acc += A(i, k) * B(k, j);
                const double e = std::fabs(acc - C(i, j));
                if (e > max_err)
                    max_err = e;
            }
        check(max_err < 1e-10, "并行 matmul 与朴素实现一致");
        check_near(C.frobenius_norm() > 0.0 ? 1.0 : 0.0, 1.0, 0.0, "结果非零");

        // 形状不匹配必须抛异常（不是静默截断）
        bool threw = false;
        try
        {
            MatrixX<double> bad = A * MatrixX<double>(K + 1, N);
            (void)bad;
        }
        catch (const std::invalid_argument &)
        {
            threw = true;
        }
        check(threw, "形状不匹配抛 invalid_argument");
    }

    // ═══════════════════════════════════════════════════════════════════
    // Parallel：for / reduce / 旧签名 elementwise
    // ═══════════════════════════════════════════════════════════════════
    std::printf("[3] Parallel：for / reduce / elementwise\n");
    {
        // 每个下标恰好被访问一次
        const size_t n = 200000;
        std::vector<int> hits(n, 0);
        numqk::nqcmath::Parallel::parallel_for(n, [&](size_t i) { hits[i]++; });
        size_t bad = 0;
        for (size_t i = 0; i < n; ++i)
            if (hits[i] != 1)
                ++bad;
        check(bad == 0, "parallel_for 每个下标恰好一次（无重复无遗漏）");

        // 归约 = 串行求和
        const double serial = 0.0;
        (void)serial;
        double s = 0.0;
        for (size_t i = 0; i < n; ++i)
            s += 1.0 / static_cast<double>(i + 1);
        const double par = numqk::nqcmath::Parallel::parallel_reduce<double>(
            n, 0.0, [&](size_t i) { return 1.0 / static_cast<double>(i + 1); },
            [](double a, double b) { return a + b; });
        check_near(par, s, 1e-9, "parallel_reduce 与串行求和一致");

        // 旧签名（修好了 2n 线程 + off-by-one 的那个）
        Matrix<double, 4, 4> s1, s2, out;
        for (int i = 0; i < 16; ++i)
        {
            s1.data()[i] = static_cast<double>(i);
            s2.data()[i] = 1.0;
        }
        numqk::nqcmath::Parallel::parallel_elementwise(s1, s2, out, [](size_t i) { /* 旧签名只给 index */ });
        check(true, "旧签名 parallel_elementwise 可用（不再是编译错误）");
    }

    // ═══════════════════════════════════════════════════════════════════
    // expermath：偏迹 / 熵 / 纠缠
    // ═══════════════════════════════════════════════════════════════════
    std::printf("[4] expermath：偏迹与熵\n");
    {
        using namespace numqk::expermath;

        // Bell 态 (|00⟩+|11⟩)/√2：偏迹任一比特 → I/2，熵 = 1 比特
        std::vector<Complex> bell(4, Complex(0.0, 0.0));
        bell[0] = Complex(1.0 / std::sqrt(2.0), 0.0);
        bell[3] = Complex(1.0 / std::sqrt(2.0), 0.0);

        const MatrixX<Complex> rho = partial_trace(bell, 2, {0});
        check(std::fabs(rho(0, 0).real() - 0.5) < 1e-12, "Bell 态偏迹 ρ(0,0)=1/2");
        check(std::fabs(rho(0, 1).real()) < 1e-12, "Bell 态偏迹无非对角元");
        check_near(von_neumann_entropy(rho), std::log(2.0), 1e-12, "Bell 态偏迹熵 = 1 比特");
        check_near(entanglement_entropy(bell, 2, 1), std::log(2.0), 1e-12, "Bell 态二分纠缠熵 = 1 比特");
        check_near(purity(rho), 0.5, 1e-12, "Bell 态偏迹纯度 = 1/2");
        check_near(linear_entropy(rho), 0.5, 1e-12, "Bell 态线性熵 = 1/2");
        // Rényi-2 熵 = -log(Tr(ρ²)) = -log(purity) = log(2)（Bell 态）
        check_near(renyi_entropy(rho, 2.0), std::log(2.0), 1e-12, "Bell 态 Rényi-2 熵 = 1 比特");
        // α→1 退化为 von Neumann 熵
        check_near(renyi_entropy(rho, 1.0), std::log(2.0), 1e-12, "Rényi α=1 退化为 von Neumann 熵");

        // 积态：所有熵为 0
        std::vector<Complex> prod = zero_state(2);
        check_near(entanglement_entropy(prod, 2, 1), 0.0, 1e-12, "积态纠缠熵 = 0");
        check_near(purity(partial_trace(prod, 2, {0})), 1.0, 1e-12, "积态偏迹纯度 = 1");

        // Rényi 纠缠熵（α=2 时有闭式解：S₂ = -log₂ Tr ρ²）
        check_near(renyi_entanglement_entropy(bell, 2, 1, 2.0), std::log(2.0), 1e-9,
                   "Bell 态 Rényi 熵(α=2) = 1 比特");

        // 三比特 GHZ 的偏迹（留两个比特）
        std::vector<Complex> ghz(8, Complex(0.0, 0.0));
        ghz[0] = Complex(1.0 / std::sqrt(2.0), 0.0);
        ghz[7] = Complex(1.0 / std::sqrt(2.0), 0.0);
        const MatrixX<Complex> rho_ghz = partial_trace(ghz, 3, {0, 1});
        check_near(von_neumann_entropy(rho_ghz), std::log(2.0), 1e-9, "GHZ3 偏迹熵 = 1 比特");
    }

    // ═══════════════════════════════════════════════════════════════════
    // expermath：保真度 / Pauli / 哈密顿量 / QFI
    // ═══════════════════════════════════════════════════════════════════
    std::printf("[5] expermath：保真度、Pauli 期望、QFI\n");
    {
        using namespace numqk::expermath;

        std::vector<Complex> a = zero_state(1);   // 单比特 |0⟩
        std::vector<Complex> b(2, Complex(0.0, 0.0));
        b[1] = Complex(1.0, 0.0);   // |1⟩
        check_near(overlap_fidelity(a, a), 1.0, 1e-12, "自身重叠保真度 = 1");
        check_near(overlap_fidelity(a, b), 0.0, 1e-12, "正交态重叠保真度 = 0");
        check_near(trace_distance(a, b), 1.0, 1e-12, "正交态迹距离 = 1");

        // ⟨Z⟩ on |0⟩ = +1, on |1⟩ = -1
        std::vector<Complex> one(2, Complex(0.0, 0.0));
        one[1] = Complex(1.0, 0.0);
        check_near(pauli_expectation(a, 1, {3}).real(), 1.0, 1e-12, "⟨Z⟩(|0⟩) = +1");
        check_near(pauli_expectation(one, 1, {3}).real(), -1.0, 1e-12, "⟨Z⟩(|1⟩) = -1");

        // ⟨X⟩ on |+⟩ = +1
        std::vector<Complex> plus(2, Complex(0.0, 0.0));
        plus[0] = Complex(1.0 / std::sqrt(2.0), 0.0);
        plus[1] = Complex(1.0 / std::sqrt(2.0), 0.0);
        check_near(pauli_expectation(plus, 1, {1}).real(), 1.0, 1e-12, "⟨X⟩(|+⟩) = +1");

        // 哈密顿量 H = Z（单比特），在 |0⟩ 上期望 = 1
        check_near(hamiltonian_expectation(a, 1, {{3}}, {1.0}), 1.0, 1e-12, "⟨Z⟩ = 1");

        // QFI：G = Z，态 |+⟩（|⟨Z⟩|=0, ⟨Z²⟩=1）→ F = 4
        check_near(quantum_fisher_information(plus, 1, {{3}}, {1.0}), 4.0, 1e-12,
                   "F_Q(|+⟩, G=Z) = 4");

        // 特征值：已知矩阵
        MatrixX<Complex> H(2, 2);
        H(0, 0) = Complex(2.0, 0.0);
        H(1, 1) = Complex(-1.0, 0.0);
        H(0, 1) = Complex(1.0, 0.0);
        H(1, 0) = Complex(1.0, 0.0);
        const auto ev = hermitian_eigenvalues(H);
        check(ev.size() == 2, "特征值个数");
        // [[2,1],[1,-1]] 的谱 = (1 ± √13)/2
        const double half_trace = 0.5, root13 = std::sqrt(13.0);
        check_near(ev[0], half_trace - root13 / 2.0, 1e-9, "最小特征值 = (1-√13)/2");
        check_near(ev[1], half_trace + root13 / 2.0, 1e-9, "最大特征值 = (1+√13)/2");
    }

    // ═══════════════════════════════════════════════════════════════════
    // expermath：QFT 电路（用模拟器验证闭式解）
    // ═══════════════════════════════════════════════════════════════════
    std::printf("[6] expermath：QFT 电路\n");
    {
        using namespace numqk::expermath;
        const size_t n = 3;
        std::vector<Complex> psi = zero_state(n);
        run_circuit(psi, qft_circuit(n));
        // QFT|0…0⟩ = 1/√(2^n) Σ_k |k⟩（等幅）
        bool uniform = true;
        for (size_t i = 0; i < psi.size(); ++i)
            if (std::fabs(std::abs(psi[i]) - 1.0 / std::sqrt(static_cast<double>(psi.size()))) > 1e-9)
                uniform = false;
        check(uniform, "QFT|0…0⟩ 是均匀叠加（振幅 1/√2^n）");

        // 逆 QFT 回到原态
        std::vector<Complex> back = psi;
        run_circuit(back, qft_circuit(n, /*inverse=*/true));
        check_near(std::abs(back[0].real() - 1.0), 0.0, 1e-9, "QFT⁻¹·QFT 回到 |0…0⟩");
        bool rest_zero = true;
        for (size_t i = 1; i < back.size(); ++i)
            if (std::abs(back[i]) > 1e-9)
                rest_zero = false;
        check(rest_zero, "QFT⁻¹·QFT 其余振幅为 0");
    }

    // ── QFT 对照**闭式定义**（这一条才是能抓住相位约定的）──────────────
    //
    // QFT|j⟩ = (1/√2^n) Σ_k ω^{jk}|k⟩，ω = e^{2πi/2^n}。
    // 上面那两条（|0…0⟩ 均匀叠加、QFT⁻¹·QFT = I）**都抓不住**相位约定的
    // 错误：|0…0⟩ 的所有相位都是 0，而往返对任何幺正电路都成立。只有拿一般
    // 输入去对照闭式解，才会发现「差一个 ω 的幂」。
    {
        using namespace numqk::expermath;
        const size_t n = 2;
        const double dn = static_cast<double>(1u << n);
        // 输入 |01⟩（即整数 1）
        std::vector<Complex> psi(4, Complex(0.0, 0.0));
        psi[1] = Complex(1.0, 0.0);
        run_circuit(psi, qft_circuit(n));

        double max_err = 0.0;
        for (size_t k = 0; k < 4; ++k)
        {
            const Complex w = std::exp(Complex(0.0, 2.0 * M_PI * static_cast<double>(1 * k) / dn));
            const Complex expect = w / std::sqrt(dn);
            max_err = std::max(max_err, std::abs(psi[k] - expect));
        }
        check(max_err < 1e-9, "QFT|01⟩ 对照闭式解 ω^{jk}");

        // 再验 n=3 的 |011⟩
        const size_t n3 = 3;
        std::vector<Complex> p3(8, Complex(0.0, 0.0));
        p3[3] = Complex(1.0, 0.0);   // |011⟩
        run_circuit(p3, qft_circuit(n3));
        double err3 = 0.0;
        const double d3 = 8.0;
        for (size_t k = 0; k < 8; ++k)
        {
            const Complex w = std::exp(Complex(0.0, 2.0 * M_PI * static_cast<double>(3 * k) / d3));
            err3 = std::max(err3, std::abs(p3[k] - w / std::sqrt(d3)));
        }
        check(err3 < 1e-9, "QFT|011⟩ 对照闭式解（n=3）");
    }

    // ═══════════════════════════════════════════════════════════════════
    // expermath：Trotter 电路（对易哈密顿量下与精确解一致）
    // ═══════════════════════════════════════════════════════════════════
    std::printf("[7] expermath：Trotter 电路\n");
    {
        using namespace numqk::expermath;
        // H = Z_0 + Z_1（对易 ⇒ 一阶 Trotter 是精确的）
        const std::vector<std::vector<int>> paulis = {{3, 0}, {0, 3}};
        const std::vector<double> coeffs = {0.7, 0.4};
        const double t = 0.3;

        std::vector<Complex> psi = zero_state(2);
        run_circuit(psi, trotter_circuit(paulis, coeffs, t, 1));

        // |00⟩ 是 Z0、Z1 的共同本征态（特征值都 +1），所以演化只改**整体
        // 相位**：e^{-i(Z0+Z1)t}|00⟩ = e^{-i(c0+c1)t}|00⟩。
        // 期望值 ⟨Z0⟩ 恒为 1（精确解与 Trotter 都一样），真正可比的是相位。
        check_near(std::abs(psi[0]), 1.0, 1e-9, "一阶 Trotter 后态仍是 |00⟩（模长 1）");
        check_near(pauli_expectation(psi, 2, {3, 0}).real(), 1.0, 1e-9,
                   "⟨Z0⟩ = 1（本征态，期望不变）");
        const double total = (coeffs[0] + coeffs[1]) * t;
        check_near(psi[0].real(), std::cos(total), 1e-9, "整体相位 = e^{-i(c0+c1)t} 的实部");
        check_near(psi[0].imag(), -std::sin(total), 1e-9, "整体相位 = e^{-i(c0+c1)t} 的虚部");
    }

    // ═══════════════════════════════════════════════════════════════════
    // expermath：振幅编码
    // ═══════════════════════════════════════════════════════════════════
    std::printf("[8] expermath：振幅编码\n");
    {
        using namespace numqk::expermath;
        std::vector<Complex> psi = zero_state(3);
        run_circuit(psi, amplitude_encode_circuit(3, 0b101));
        check_near(std::abs(psi[0b101].real() - 1.0), 0.0, 1e-9, "振幅编码把 |101⟩ 装进态");
    }

    std::printf("\n%s（失败 %d 项）\n", g_fail == 0 ? "NUMQK-MATH: 全部通过" : "NUMQK-MATH: 失败", g_fail);
    return g_fail == 0 ? 0 : 1;
}
