#pragma once
//
// QStateAbi.hpp —— 量子态量度（保真度 / 纠缠熵）语言层 ABI 桥接
//
// 从后端态矢量（QVM 的 get_state_vector）提取 QObject 占用 qubit 的约化密度矩阵，
// 计算 von Neumann 熵与保真度，暴露为 rvdl 语言层可调用函数。对应 TQNF 设计法则
// 「拓扑-表征分离律」（纠缠由拓扑/纠缠结构决定）。
//
//   rvdl_qstate_entropy(QObject*)          -> double  约化密度矩阵的 von Neumann 熵
//   rvdl_qstate_fidelity(QObject*, QObject*) -> double 两个量子态的保真度 F(ρ,σ)
//
// 仅对维护态矢量的后端（QVM）有意义；无态矢量或规模超限（>10 qubit）返回 -1。
//
#include "Inference.hpp" // ::QObject（hardware_ids）、global_qm
#include "../numqk/ComplexTensor.hpp"
#include <vector>

#ifndef RVDL_HOST_EXPORT
#define RVDL_HOST_EXPORT extern "C" RVDL_RT_API
#endif

namespace qml
{
    // 从全局态矢量提取 ids 占用 qubit 的约化密度矩阵
    inline numqk::CTensor reduced_density_of(qhal::IQuantumBackend *be,
                                             const std::vector<size_t> &ids)
    {
        if (!be)
            return numqk::CTensor({0}, false);
        const size_t N = be->get_num_qubits();
        std::vector<std::complex<double>> sv = be->get_state_vector();
        if (N == 0 || N >= 32 || sv.empty() || ids.empty())
            return numqk::CTensor({0}, false);
        if (sv.size() != (size_t(1) << N))
            return numqk::CTensor({0}, false);

        std::vector<bool> keep(N, false);
        size_t k = 0;
        for (size_t id : ids)
            if (id < N && !keep[id])
            {
                keep[id] = true;
                ++k;
            }
        if (k == 0 || k > 10) // 规模保护：>10 qubit 的密度矩阵 / 特征分解开销过大
            return numqk::CTensor({0}, false);

        numqk::CTensor ket({sv.size()}, false);
        for (size_t i = 0; i < sv.size(); ++i)
            ket.data()[i] = sv[i];

        std::vector<size_t> dims(N, 2);
        return numqk::partial_trace_pure(ket, dims, keep);
    }
}

extern "C"
{
    RVDL_HOST_EXPORT double rvdl_qstate_entropy(QObject *obj);
    RVDL_HOST_EXPORT double rvdl_qstate_fidelity(QObject *a, QObject *b);
}

#if defined(RVDL_RT_BUILD)
double rvdl_qstate_entropy(QObject *obj)
{
    if (!obj || !global_qm)
        return -1.0;
    numqk::CTensor rho = qml::reduced_density_of(global_qm, obj->hardware_ids);
    if (rho.size() == 0)
        return -1.0;
    return numqk::von_neumann_entropy(rho);
}

double rvdl_qstate_fidelity(QObject *a, QObject *b)
{
    if (!a || !b || !global_qm)
        return -1.0;
    numqk::CTensor rho_a = qml::reduced_density_of(global_qm, a->hardware_ids);
    numqk::CTensor rho_b = qml::reduced_density_of(global_qm, b->hardware_ids);
    if (rho_a.size() == 0 || rho_b.size() == 0)
        return -1.0;
    if (rho_a.get_shape() != rho_b.get_shape())
        return -1.0;
    return numqk::fidelity(rho_a, rho_b);
}
#endif