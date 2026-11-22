#pragma once
#include "Inference.hpp"
#include "../numqk/ComplexTensor.hpp"
#include <vector>

#ifndef RVDL_HOST_EXPORT
#define RVDL_HOST_EXPORT extern "C" RVDL_RT_API
#endif

namespace qml
{
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
        if (k == 0 || k > 10)
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