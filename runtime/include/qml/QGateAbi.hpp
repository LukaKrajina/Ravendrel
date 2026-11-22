#pragma once
#include "Inference.hpp"

#ifndef RVDL_HOST_EXPORT
#define RVDL_HOST_EXPORT extern "C" RVDL_RT_API
#endif

extern "C"
{
    RVDL_HOST_EXPORT void rvdl_qgate_h(QObject *qobj, int index);
    RVDL_HOST_EXPORT void rvdl_qgate_x(QObject *qobj, int index);
    RVDL_HOST_EXPORT void rvdl_qgate_rz(QObject *qobj, int index, double angle);
    RVDL_HOST_EXPORT void rvdl_qgate_cnot(QObject *qobj, int control, int target);
    RVDL_HOST_EXPORT double rvdl_qexpect_z(QObject *qobj, int index);
    RVDL_HOST_EXPORT int rvdl_qmeasure(QObject *qobj, int index);
    RVDL_HOST_EXPORT int rvdl_qobj_num_qubits(QObject *qobj);
    RVDL_HOST_EXPORT void rvdl_qgate_cnot_pair(QObject *a, int ia, QObject *b, int ib);
}

#if defined(RVDL_RT_BUILD)
namespace
{
    inline bool qgate_in_range(const QObject *obj, int i)
    {
        return obj && i >= 0 && i < static_cast<int>(obj->hardware_ids.size());
    }
}

void rvdl_qgate_h(QObject *obj, int index)
{
    if (qgate_in_range(obj, index) && global_qm)
        global_qm->apply_h(obj->hardware_ids[static_cast<size_t>(index)]);
}

void rvdl_qgate_x(QObject *obj, int index)
{
    if (qgate_in_range(obj, index) && global_qm)
        global_qm->apply_x(obj->hardware_ids[static_cast<size_t>(index)]);
}

void rvdl_qgate_rz(QObject *obj, int index, double angle)
{
    if (qgate_in_range(obj, index) && global_qm)
        global_qm->apply_rz(obj->hardware_ids[static_cast<size_t>(index)], angle);
}

void rvdl_qgate_cnot(QObject *obj, int control, int target)
{
    if (qgate_in_range(obj, control) && qgate_in_range(obj, target) && global_qm)
        global_qm->apply_cnot(obj->hardware_ids[static_cast<size_t>(control)],
                              obj->hardware_ids[static_cast<size_t>(target)]);
}

double rvdl_qexpect_z(QObject *obj, int index)
{
    if (qgate_in_range(obj, index) && global_qm)
        return global_qm->expectation_z(obj->hardware_ids[static_cast<size_t>(index)]);
    return 0.0;
}

int rvdl_qmeasure(QObject *obj, int index)
{
    if (qgate_in_range(obj, index) && global_qm)
        return global_qm->measure(obj->hardware_ids[static_cast<size_t>(index)]);
    return 0;
}

int rvdl_qobj_num_qubits(QObject *obj)
{
    return obj ? static_cast<int>(obj->hardware_ids.size()) : 0;
}

void rvdl_qgate_cnot_pair(QObject *a, int ia, QObject *b, int ib)
{
    if (qgate_in_range(a, ia) && qgate_in_range(b, ib) && global_qm)
        global_qm->apply_cnot(a->hardware_ids[static_cast<size_t>(ia)],
                              b->hardware_ids[static_cast<size_t>(ib)]);
}
#endif