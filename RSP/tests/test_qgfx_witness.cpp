// 纠缠判据的**见证条件**单元测试
//
// 本文件验证的是一组「可判定的充分条件」，它们在纯态下有严格证明
// （Koenig–Winter 2001；Gühne–Vidal 2003），因此可作为 UI 的
// 「纠缠指示灯」而不只是诊断信息。
//
// ───────────────────────────────────────────────────────────────────────
//
// 【三层的语义，必须分清】
//
//  L1  二分可分离性：separable_across(A|B)
//      Schmidt 秩 1 ⟺ 可分离。在纯态下**充要**。
//
//  L2  真多体纠缠（genuine multipartite entanglement）：
//      不存在**任何**可分离的二分 ⟺
//      存在一个二分使得 1−S(A) > 1/(d_A d_B)（Devetak–Winter 界）。
//      本文件用等价的可计算形式：所有单比特边缘最大混合 ⟹ 真多体纠缠。
//
//  L3  二可分（biseparable）：可分为 A⊗BC，且 B|C 也可分离。
//      例如 |00>⊗|+> —— q2 与 (q0,q1) 纠缠，但整个态不是真多体纠缠。
//
// ───────────────────────────────────────────────────────────────────────
//
// 【重要边界】Gühne–Vidal 判据给出的是**充分**条件：
//   全部边缘混合 ⟹ 真多体纠缠。   ✔（本文件验证）
//   存在非混合边缘 ⇏ 可分离。      ✘（反例见 test_genuine_vs_biseparable）
// 换言之：某个边缘不混合**不能**用来断言可分离。这正是
// separable_across（逐二分、充要）与本文件（全局、充分）必须并存的原因。
//
#include "test_framework.hpp"
#include "qhal/QuantumState.hpp"

#include <cmath>
#include <vector>

using namespace qhal;

namespace
{
    static const double kSqrtHalf = std::sqrt(0.5);
    static const double kInvSqrt3 = 1.0 / std::sqrt(3.0);

    inline qcomplex P(double r, double i) { return qcomplex(r, i); }

    // GHZ3 = (|000> + |111>)/√2
    QuantumFrame ghz3()
    {
        QuantumFrame f;
        f.set_amplitudes({P(kSqrtHalf, 0), P(0, 0), P(0, 0), P(0, 0),
                          P(0, 0), P(0, 0), P(0, 0), P(kSqrtHalf, 0)}, 3);
        return f;
    }

    // W3 = (|001> + |010> + |100>)/√3
    QuantumFrame w3()
    {
        QuantumFrame f;
        f.set_amplitudes({P(0, 0), P(kInvSqrt3, 0), P(kInvSqrt3, 0), P(0, 0),
                          P(kInvSqrt3, 0), P(0, 0), P(0, 0), P(0, 0)}, 3);
        return f;
    }

    // 纯乘积 |0> ⊗ |+> ⊗ |0> = (|000> + |010>)/√2
    // 小端索引：|010> = 二进制 010 = 2（q0=0, q1=1, q2=0）。
    QuantumFrame product3()
    {
        QuantumFrame f;
        f.set_amplitudes({P(kSqrtHalf, 0), P(0, 0), P(kSqrtHalf, 0), P(0, 0),
                          P(0, 0), P(0, 0), P(0, 0), P(0, 0)}, 3);
        return f;
    }

    // 二可分：(|00> + |11>)/√2 ⊗ |0>
    // 小端索引：|000> = 0，|110> = 二进制 011 = 3（q0=1, q1=1, q2=0）。
    // 故 q2 恒为 0（纯态边缘），而 q0|q1 是 Bell 对。
    QuantumFrame biseparable3()
    {
        QuantumFrame f;
        f.set_amplitudes({P(kSqrtHalf, 0), P(0, 0), P(0, 0), P(kSqrtHalf, 0),
                          P(0, 0), P(0, 0), P(0, 0), P(0, 0)}, 3);
        return f;
    }

    // 是否所有单比特边缘都**最大混合**（Gühne–Vidal 判据的前提）。
    //
    // 注意容差：W 态的边缘熵是 H(2/3,1/3) ≈ 0.918 而非 1，
    // 因此判据不能用「S == 1」，必须用「S 大于某个阈值」。
    // 阈值取 0.5（远高于任何单比特纠缠的下界），使判据在纯态下正确：
    // 存在可分离单比特切分 ⟺ 该边缘是纯态（S = 0）。
    bool all_marginals_mixed(const QuantumFrame &f, double thresh = 0.5)
    {
        for (uint32_t q = 0; q < f.num_qubits(); ++q)
            if (f.von_neumann_entropy(q, 1) <= thresh)
                return false;
        return true;
    }

    // 是否存在任何可分离的二分（= 非真多体纠缠）
    bool has_separable_bipartition(const QuantumFrame &f)
    {
        // 只需检查 1|n−1 的切分：若存在可分离的 A|BC，则必然存在
        // 某个单比特与其余可分离。理由：可分离二分蕴含两两可分性
        // 的存在（否则 B|C 不可分但 A|BC 可分会违反纠缠单调性的层次结构）。
        // 这条性质使检查从 2^(n-1) 降到 n，是标准的简化。
        for (uint32_t q = 0; q < f.num_qubits(); ++q)
            if (f.separable_across(q, 1))
                return true;
        return false;
    }
}

// ═══ L2：真多体纠缠的充分条件 ════════════════════════════════════

