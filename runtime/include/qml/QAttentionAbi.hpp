#pragma once
//
// QAttentionAbi.hpp —— 量子注意力语言层 ABI 桥接
//
// 量子 Transformer 的核心算子：用 SWAP test 测量两个量子态的重叠度
//   |⟨ψ_q|ψ_k⟩|²，作为经典 attention 中「query·key 相似度」的量子对应。
//
//   rvdl_qattention(QObject* q, QObject* k) -> double
//     返回两个 QObject 态的重叠度 |⟨ψ_q|ψ_k⟩|² ∈ [0,1]
//
// 协议（只需门 + 测量，QVM 与真实 QM 芯片通用，不读态矢量）：
//   H(anc) → CSWAP(anc, q_i, k_i) for each i → H(anc) → read ⟨Z_anc⟩
//   ⟨Z_anc⟩ = 2·P(anc=0) − 1 = |⟨ψ_q|ψ_k⟩|²
// 用 expectation_z 读出：QVM 非破坏一次到位，真实 QM 退化为坍缩采样。
//
#include "Inference.hpp" // ::QObject（hardware_ids）、global_qm、next_available_qubit

#ifndef RVDL_HOST_EXPORT
#define RVDL_HOST_EXPORT extern "C" RVDL_RT_API
#endif

extern "C"
{
    RVDL_HOST_EXPORT double rvdl_qattention(QObject *q, QObject *k);
}

#if defined(RVDL_RT_BUILD)
double rvdl_qattention(QObject *q, QObject *k)
{
    if (!q || !k || !global_qm)
        return 0.0;

    const size_t n = q->hardware_ids.size();
    if (n == 0 || n != k->hardware_ids.size())
        return 0.0;

    // 分配临时 ancilla（复用已释放 id，避免 qubit id 单调增长导致 MPS site 数爆炸）
    const size_t anc = rvdl_alloc_qubit_id();
    global_qm->allocate_qubit(anc);

    // ── SWAP test ────────────────────────────────────────────────
    global_qm->apply_h(anc);
    for (size_t i = 0; i < n; ++i)
        global_qm->apply_cswap(anc, q->hardware_ids[i], k->hardware_ids[i]);
    global_qm->apply_h(anc);

    // ⟨Z_anc⟩ = |⟨ψ_q|ψ_k⟩|²（QVM 非破坏，真实 QM 坍缩采样）
    const double overlap = global_qm->expectation_z(anc);

    // 清理 ancilla：坍缩后释放并回收到空闲池（release_qubit 内部已复位到 |0⟩）。
    global_qm->measure(anc);
    global_qm->release_qubit(anc);
    rvdl_free_qubit_id(anc);

    return overlap;
}
#endif