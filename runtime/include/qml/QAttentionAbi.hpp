#pragma once
#include "Inference.hpp"

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

    const size_t anc = rvdl_alloc_qubit_id();
    global_qm->allocate_qubit(anc);

    global_qm->apply_h(anc);
    for (size_t i = 0; i < n; ++i)
        global_qm->apply_cswap(anc, q->hardware_ids[i], k->hardware_ids[i]);
    global_qm->apply_h(anc);

    const double overlap = global_qm->expectation_z(anc);

    global_qm->measure(anc);
    global_qm->release_qubit(anc);
    rvdl_free_qubit_id(anc);

    return overlap;
}
#endif