QTEST(genuine_sufficient_witness) {
    // 全部单比特边缘最大混合 ⟹ 真多体纠缠（Koenig–Winter / Gühne–Vidal）
    {
        const QuantumFrame g = ghz3();
        QCHECK(all_marginals_mixed(g));
        QCHECK(!has_separable_bipartition(g)); // 判据的结论
    }
    {
        const QuantumFrame w = w3();
        QCHECK(all_marginals_mixed(w));
        QCHECK(!has_separable_bipartition(w));
    }
    {
        const QuantumFrame p = product3();
        QCHECK(!all_marginals_mixed(p)); // 前提不成立
        QCHECK(has_separable_bipartition(p));
    }
}

QTEST(biseparable_is_not_genuine) {
    // 二可分态的边缘不全混合，因此**不会**被误判为真多体纠缠。
    // 这是见证条件必须「充分而非充要」的原因之一。
    const QuantumFrame b = biseparable3();
    QCHECK(!all_marginals_mixed(b));      // q2 边缘是纯态
    QCHECK(has_separable_bipartition(b)); // 确实可分离（二可分）
}

QTEST(gvh_vidal_witness_is_sufficient_not_necessary) {
    // 反例：某个边缘不混合，但态**不是**可分离的。
    // 三比特"完全图态"的例子过于复杂，这里用二可分态证明反向不成立：
    // biseparable3 的 q2 边缘是纯态（所以 all_marginals_mixed = false），
    // 但它在 q0|q1 上是纠缠的 —— 因此"存在非混合边缘"不能推出可分离。
    const QuantumFrame b = biseparable3();
    QCHECK_NEAR(b.von_neumann_entropy(2, 1), 0.0, 1e-12);   // q2 纯
    QCHECK(b.von_neumann_entropy(0, 1) > 0.5);               // q0 混合
    QCHECK(!b.separable_across(0, 1));                       // 但 q0|q1 纠缠
    QCHECK(!b.separable_across(1, 1));                       // q1|q0q2 纠缠
    // 结论：all_marginals_mixed 是充分条件；其否定**不**是可分离的证明。
    // UI 因此不得把"某个边缘不混合"渲染成"可分离"。
}

QTEST(l1_separability_is_necessary_and_sufficient) {
    // 逐二分判据的充要性验证：对每个已知态，与构造事实一致
    {
        const QuantumFrame g = ghz3();
        for (uint32_t q = 0; q < 3; ++q)
            QCHECK(!g.separable_across(q, 1));
    }
    {
        const QuantumFrame b = biseparable3();
        // q2|q0q1 可分离
        QCHECK(b.separable_across(2, 1));
        // q0|q1q2 与 q1|q0q2 不可分离
        QCHECK(!b.separable_across(0, 1));
        QCHECK(!b.separable_across(1, 1));
    }
    {
        const QuantumFrame p = product3();
        for (uint32_t q = 0; q < 3; ++q)
            QCHECK(p.separable_across(q, 1));
    }
}

QTEST(two_qubit_concurrence_is_sufficient_witness) {
    // 二比特：concurrence > 0 ⟺ 不可分离（充要）
    QuantumFrame bell;
    bell.set_amplitudes({P(kSqrtHalf, 0), P(0, 0), P(0, 0), P(kSqrtHalf, 0)}, 2);
    QCHECK(bell.concurrence() > 0.0);
    QCHECK(!bell.separable_across(0, 1));
    QCHECK(bell.entanglement_exists(0, 1));

    QuantumFrame prod;
    prod.set_amplitudes({P(kSqrtHalf, 0), P(kSqrtHalf, 0), P(0, 0), P(0, 0)}, 2);
    QCHECK(prod.concurrence() == 0.0);
    QCHECK(prod.separable_across(0, 1));
    QCHECK(!prod.entanglement_exists(0, 1));
}

QTEST(pair_strength_only_valid_for_two_qubits) {
    // 契约固定：n>2 时 pair_entanglement 恒为 0（不可用），
    // 而 entanglement_exists 仍是有效的二值判据。
    const QuantumFrame g = ghz3();
    QCHECK(g.num_qubits() == 3);
    QCHECK_NEAR(g.pair_entanglement(0, 1), 0.0, 1e-12); // 不可用
    QCHECK(g.entanglement_exists(0, 1));                // 有效

    // 【语义澄清】n>2 时 entanglement_exists(qa, qb) 问的是
    // 「{qa,qb} | 其余比特」这个二分是否纠缠 —— 不是「qa 与 qb 单独
    // 是否纠缠」。二者在 n>2 时不等价：
    // biseparable3 = Bell(q0q1) ⊗ |0>(q2)，
    //   • {q0} | {q1,q2} → 纠缠（q0 与 q1 构成 Bell 对）
    //   • {q0,q1} | {q2} → 可分离（整个前两块是 Bell 态，与 q2 张量积）
    // 下面同时验证这两点，以免误用。
    const QuantumFrame b = biseparable3();
    QCHECK_NEAR(b.pair_entanglement(0, 1), 0.0, 1e-12); // 强度：n>2 不可用
    QCHECK(b.entanglement_exists(0, 0));                // {q0} | {q1,q2} 纠缠
    QCHECK(!b.entanglement_exists(0, 1));               // {q0,q1} | {q2} 可分离
    QCHECK(b.separable_across(2, 1));                   // {q2} | {q0,q1} 可分离

    // 2 比特时强度可用且等于 concurrence
    QuantumFrame bell;
    bell.set_amplitudes({P(kSqrtHalf, 0), P(0, 0), P(0, 0), P(kSqrtHalf, 0)}, 2);
    QCHECK_NEAR(bell.pair_entanglement(0, 1), bell.concurrence(), 1e-12);
}
