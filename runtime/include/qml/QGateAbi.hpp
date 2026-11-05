#pragma once
//
// QGateAbi.hpp —— QObject 层量子门 ABI（「提取单 qubit → 门操作 → 重组」的原子化）
//
// QObject 是多 qubit 容器（hardware_ids），而语言层门操作（h/rz/cnot）只作用于
// 单个 Qubit。本桥接把「从 QObject 提取第 i 个 qubit → 施加门 → 重组」原子化为
// 直接作用于 QObject 内部 qubit id 的门操作——由于 Qubit 与 QObject 共享同一
// hardware id，门直接改 id 即可，无需真正的「重组」，也规避了借用视图的释放语义。
//
// 供量子 Transformer 的 FFN（前馈层）在 QObject 数据流上串联参数化门电路：
//   qgate_h(qobj, i) / qgate_x(qobj, i) / qgate_rz(qobj, i, θ) / qgate_cnot(qobj, c, t)
//
// 门 + 测量实现，QVM 与真实 QM 芯片通用（不读态矢量）。
//
#include "Inference.hpp" // ::QObject（hardware_ids）、global_qm

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
    // 跨对象受控门：control 在 QObject a 的 ia 位，target 在 QObject b 的 ib 位。
    // 供 POVM 弱测量（主 qubit 控制辅助 qubit）使用。
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

// 查询 QObject 的 qubit 数（供多 qubit 变分 ansatz 判定态维度对齐感知态）。
int rvdl_qobj_num_qubits(QObject *obj)
{
    return obj ? static_cast<int>(obj->hardware_ids.size()) : 0;
}

// 跨对象受控非门：control 在 a 的 ia 位，target 在 b 的 ib 位。
void rvdl_qgate_cnot_pair(QObject *a, int ia, QObject *b, int ib)
{
    if (qgate_in_range(a, ia) && qgate_in_range(b, ib) && global_qm)
        global_qm->apply_cnot(a->hardware_ids[static_cast<size_t>(ia)],
                              b->hardware_ids[static_cast<size_t>(ib)]);
}
#endif