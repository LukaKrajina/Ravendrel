#pragma once
#include <memory>
#include <string>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

// LLVM Core & IR
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/IRBuilder.h>
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/Error.h"

// LLVM ORC JIT
#include "llvm/ExecutionEngine/Orc/LLJIT.h"
#include "llvm/ExecutionEngine/Orc/ThreadSafeModule.h"
#include "llvm/ExecutionEngine/JITSymbol.h"

// LLVM Process
#include "llvm/ExecutionEngine/Orc/ExecutionUtils.h"

// Ravendrel Core
#include "IQuantumBackend.hpp"

// QLM & QML
#include "../qlm/QLM.hpp"
#include "../../src/QDataEncoder.hpp"
#include "../qml/Inference.hpp"

// QBNS
#include "../qbNs/qbNSBridge.hpp"

// VedaROS
#include "../vedaRos/quantum/qlm.hpp"

// Tracker
#include "CircuitTelemetry.hpp"

#include "../numqk/SoftLogic.hpp"
#include "PolymerSampler.hpp"
#include "../qbNs/SpikeBeliefPropagation.hpp"

// QCOS syscall ABI + QMS MK
#include "QcosSyscall.hpp"
#include "Qms.hpp"

// QChain（rvdl_qchain_* C ABI）
#include "../qchain/bridge/qchain_bridge.hpp"

// Lattice（晶格数组 rvdl_lattice_* C ABI）
#include "Lattice.hpp"

// MoE 对偶融合（rvdl_moe_* / rvdl_curvature_gate / rvdl_dual_fuse /
// rvdl_dual_moe_forward C ABI）—— 让 .rvdl 模型层用自动推导的专家拓扑，
// 不再硬编码 num_experts。
#include "../qml/MoeAbi.hpp"

// 量子时间缩微调度（rvdl_tau_* C ABI）—— 韬定律（ChinaXiv:202605.00224v2）
// 的时间缩微公式改造到量子 AI 层：代际缩放 / 折叠深度 / 齿轮比 / 调度代价 /
// 时间影子价格路由。让 .rvdl 模型层用统一的时间常数 τ 做预算调度。
#include "../qml/TauAbi.hpp"

// 经典 GUI / 图形引擎（cgui_* / cgfx_* C ABI）
#include "ClassicGui.hpp"
#include "ClassicGfx.hpp"

// 量子图形引擎（qgfx_* / qqui_* C ABI）
// 下面 HostApiMap 注册区引用了 rvdl_qgfx_* / rvdl_qqui_* 符号，
// 得显式引入声明，不然编译会报 undeclared identifier。
#include "QuantumGfx.hpp"
#include "QuantumUi.hpp"


// --- QHAL Trampolines ---
static qhal::IQuantumBackend *ActiveBackend = nullptr;

RVDL_HOST_EXPORT void rvdl_alloc(size_t num_qubits);
RVDL_HOST_EXPORT int rvdl_measure(size_t qubit_id);
RVDL_HOST_EXPORT void *rvdl_create_DiracState(int n);
RVDL_HOST_EXPORT void *rvdl_create_BellState();
RVDL_HOST_EXPORT void *rvdl_create_QuantumRegister(int size);
RVDL_HOST_EXPORT void *rvdl_create_basis_state(double theta, double phi, int value);
RVDL_HOST_EXPORT int rvdl_measure_object(void *ptr);
RVDL_HOST_EXPORT void rvdl_chkstk_stub();
RVDL_HOST_EXPORT void rvdl_release_object(void *ptr);
RVDL_HOST_EXPORT void *rvdl_encode_text(const char *text);
RVDL_HOST_EXPORT void *rvdl_encode_amplitudes(const char *text, int num_qubits);
RVDL_HOST_EXPORT void *rvdl_encode_image(const char *path, int num_qubits);
RVDL_HOST_EXPORT void *rvdl_encode_adaptive(const char *text);
RVDL_HOST_EXPORT void *rvdl_qlm_invoke(void *qobj_ptr, int epochs, double lr);
RVDL_HOST_EXPORT void rvdl_qkm_export(void *model_ptr, const char *path);
RVDL_HOST_EXPORT double rvdl_surrogate(double x, double theta, double gamma);
RVDL_HOST_EXPORT double rvdl_tanh_quantize(double w, double alpha, int bits);
RVDL_HOST_EXPORT double rvdl_lif_step(double v, double current, double tau, double theta);
RVDL_HOST_EXPORT double rvdl_mellowmax2(double a, double b, double omega);
RVDL_HOST_EXPORT double rvdl_logsumexp2(double a, double b, double beta);
RVDL_HOST_EXPORT double rvdl_boltzmann2(double a, double b, double beta);
RVDL_HOST_EXPORT double rvdl_tnorm_luk(double a, double b);
RVDL_HOST_EXPORT double rvdl_tnorm_prod(double a, double b);
RVDL_HOST_EXPORT double rvdl_tnorm_godel(double a, double b);
RVDL_HOST_EXPORT double rvdl_polymer_weight(double beta, double h, double len);
RVDL_HOST_EXPORT double rvdl_polymer_mix_bound(double n, double eps);
RVDL_HOST_EXPORT double rvdl_qms_gap(std::int32_t model, double p, double q);
RVDL_HOST_EXPORT double rvdl_mix_bound(double gap, double n, double eps);
RVDL_HOST_EXPORT double rvdl_qms_conc(double gap, double eps);
RVDL_HOST_EXPORT int rvdl_dla_dim(const char *spec, int n_qubits);
RVDL_HOST_EXPORT double rvdl_qstate_entropy(QObject *obj);
RVDL_HOST_EXPORT double rvdl_qstate_fidelity(QObject *a, QObject *b);
RVDL_HOST_EXPORT double rvdl_qstate_purity(QObject *obj);
RVDL_HOST_EXPORT double rvdl_qstate_linear_entropy(QObject *obj);
RVDL_HOST_EXPORT double rvdl_qstate_renyi_entropy(QObject *obj, double alpha);
RVDL_HOST_EXPORT double rvdl_math_dot(const double *a, const double *b, int32_t n);
RVDL_HOST_EXPORT double rvdl_math_norm(const double *a, int32_t n);
RVDL_HOST_EXPORT double rvdl_qattention(QObject *q, QObject *k);
RVDL_HOST_EXPORT double rvdl_shannon4(int32_t n0, int32_t n1, int32_t n2, int32_t n3);
RVDL_HOST_EXPORT double rvdl_shannon8(int32_t n0, int32_t n1, int32_t n2, int32_t n3,
                                     int32_t n4, int32_t n5, int32_t n6, int32_t n7);
RVDL_HOST_EXPORT void rvdl_qgate_h(QObject *qobj, int index);
RVDL_HOST_EXPORT void rvdl_qgate_x(QObject *qobj, int index);
RVDL_HOST_EXPORT void rvdl_qgate_rz(QObject *qobj, int index, double angle);
RVDL_HOST_EXPORT void rvdl_qgate_cnot(QObject *qobj, int control, int target);
RVDL_HOST_EXPORT double rvdl_qexpect_z(QObject *qobj, int index);
RVDL_HOST_EXPORT int rvdl_qmeasure(QObject *qobj, int index);
RVDL_HOST_EXPORT int rvdl_qobj_num_qubits(QObject *qobj);
RVDL_HOST_EXPORT void rvdl_qgate_cnot_pair(QObject *a, int ia, QObject *b, int ib);
RVDL_HOST_EXPORT double rvdl_geodesic_distance(QObject *a, QObject *b);
RVDL_HOST_EXPORT double rvdl_inversion(double R);
RVDL_HOST_EXPORT double rvdl_hyperbolic_metric(double x);
RVDL_HOST_EXPORT double rvdl_hyperbolic_distance(double x, double y);
RVDL_HOST_EXPORT double rvdl_retrocausal_imax(int32_t kind, double p);
RVDL_HOST_EXPORT double rvdl_retrocausal_idoe(int32_t kind, double p);
RVDL_HOST_EXPORT double rvdl_retrocausal_q_capacity(int32_t kind, double p);
RVDL_HOST_EXPORT double rvdl_retrocausal_c_capacity(int32_t kind, double p);
RVDL_HOST_EXPORT double rvdl_retrocausal_q_one_shot(int32_t kind, double p, double eps);
RVDL_HOST_EXPORT double rvdl_retrocausal_gain(int32_t kind, double p);
RVDL_HOST_EXPORT double rvdl_retrocausal_deformed(int32_t kind, double p, double q);
RVDL_HOST_EXPORT double rvdl_retrocausal_ctc_q_capacity(int32_t n, double theta, double mu2, double lambda);
RVDL_HOST_EXPORT double rvdl_retrocausal_ctc_c_capacity(int32_t n, double theta, double mu2, double lambda);
RVDL_HOST_EXPORT double rvdl_retrocausal_ctc_gain(int32_t n, double theta, double mu2, double lambda);
RVDL_HOST_EXPORT double rvdl_retrocausal_ctc_dephasing(double theta);

extern "C" void *__quantum__rt__qubit_allocate();
extern "C" void  __quantum__rt__qubit_release(void *qubit);
extern "C" int   __quantum__qis__measure_int(void *qubit);
extern "C" void __quantum__qis__h(void *qubit);
extern "C" void __quantum__qis__x(void *qubit);
extern "C" void __quantum__qis__y(void *qubit);
extern "C" void __quantum__qis__z(void *qubit);
extern "C" void __quantum__qis__s(void *qubit);
extern "C" void __quantum__qis__t(void *qubit);
extern "C" void __quantum__qis__rz(double angle, void *qubit);
extern "C" void __quantum__qis__rx(double angle, void *qubit);
extern "C" void __quantum__qis__ry(double angle, void *qubit);
extern "C" void __quantum__qis__cnot(void *control, void *target);
extern "C" void __quantum__qis__toffoli(void *c1, void *c2, void *target);
extern "C" void __quantum__qis__swap(void *a, void *b);
extern "C" void __quantum__qis__qft(int num_qubits);
extern "C" void __quantum__qis__iqft(int num_qubits);
extern "C" void __quantum__qis__cqft(void *control, int num_qubits);
extern "C" void __quantum__qis__braid(void *a, void *b);
extern "C" void __quantum__qis__cbraid(void *control, void *a, void *b);
// 并发线程（spawn）：以 std::thread 启动独立线程函数（带 env 闭包捕获）并 detach
extern "C" void rvdl_spawn(void (*fn)(void *), void *env);
// 受控门（可逆编织 @[steer]）：cx / ch / crz / cswap / c_toffoli
extern "C" void __quantum__qis__cx(void *control, void *target);
extern "C" void __quantum__qis__ch(void *control, void *target);
extern "C" void __quantum__qis__crz(void *control, void *target, double angle);
extern "C" void __quantum__qis__cswap(void *control, void *a, void *b);
extern "C" void __quantum__qis__c_toffoli(void *control, void *a, void *b, void *target);
// 噪声通道注入（@[noise]/@[coherence] 物理特性）
extern "C" void __quantum__qis__apply_noise(void *qubit, int channel, double param);
extern "C" int  __quantum__qis__measure_basis(void *qubit, char basis);
// 多维标签函数执行拓扑（@layer）调度入口：由 IR 生成的 rvdl_topology_entry 调用
extern "C" int32_t rvdl_runtime_run_topology(const char *json);

#if defined(RVDL_RT_BUILD)
void rvdl_alloc(size_t num_qubits)
{
    if (ActiveBackend)
        ActiveBackend->allocate_qubits(num_qubits);
}

int rvdl_measure(size_t qubit_id)
{
    if (ActiveBackend)
        return ActiveBackend->measure(qubit_id);
    return -1;
}

void *rvdl_create_DiracState(int n)
{
    QObject *obj = new QObject();
    if (n < 1)
        n = 1;
    for (int i = 0; i < n; ++i)
    {
        size_t id = rvdl_alloc_qubit_id();
        obj->hardware_ids.push_back(id);
        if (ActiveBackend)
            ActiveBackend->allocate_qubit(id);
    }
    qhal::CircuitTelemetry::get_instance().log_object(
        "DiracState", std::vector<int>(obj->hardware_ids.begin(), obj->hardware_ids.end()));
    return obj;
}

void *rvdl_create_basis_state(double theta, double phi, int value)
{
    QObject *obj = new QObject();
    size_t q = rvdl_alloc_qubit_id();
    obj->hardware_ids = {q};

    if (ActiveBackend)
    {
        ActiveBackend->allocate_qubit(q);
        // 把 |0⟩ 旋转到任意基 (θ, φ) 的 + 本征态；value != 0 时取 - 本征态（X 翻转）。
        ActiveBackend->apply_basis(q, theta, phi);
        if (value != 0)
        {
            ActiveBackend->apply_x(q);
        }
        qhal::CircuitTelemetry::get_instance().log_gate("BASIS", q);
    }

    qhal::CircuitTelemetry::get_instance().log_object(
        "BasisState", std::vector<int>{static_cast<int>(q)}, theta, phi);

    return obj;
}

void *rvdl_create_BellState()
{
    QObject *obj = new QObject();
    size_t q0 = rvdl_alloc_qubit_id();
    size_t q1 = rvdl_alloc_qubit_id();
    obj->hardware_ids = {q0, q1};

    if (ActiveBackend)
    {
        ActiveBackend->allocate_qubit(q0);
        ActiveBackend->allocate_qubit(q1);
        ActiveBackend->apply_h(q0);
        qhal::CircuitTelemetry::get_instance().log_gate("H", q0);
        ActiveBackend->apply_cnot(q0, q1);
        qhal::CircuitTelemetry::get_instance().log_gate("CNOT", q1, q0);
    }

    qhal::CircuitTelemetry::get_instance().log_object(
        "BellState", std::vector<int>{static_cast<int>(q0), static_cast<int>(q1)});

    return obj;
}

void *rvdl_create_QuantumRegister(int size)
{
    QObject *obj = new QObject();
    for (int i = 0; i < size; ++i)
    {
        size_t id = rvdl_alloc_qubit_id();
        obj->hardware_ids.push_back(id);
        if (ActiveBackend)
            ActiveBackend->allocate_qubit(id);
    }
    qhal::CircuitTelemetry::get_instance().log_object(
        "QuantumRegister", std::vector<int>(obj->hardware_ids.begin(), obj->hardware_ids.end()));
    return obj;
}

int rvdl_measure_object(void *ptr)
{
    QObject *obj = static_cast<QObject *>(ptr);
    if (ActiveBackend && obj && !obj->hardware_ids.empty())
    {
        return ActiveBackend->measure(obj->hardware_ids[0]);
    }
    return 0;
}

void rvdl_chkstk_stub()
{
}

void rvdl_release_object(void *ptr)
{
    QObject *obj = static_cast<QObject *>(ptr);
    if (!obj)
        return;

    if (obj->data_kind == QOBJ_DATA_SHARED)
    {
        // 共享对象（encode_text / encode_amplitudes / encode_image / encode_adaptive）：
        // 后端 qubit 由 rvdl::QObject 析构统一 release（避免双重 release）。先销毁
        // shared_ptr（其析构完成后端 qubit 的复位 + 收缩），再把全局 id 回收到空闲池。
        if (obj->qlm_data)
            delete static_cast<std::shared_ptr<rvdl::QObject> *>(obj->qlm_data);
        for (size_t q_id : obj->hardware_ids)
            rvdl_free_qubit_id(q_id);
    }
    else
    {
        // 普通对象（rvdl_create_*）与 encode_string（QOBJ_DATA_STRING）：
        // 后端 release + 全局 id 回收。
        if (ActiveBackend)
            for (size_t q_id : obj->hardware_ids)
                ActiveBackend->release_qubit(q_id);
        for (size_t q_id : obj->hardware_ids)
            rvdl_free_qubit_id(q_id);
        if (obj->qlm_data && obj->data_kind == QOBJ_DATA_STRING)
            delete static_cast<std::string *>(obj->qlm_data);
    }

    obj->qlm_data = nullptr;
    obj->data_kind = QOBJ_DATA_NONE;
    delete obj;
}

void *rvdl_encode_text(const char *text)
{
    if (!ActiveBackend)
        return nullptr;

    rvdl::qml::QDataEncoder encoder(ActiveBackend);
    auto sp = encoder.text_to_basis(std::string(text));

    QObject *obj = new QObject();
    obj->hardware_ids = sp->get_ids();
    obj->qlm_data = new std::shared_ptr<rvdl::QObject>(sp);
    obj->data_kind = QOBJ_DATA_SHARED;

    return obj;
}

void *rvdl_encode_amplitudes(const char *text, int num_qubits)
{
    if (!ActiveBackend || num_qubits <= 0 || num_qubits > 30)
        return nullptr;

    rvdl::qml::QDataEncoder encoder(ActiveBackend);
    auto sp = encoder.text_to_amplitudes(std::string(text), static_cast<size_t>(num_qubits));

    QObject *obj = new QObject();
    obj->hardware_ids = sp->get_ids();
    obj->qlm_data = new std::shared_ptr<rvdl::QObject>(sp);
    obj->data_kind = QOBJ_DATA_SHARED;

    return obj;
}

void *rvdl_encode_adaptive(const char *text)
{
    if (!ActiveBackend)
        return nullptr;

    rvdl::qml::QDataEncoder encoder(ActiveBackend);
    auto sp = encoder.encode_adaptive(std::string(text));

    QObject *obj = new QObject();
    obj->hardware_ids = sp->get_ids();
    obj->qlm_data = new std::shared_ptr<rvdl::QObject>(sp);
    obj->data_kind = QOBJ_DATA_SHARED;

    return obj;
}

void *rvdl_encode_image(const char *path, int num_qubits)
{
    if (!ActiveBackend || !path || num_qubits <= 0 || num_qubits > 20)
        return nullptr;

    rvdl::qml::QDataEncoder encoder(ActiveBackend);
    auto sp = encoder.image_to_amplitudes(std::string(path), static_cast<size_t>(num_qubits));

    QObject *obj = new QObject();
    obj->hardware_ids = sp->get_ids();
    obj->qlm_data = new std::shared_ptr<rvdl::QObject>(sp);
    obj->data_kind = QOBJ_DATA_SHARED;

    return obj;
}

void *rvdl_qlm_invoke(void *qobj_ptr, int epochs, double lr)
{
    QObject *obj = static_cast<QObject *>(qobj_ptr);
    QModelJob *job = new QModelJob();

    job->prompt_data = obj->qlm_data;
    job->epochs = epochs;
    job->lr = lr;

    return job;
}

void rvdl_qkm_export(void *model_ptr, const char *path)
{
    QModelJob *job = static_cast<QModelJob *>(model_ptr);

    if (job && job->prompt_data && ActiveBackend)
    {
        auto *sp = static_cast<std::shared_ptr<rvdl::QObject> *>(job->prompt_data);
        if ((*sp)->qlm_data == nullptr) {
             (*sp)->qlm_data = new uint8_t(0);
        }

        std::vector<std::shared_ptr<rvdl::QObject>> dataset_wrapper;
        dataset_wrapper.push_back(*sp);
        qlm::QLM learning_machine(ActiveBackend, 8*2, 6);
        learning_machine.train_and_export(job->epochs, job->lr, std::string(path), dataset_wrapper);
        delete static_cast<uint8_t*>((*sp)->qlm_data);
        (*sp)->qlm_data = nullptr;
    }

    delete job;
}

// 神经/软逻辑/polymer 原语定义已下沉到 qml/SoftLogicAbi.hpp（见上方 include）。

void *__quantum__rt__qubit_allocate()
{
    if (!ActiveBackend)
        return nullptr;
    size_t id = rvdl_alloc_qubit_id();
    ActiveBackend->allocate_qubit(id);
    return new size_t(id);
}

void __quantum__rt__qubit_release(void *qubit)
{
    if (!qubit)
        return;
    size_t *id = static_cast<size_t *>(qubit);
    if (ActiveBackend)
        ActiveBackend->release_qubit(*id);
    rvdl_free_qubit_id(*id);
    delete id;
}

int __quantum__qis__measure_int(void *qubit)
{
    if (!qubit || !ActiveBackend)
        return 0;
    size_t *id = static_cast<size_t *>(qubit);
    return ActiveBackend->measure(*id);
}

static size_t qubit_handle_id(void *qubit)
{
    return qubit ? *static_cast<size_t *>(qubit) : 0;
}

void __quantum__qis__h(void *qubit)
{
    if (ActiveBackend) ActiveBackend->apply_h(qubit_handle_id(qubit));
}

void __quantum__qis__x(void *qubit)
{
    if (ActiveBackend) ActiveBackend->apply_x(qubit_handle_id(qubit));
}

void __quantum__qis__y(void *qubit)
{
    if (ActiveBackend) ActiveBackend->apply_y(qubit_handle_id(qubit));
}

void __quantum__qis__z(void *qubit)
{
    if (ActiveBackend) ActiveBackend->apply_z(qubit_handle_id(qubit));
}

void __quantum__qis__s(void *qubit)
{
    if (ActiveBackend) ActiveBackend->apply_s(qubit_handle_id(qubit));
}

void __quantum__qis__t(void *qubit)
{
    if (ActiveBackend) ActiveBackend->apply_t(qubit_handle_id(qubit));
}

void __quantum__qis__rz(double angle, void *qubit)
{
    if (ActiveBackend) ActiveBackend->apply_rz(qubit_handle_id(qubit), angle);
}

void __quantum__qis__rx(double angle, void *qubit)
{
    if (ActiveBackend) ActiveBackend->apply_rx(qubit_handle_id(qubit), angle);
}

void __quantum__qis__ry(double angle, void *qubit)
{
    if (ActiveBackend) ActiveBackend->apply_ry(qubit_handle_id(qubit), angle);
}

void __quantum__qis__cnot(void *control, void *target)
{
    if (ActiveBackend) ActiveBackend->apply_cnot(qubit_handle_id(control), qubit_handle_id(target));
}

void __quantum__qis__toffoli(void *c1, void *c2, void *target)
{
    if (ActiveBackend) ActiveBackend->apply_toffoli(qubit_handle_id(c1), qubit_handle_id(c2), qubit_handle_id(target));
}

void __quantum__qis__swap(void *a, void *b)
{
    if (ActiveBackend) ActiveBackend->apply_swap(qubit_handle_id(a), qubit_handle_id(b));
}

void __quantum__qis__qft(int num_qubits)
{
    if (ActiveBackend && num_qubits > 0)
        ActiveBackend->apply_qft(0, static_cast<size_t>(num_qubits - 1));
}

void __quantum__qis__iqft(int num_qubits)
{
    if (ActiveBackend && num_qubits > 0)
        ActiveBackend->apply_iqft(0, static_cast<size_t>(num_qubits - 1));
}

void __quantum__qis__cqft(void *control, int num_qubits)
{
    if (ActiveBackend && num_qubits > 0)
        ActiveBackend->apply_cqft(qubit_handle_id(control), 0, static_cast<size_t>(num_qubits - 1));
}

void __quantum__qis__cbraid(void *control, void *a, void *b)
{
    if (ActiveBackend) ActiveBackend->apply_cbraid(qubit_handle_id(control), qubit_handle_id(a), qubit_handle_id(b));
}

void rvdl_spawn(void (*fn)(void *), void *env)
{
    if (fn)
    {
        std::thread t(fn, env);
        t.detach();
    }
}

void __quantum__qis__braid(void *a, void *b)
{
    if (ActiveBackend) ActiveBackend->apply_braid(qubit_handle_id(a), qubit_handle_id(b));
}

void __quantum__qis__cx(void *control, void *target)
{
    if (ActiveBackend) ActiveBackend->apply_cx(qubit_handle_id(control), qubit_handle_id(target));
}

void __quantum__qis__ch(void *control, void *target)
{
    if (ActiveBackend) ActiveBackend->apply_ch(qubit_handle_id(control), qubit_handle_id(target));
}

void __quantum__qis__crz(void *control, void *target, double angle)
{
    if (ActiveBackend) ActiveBackend->apply_crz(qubit_handle_id(control), qubit_handle_id(target), angle);
}

void __quantum__qis__cswap(void *control, void *a, void *b)
{
    if (ActiveBackend) ActiveBackend->apply_cswap(qubit_handle_id(control), qubit_handle_id(a), qubit_handle_id(b));
}

void __quantum__qis__c_toffoli(void *control, void *a, void *b, void *target)
{
    if (ActiveBackend) ActiveBackend->apply_c_toffoli(qubit_handle_id(control), qubit_handle_id(a), qubit_handle_id(b), qubit_handle_id(target));
}

void __quantum__qis__apply_noise(void *qubit, int channel, double param)
{
    if (ActiveBackend) ActiveBackend->apply_noise(qubit_handle_id(qubit), channel, param);
}

int __quantum__qis__measure_basis(void *qubit, char basis)
{
    if (!ActiveBackend)
        return 0;
    return ActiveBackend->measure_basis(qubit_handle_id(qubit), basis);
}
#endif

namespace qhal
{
    class RVDL_RT_API JIT
    {
    private:
        std::unique_ptr<llvm::orc::LLJIT> JIT_ptr;
        static void handle_llvm_error(llvm::Error Err)
        {
            llvm::handleAllErrors(std::move(Err), [](llvm::ErrorInfoBase &EIB)
                                  { std::cerr << "Ravendrel JIT Error: " << EIB.message() << std::endl; });
        }

    public:
        JIT(qhal::IQuantumBackend *backend)
        {
            ActiveBackend = backend;
            llvm::InitializeNativeTarget();
            llvm::InitializeNativeTargetAsmPrinter();
            llvm::InitializeNativeTargetAsmParser();

            auto JITOrErr = llvm::orc::LLJITBuilder().create();
            if (!JITOrErr)
            {
                handle_llvm_error(JITOrErr.takeError());
                throw std::runtime_error("Failed to initialize Ravendrel LLJIT.");
            }
            JIT_ptr = std::move(*JITOrErr);
            bind_hardware_api();
        }

        ~JIT()
        {
            ActiveBackend = nullptr;
        }

        void add_ir_module(std::unique_ptr<llvm::Module> M, std::unique_ptr<llvm::LLVMContext> Ctx)
        {
            llvm::orc::ThreadSafeModule TSM(std::move(M), std::move(Ctx));
            auto Err = JIT_ptr->addIRModule(std::move(TSM));
            if (Err)
            {
                handle_llvm_error(std::move(Err));
            }
        }

        template <typename Signature>
        std::function<Signature> get_function(const std::string &Name)
        {
            auto ExprSymbol = JIT_ptr->lookup(Name);
            if (!ExprSymbol)
            {
                handle_llvm_error(ExprSymbol.takeError());
                return nullptr;
            }

            auto *FnPtr = llvm::jitTargetAddressToPointer<Signature *>(ExprSymbol->getValue());
            return [FnPtr](auto &&...args)
            {
                return FnPtr(std::forward<decltype(args)>(args)...);
            };
        }

        // 动态绑定外部符号（供 .rvdli 导出函数注入主 JIT，实现主程序 import .rvdli）
        void bind_symbol(const std::string &Name, void *Addr)
        {
            auto &Dylib = JIT_ptr->getMainJITDylib();
            llvm::orc::MangleAndInterner Mangle(JIT_ptr->getExecutionSession(), JIT_ptr->getDataLayout());
            llvm::orc::SymbolMap Map;
            Map[Mangle(Name)] = llvm::orc::ExecutorSymbolDef(
                llvm::orc::ExecutorAddr::fromPtr(Addr),
                llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);
            auto Err = Dylib.define(llvm::orc::absoluteSymbols(Map));
            if (Err)
                handle_llvm_error(std::move(Err));
        }

    private:
        void bind_hardware_api()
        {
            auto &MainDylib = JIT_ptr->getMainJITDylib();

            auto Generator = llvm::orc::DynamicLibrarySearchGenerator::GetForCurrentProcess(
                JIT_ptr->getDataLayout().getGlobalPrefix());
            if (Generator)
            {
                MainDylib.addGenerator(std::move(*Generator));
            }

            llvm::orc::MangleAndInterner Mangle(JIT_ptr->getExecutionSession(), JIT_ptr->getDataLayout());

            llvm::orc::SymbolMap HostApiMap;

            HostApiMap[Mangle("___chkstk_ms")] = llvm::orc::ExecutorSymbolDef(
                llvm::orc::ExecutorAddr::fromPtr(&rvdl_chkstk_stub),
                llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);

            HostApiMap[Mangle("rvdl_alloc")] = llvm::orc::ExecutorSymbolDef(
                llvm::orc::ExecutorAddr::fromPtr(&rvdl_alloc),
                llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);

            HostApiMap[Mangle("rvdl_measure")] = llvm::orc::ExecutorSymbolDef(
                llvm::orc::ExecutorAddr::fromPtr(&rvdl_measure),
                llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);

            HostApiMap[Mangle("rvdl_create_DiracState")] = llvm::orc::ExecutorSymbolDef(
                llvm::orc::ExecutorAddr::fromPtr(&rvdl_create_DiracState),
                llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);

            HostApiMap[Mangle("rvdl_create_BellState")] = llvm::orc::ExecutorSymbolDef(
                llvm::orc::ExecutorAddr::fromPtr(&rvdl_create_BellState),
                llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);

            HostApiMap[Mangle("rvdl_create_QuantumRegister")] = llvm::orc::ExecutorSymbolDef(
                llvm::orc::ExecutorAddr::fromPtr(&rvdl_create_QuantumRegister),
                llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);

            HostApiMap[Mangle("rvdl_create_basis_state")] = llvm::orc::ExecutorSymbolDef(
                llvm::orc::ExecutorAddr::fromPtr(&rvdl_create_basis_state),
                llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);

            HostApiMap[Mangle("rvdl_measure_object")] = llvm::orc::ExecutorSymbolDef(
                llvm::orc::ExecutorAddr::fromPtr(&rvdl_measure_object),
                llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);

            HostApiMap[Mangle("rvdl_release_object")] = llvm::orc::ExecutorSymbolDef(
                llvm::orc::ExecutorAddr::fromPtr(&rvdl_release_object),
                llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);

            HostApiMap[Mangle("rvdl_encode_text")] = llvm::orc::ExecutorSymbolDef(
                llvm::orc::ExecutorAddr::fromPtr(&rvdl_encode_text),
                llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);
            HostApiMap[Mangle("rvdl_encode_amplitudes")] = llvm::orc::ExecutorSymbolDef(
                llvm::orc::ExecutorAddr::fromPtr(&rvdl_encode_amplitudes),
                llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);
            HostApiMap[Mangle("rvdl_encode_image")] = llvm::orc::ExecutorSymbolDef(
                llvm::orc::ExecutorAddr::fromPtr(&rvdl_encode_image),
                llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);
            HostApiMap[Mangle("rvdl_encode_adaptive")] = llvm::orc::ExecutorSymbolDef(
                llvm::orc::ExecutorAddr::fromPtr(&rvdl_encode_adaptive),
                llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);

            HostApiMap[Mangle("rvdl_qlm_invoke")] = llvm::orc::ExecutorSymbolDef(
                llvm::orc::ExecutorAddr::fromPtr(&rvdl_qlm_invoke),
                llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);

            HostApiMap[Mangle("rvdl_qkm_export")] = llvm::orc::ExecutorSymbolDef(
                llvm::orc::ExecutorAddr::fromPtr(&rvdl_qkm_export),
                llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);

            HostApiMap[Mangle("rvdl_qlm_load")] = llvm::orc::ExecutorSymbolDef(
                llvm::orc::ExecutorAddr::fromPtr(&rvdl_qlm_load),
                llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);

            HostApiMap[Mangle("rvdl_encode_string")] = llvm::orc::ExecutorSymbolDef(
                llvm::orc::ExecutorAddr::fromPtr(&rvdl_encode_string),
                llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);

            HostApiMap[Mangle("rvdl_qlm_forward")] = llvm::orc::ExecutorSymbolDef(
                llvm::orc::ExecutorAddr::fromPtr(&rvdl_qlm_forward),
                llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);

            HostApiMap[Mangle("rvdl_decode_string")] = llvm::orc::ExecutorSymbolDef(
                llvm::orc::ExecutorAddr::fromPtr(&rvdl_decode_string),
                llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);

            HostApiMap[Mangle("rvdl_mind_read")] = llvm::orc::ExecutorSymbolDef(
                llvm::orc::ExecutorAddr::fromPtr(&rvdl_mind_read),
                llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);

            HostApiMap[Mangle("rvdl_mind_train")] = llvm::orc::ExecutorSymbolDef(
                llvm::orc::ExecutorAddr::fromPtr(&rvdl_mind_train),
                llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);

            HostApiMap[Mangle("rvdl_mind_feedback")] = llvm::orc::ExecutorSymbolDef(
                llvm::orc::ExecutorAddr::fromPtr(&rvdl_mind_feedback),
                llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);

            HostApiMap[Mangle("rvdl_veda_qlm_train")] = llvm::orc::ExecutorSymbolDef(
                llvm::orc::ExecutorAddr::fromPtr(&rvdl_veda_qlm_train),
                llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);

            HostApiMap[Mangle("rvdl_surrogate")] = llvm::orc::ExecutorSymbolDef(
                llvm::orc::ExecutorAddr::fromPtr(&rvdl_surrogate),
                llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);
            HostApiMap[Mangle("rvdl_tanh_quantize")] = llvm::orc::ExecutorSymbolDef(
                llvm::orc::ExecutorAddr::fromPtr(&rvdl_tanh_quantize),
                llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);
            HostApiMap[Mangle("rvdl_lif_step")] = llvm::orc::ExecutorSymbolDef(
                llvm::orc::ExecutorAddr::fromPtr(&rvdl_lif_step),
                llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);
            HostApiMap[Mangle("rvdl_mellowmax2")] = llvm::orc::ExecutorSymbolDef(
                llvm::orc::ExecutorAddr::fromPtr(&rvdl_mellowmax2),
                llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);
            HostApiMap[Mangle("rvdl_logsumexp2")] = llvm::orc::ExecutorSymbolDef(
                llvm::orc::ExecutorAddr::fromPtr(&rvdl_logsumexp2),
                llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);
            HostApiMap[Mangle("rvdl_boltzmann2")] = llvm::orc::ExecutorSymbolDef(
                llvm::orc::ExecutorAddr::fromPtr(&rvdl_boltzmann2),
                llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);
            HostApiMap[Mangle("rvdl_tnorm_luk")] = llvm::orc::ExecutorSymbolDef(
                llvm::orc::ExecutorAddr::fromPtr(&rvdl_tnorm_luk),
                llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);
            HostApiMap[Mangle("rvdl_tnorm_prod")] = llvm::orc::ExecutorSymbolDef(
                llvm::orc::ExecutorAddr::fromPtr(&rvdl_tnorm_prod),
                llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);
            HostApiMap[Mangle("rvdl_tnorm_godel")] = llvm::orc::ExecutorSymbolDef(
                llvm::orc::ExecutorAddr::fromPtr(&rvdl_tnorm_godel),
                llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);
            HostApiMap[Mangle("rvdl_polymer_weight")] = llvm::orc::ExecutorSymbolDef(
                llvm::orc::ExecutorAddr::fromPtr(&rvdl_polymer_weight),
                llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);
            HostApiMap[Mangle("rvdl_polymer_mix_bound")] = llvm::orc::ExecutorSymbolDef(
                llvm::orc::ExecutorAddr::fromPtr(&rvdl_polymer_mix_bound),
                llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);

            // ─── MoE 对偶融合 ─────────────────────────────────────────
            HostApiMap[Mangle("rvdl_moe_groups")] = llvm::orc::ExecutorSymbolDef(
                llvm::orc::ExecutorAddr::fromPtr(&rvdl_moe_groups),
                llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);
            HostApiMap[Mangle("rvdl_moe_experts_per")] = llvm::orc::ExecutorSymbolDef(
                llvm::orc::ExecutorAddr::fromPtr(&rvdl_moe_experts_per),
                llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);
            HostApiMap[Mangle("rvdl_moe_virtual_experts")] = llvm::orc::ExecutorSymbolDef(
                llvm::orc::ExecutorAddr::fromPtr(&rvdl_moe_virtual_experts),
                llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);
            HostApiMap[Mangle("rvdl_moe_savings")] = llvm::orc::ExecutorSymbolDef(
                llvm::orc::ExecutorAddr::fromPtr(&rvdl_moe_savings),
                llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);
            HostApiMap[Mangle("rvdl_curvature_gate")] = llvm::orc::ExecutorSymbolDef(
                llvm::orc::ExecutorAddr::fromPtr(&rvdl_curvature_gate),
                llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);
            HostApiMap[Mangle("rvdl_dual_fuse")] = llvm::orc::ExecutorSymbolDef(
                llvm::orc::ExecutorAddr::fromPtr(&rvdl_dual_fuse),
                llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);
            HostApiMap[Mangle("rvdl_dual_moe_forward")] = llvm::orc::ExecutorSymbolDef(
                llvm::orc::ExecutorAddr::fromPtr(&rvdl_dual_moe_forward),
                llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);

            // ─── 量子时间缩微调度（tau 缩放）───────────────────────────
            HostApiMap[Mangle("rvdl_tau_alpha")] = llvm::orc::ExecutorSymbolDef(
                llvm::orc::ExecutorAddr::fromPtr(&rvdl_tau_alpha),
                llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);
            HostApiMap[Mangle("rvdl_tau_generational")] = llvm::orc::ExecutorSymbolDef(
                llvm::orc::ExecutorAddr::fromPtr(&rvdl_tau_generational),
                llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);
            HostApiMap[Mangle("rvdl_tau_fold_depth")] = llvm::orc::ExecutorSymbolDef(
                llvm::orc::ExecutorAddr::fromPtr(&rvdl_tau_fold_depth),
                llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);
            HostApiMap[Mangle("rvdl_tau_gear_ratio")] = llvm::orc::ExecutorSymbolDef(
                llvm::orc::ExecutorAddr::fromPtr(&rvdl_tau_gear_ratio),
                llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);
            HostApiMap[Mangle("rvdl_tau_route_cost")] = llvm::orc::ExecutorSymbolDef(
                llvm::orc::ExecutorAddr::fromPtr(&rvdl_tau_route_cost),
                llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);
            HostApiMap[Mangle("rvdl_tau_route_price")] = llvm::orc::ExecutorSymbolDef(
                llvm::orc::ExecutorAddr::fromPtr(&rvdl_tau_route_price),
                llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);

            // --- TQNF 拓扑量子神经场 ABI ---
            HostApiMap[Mangle("rvdl_dla_dim")] = llvm::orc::ExecutorSymbolDef(
                llvm::orc::ExecutorAddr::fromPtr(&rvdl_dla_dim),
                llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);
            HostApiMap[Mangle("rvdl_qstate_entropy")] = llvm::orc::ExecutorSymbolDef(
                llvm::orc::ExecutorAddr::fromPtr(&rvdl_qstate_entropy),
                llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);
            HostApiMap[Mangle("rvdl_qstate_fidelity")] = llvm::orc::ExecutorSymbolDef(
                llvm::orc::ExecutorAddr::fromPtr(&rvdl_qstate_fidelity),
                llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);
            HostApiMap[Mangle("rvdl_qstate_purity")] = llvm::orc::ExecutorSymbolDef(
                llvm::orc::ExecutorAddr::fromPtr(&rvdl_qstate_purity),
                llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);
            HostApiMap[Mangle("rvdl_qstate_linear_entropy")] = llvm::orc::ExecutorSymbolDef(
                llvm::orc::ExecutorAddr::fromPtr(&rvdl_qstate_linear_entropy),
                llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);
            HostApiMap[Mangle("rvdl_qstate_renyi_entropy")] = llvm::orc::ExecutorSymbolDef(
                llvm::orc::ExecutorAddr::fromPtr(&rvdl_qstate_renyi_entropy),
                llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);
            HostApiMap[Mangle("rvdl_math_dot")] = llvm::orc::ExecutorSymbolDef(
                llvm::orc::ExecutorAddr::fromPtr(&rvdl_math_dot),
                llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);
            HostApiMap[Mangle("rvdl_math_norm")] = llvm::orc::ExecutorSymbolDef(
                llvm::orc::ExecutorAddr::fromPtr(&rvdl_math_norm),
                llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);
            HostApiMap[Mangle("rvdl_qattention")] = llvm::orc::ExecutorSymbolDef(
                llvm::orc::ExecutorAddr::fromPtr(&rvdl_qattention),
                llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);
            HostApiMap[Mangle("rvdl_shannon4")] = llvm::orc::ExecutorSymbolDef(
                llvm::orc::ExecutorAddr::fromPtr(&rvdl_shannon4),
                llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);
            HostApiMap[Mangle("rvdl_shannon8")] = llvm::orc::ExecutorSymbolDef(
                llvm::orc::ExecutorAddr::fromPtr(&rvdl_shannon8),
                llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);
            HostApiMap[Mangle("rvdl_qgate_h")] = llvm::orc::ExecutorSymbolDef(
                llvm::orc::ExecutorAddr::fromPtr(&rvdl_qgate_h),
                llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);
            HostApiMap[Mangle("rvdl_qgate_x")] = llvm::orc::ExecutorSymbolDef(
                llvm::orc::ExecutorAddr::fromPtr(&rvdl_qgate_x),
                llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);
            HostApiMap[Mangle("rvdl_qgate_rz")] = llvm::orc::ExecutorSymbolDef(
                llvm::orc::ExecutorAddr::fromPtr(&rvdl_qgate_rz),
                llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);
            HostApiMap[Mangle("rvdl_qgate_cnot")] = llvm::orc::ExecutorSymbolDef(
                llvm::orc::ExecutorAddr::fromPtr(&rvdl_qgate_cnot),
                llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);
            HostApiMap[Mangle("rvdl_qexpect_z")] = llvm::orc::ExecutorSymbolDef(
                llvm::orc::ExecutorAddr::fromPtr(&rvdl_qexpect_z),
                llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);
            HostApiMap[Mangle("rvdl_qmeasure")] = llvm::orc::ExecutorSymbolDef(
                llvm::orc::ExecutorAddr::fromPtr(&rvdl_qmeasure),
                llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);
            HostApiMap[Mangle("rvdl_qobj_num_qubits")] = llvm::orc::ExecutorSymbolDef(
                llvm::orc::ExecutorAddr::fromPtr(&rvdl_qobj_num_qubits),
                llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);
            HostApiMap[Mangle("rvdl_qgate_cnot_pair")] = llvm::orc::ExecutorSymbolDef(
                llvm::orc::ExecutorAddr::fromPtr(&rvdl_qgate_cnot_pair),
                llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);
            // --- 量子非欧几里德曲面体几何原语 ABI ---
            HostApiMap[Mangle("rvdl_geodesic_distance")] = llvm::orc::ExecutorSymbolDef(
                llvm::orc::ExecutorAddr::fromPtr(&rvdl_geodesic_distance),
                llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);
            HostApiMap[Mangle("rvdl_inversion")] = llvm::orc::ExecutorSymbolDef(
                llvm::orc::ExecutorAddr::fromPtr(&rvdl_inversion),
                llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);
            HostApiMap[Mangle("rvdl_hyperbolic_metric")] = llvm::orc::ExecutorSymbolDef(
                llvm::orc::ExecutorAddr::fromPtr(&rvdl_hyperbolic_metric),
                llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);
            HostApiMap[Mangle("rvdl_hyperbolic_distance")] = llvm::orc::ExecutorSymbolDef(
                llvm::orc::ExecutorAddr::fromPtr(&rvdl_hyperbolic_distance),
                llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);

            // --- 量子通道逆因果容量（回程能力）ABI ---
            HostApiMap[Mangle("rvdl_retrocausal_imax")] = llvm::orc::ExecutorSymbolDef(
                llvm::orc::ExecutorAddr::fromPtr(&rvdl_retrocausal_imax),
                llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);
            HostApiMap[Mangle("rvdl_retrocausal_idoe")] = llvm::orc::ExecutorSymbolDef(
                llvm::orc::ExecutorAddr::fromPtr(&rvdl_retrocausal_idoe),
                llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);
            HostApiMap[Mangle("rvdl_retrocausal_q_capacity")] = llvm::orc::ExecutorSymbolDef(
                llvm::orc::ExecutorAddr::fromPtr(&rvdl_retrocausal_q_capacity),
                llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);
            HostApiMap[Mangle("rvdl_retrocausal_c_capacity")] = llvm::orc::ExecutorSymbolDef(
                llvm::orc::ExecutorAddr::fromPtr(&rvdl_retrocausal_c_capacity),
                llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);
            HostApiMap[Mangle("rvdl_retrocausal_q_one_shot")] = llvm::orc::ExecutorSymbolDef(
                llvm::orc::ExecutorAddr::fromPtr(&rvdl_retrocausal_q_one_shot),
                llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);
            HostApiMap[Mangle("rvdl_retrocausal_gain")] = llvm::orc::ExecutorSymbolDef(
                llvm::orc::ExecutorAddr::fromPtr(&rvdl_retrocausal_gain),
                llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);
            HostApiMap[Mangle("rvdl_retrocausal_deformed")] = llvm::orc::ExecutorSymbolDef(
                llvm::orc::ExecutorAddr::fromPtr(&rvdl_retrocausal_deformed),
                llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);
            HostApiMap[Mangle("rvdl_retrocausal_ctc_q_capacity")] = llvm::orc::ExecutorSymbolDef(
                llvm::orc::ExecutorAddr::fromPtr(&rvdl_retrocausal_ctc_q_capacity),
                llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);
            HostApiMap[Mangle("rvdl_retrocausal_ctc_c_capacity")] = llvm::orc::ExecutorSymbolDef(
                llvm::orc::ExecutorAddr::fromPtr(&rvdl_retrocausal_ctc_c_capacity),
                llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);
            HostApiMap[Mangle("rvdl_retrocausal_ctc_gain")] = llvm::orc::ExecutorSymbolDef(
                llvm::orc::ExecutorAddr::fromPtr(&rvdl_retrocausal_ctc_gain),
                llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);
            HostApiMap[Mangle("rvdl_retrocausal_ctc_dephasing")] = llvm::orc::ExecutorSymbolDef(
                llvm::orc::ExecutorAddr::fromPtr(&rvdl_retrocausal_ctc_dephasing),
                llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);

            // --- QCOS syscall ABI ---
            HostApiMap[Mangle("rvdl_sys_call")] = llvm::orc::ExecutorSymbolDef(
                llvm::orc::ExecutorAddr::fromPtr(&rvdl_sys_call),
                llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);
            HostApiMap[Mangle("rvdl_sys_calld")] = llvm::orc::ExecutorSymbolDef(
                llvm::orc::ExecutorAddr::fromPtr(&rvdl_sys_calld),
                llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);
            HostApiMap[Mangle("rvdl_sys_log")] = llvm::orc::ExecutorSymbolDef(
                llvm::orc::ExecutorAddr::fromPtr(&rvdl_sys_log),
                llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);
            HostApiMap[Mangle("rvdl_sys_logi")] = llvm::orc::ExecutorSymbolDef(
                llvm::orc::ExecutorAddr::fromPtr(&rvdl_sys_logi),
                llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);
            HostApiMap[Mangle("rvdl_sys_callp")] = llvm::orc::ExecutorSymbolDef(
                llvm::orc::ExecutorAddr::fromPtr(&rvdl_sys_callp),
                llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);
            HostApiMap[Mangle("rvdl_gc_alloc")] = llvm::orc::ExecutorSymbolDef(
                llvm::orc::ExecutorAddr::fromPtr(&rvdl_gc_alloc),
                llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);
            HostApiMap[Mangle("rvdl_gc_free")] = llvm::orc::ExecutorSymbolDef(
                llvm::orc::ExecutorAddr::fromPtr(&rvdl_gc_free),
                llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);
            HostApiMap[Mangle("rvdl_qms_gap")] = llvm::orc::ExecutorSymbolDef(
                llvm::orc::ExecutorAddr::fromPtr(&rvdl_qms_gap),
                llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);
            HostApiMap[Mangle("rvdl_mix_bound")] = llvm::orc::ExecutorSymbolDef(
                llvm::orc::ExecutorAddr::fromPtr(&rvdl_mix_bound),
                llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);
            HostApiMap[Mangle("rvdl_qms_conc")] = llvm::orc::ExecutorSymbolDef(
                llvm::orc::ExecutorAddr::fromPtr(&rvdl_qms_conc),
                llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);

            // --- QChain ABI ---
            HostApiMap[Mangle("rvdl_qchain_wallet")] = llvm::orc::ExecutorSymbolDef(
                llvm::orc::ExecutorAddr::fromPtr(&rvdl_qchain_wallet),
                llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);
            HostApiMap[Mangle("rvdl_qchain_balance")] = llvm::orc::ExecutorSymbolDef(
                llvm::orc::ExecutorAddr::fromPtr(&rvdl_qchain_balance),
                llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);
            HostApiMap[Mangle("rvdl_qchain_mint")] = llvm::orc::ExecutorSymbolDef(
                llvm::orc::ExecutorAddr::fromPtr(&rvdl_qchain_mint),
                llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);
            HostApiMap[Mangle("rvdl_qchain_transfer")] = llvm::orc::ExecutorSymbolDef(
                llvm::orc::ExecutorAddr::fromPtr(&rvdl_qchain_transfer),
                llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);
            HostApiMap[Mangle("rvdl_qchain_mine")] = llvm::orc::ExecutorSymbolDef(
                llvm::orc::ExecutorAddr::fromPtr(&rvdl_qchain_mine),
                llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);
            HostApiMap[Mangle("rvdl_qchain_height")] = llvm::orc::ExecutorSymbolDef(
                llvm::orc::ExecutorAddr::fromPtr(&rvdl_qchain_height),
                llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);
            HostApiMap[Mangle("rvdl_qchain_verify")] = llvm::orc::ExecutorSymbolDef(
                llvm::orc::ExecutorAddr::fromPtr(&rvdl_qchain_verify),
                llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);
            HostApiMap[Mangle("rvdl_qchain_qkd")] = llvm::orc::ExecutorSymbolDef(
                llvm::orc::ExecutorAddr::fromPtr(&rvdl_qchain_qkd),
                llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);
            HostApiMap[Mangle("rvdl_qchain_qdba")] = llvm::orc::ExecutorSymbolDef(
                llvm::orc::ExecutorAddr::fromPtr(&rvdl_qchain_qdba),
                llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);
            HostApiMap[Mangle("rvdl_qchain_coin_mint")] = llvm::orc::ExecutorSymbolDef(
                llvm::orc::ExecutorAddr::fromPtr(&rvdl_qchain_coin_mint),
                llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);
            HostApiMap[Mangle("rvdl_qchain_coin_verify")] = llvm::orc::ExecutorSymbolDef(
                llvm::orc::ExecutorAddr::fromPtr(&rvdl_qchain_coin_verify),
                llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);
                
            HostApiMap[Mangle("rvdl_qchain_sha3")] = llvm::orc::ExecutorSymbolDef(
                llvm::orc::ExecutorAddr::fromPtr(&rvdl_qchain_sha3),
                llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);
            HostApiMap[Mangle("rvdl_qchain_hmac")] = llvm::orc::ExecutorSymbolDef(
                llvm::orc::ExecutorAddr::fromPtr(&rvdl_qchain_hmac),
                llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);
            HostApiMap[Mangle("rvdl_qchain_hash_unicode")] = llvm::orc::ExecutorSymbolDef(
                llvm::orc::ExecutorAddr::fromPtr(&rvdl_qchain_hash_unicode),
                llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);
            HostApiMap[Mangle("rvdl_qchain_sign")] = llvm::orc::ExecutorSymbolDef(
                llvm::orc::ExecutorAddr::fromPtr(&rvdl_qchain_sign),
                llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);
            HostApiMap[Mangle("rvdl_qchain_sign_verify")] = llvm::orc::ExecutorSymbolDef(
                llvm::orc::ExecutorAddr::fromPtr(&rvdl_qchain_sign_verify),
                llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);
            HostApiMap[Mangle("rvdl_qchain_sign_pubkey")] = llvm::orc::ExecutorSymbolDef(
                llvm::orc::ExecutorAddr::fromPtr(&rvdl_qchain_sign_pubkey),
                llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);
            HostApiMap[Mangle("rvdl_qchain_mlkem_encaps")] = llvm::orc::ExecutorSymbolDef(
                llvm::orc::ExecutorAddr::fromPtr(&rvdl_qchain_mlkem_encaps),
                llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);
            HostApiMap[Mangle("rvdl_qchain_mlkem_decaps")] = llvm::orc::ExecutorSymbolDef(
                llvm::orc::ExecutorAddr::fromPtr(&rvdl_qchain_mlkem_decaps),
                llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);
            HostApiMap[Mangle("rvdl_qchain_causal_verify")] = llvm::orc::ExecutorSymbolDef(
                llvm::orc::ExecutorAddr::fromPtr(&rvdl_qchain_causal_verify),
                llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);
            HostApiMap[Mangle("rvdl_qchain_cipher_encrypt")] = llvm::orc::ExecutorSymbolDef(
                llvm::orc::ExecutorAddr::fromPtr(&rvdl_qchain_cipher_encrypt),
                llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);
            HostApiMap[Mangle("rvdl_qchain_cipher_decrypt")] = llvm::orc::ExecutorSymbolDef(
                llvm::orc::ExecutorAddr::fromPtr(&rvdl_qchain_cipher_decrypt),
                llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);

            // --- Lattice 晶格数组 ABI ---
            HostApiMap[Mangle("rvdl_lattice_new")] = llvm::orc::ExecutorSymbolDef(
                llvm::orc::ExecutorAddr::fromPtr(&rvdl_lattice_new),
                llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);
            HostApiMap[Mangle("rvdl_lattice_free")] = llvm::orc::ExecutorSymbolDef(
                llvm::orc::ExecutorAddr::fromPtr(&rvdl_lattice_free),
                llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);
            HostApiMap[Mangle("rvdl_lattice_ref")] = llvm::orc::ExecutorSymbolDef(
                llvm::orc::ExecutorAddr::fromPtr(&rvdl_lattice_ref),
                llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);
            HostApiMap[Mangle("rvdl_lattice_set")] = llvm::orc::ExecutorSymbolDef(
                llvm::orc::ExecutorAddr::fromPtr(&rvdl_lattice_set),
                llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);
            HostApiMap[Mangle("rvdl_lattice_rank")] = llvm::orc::ExecutorSymbolDef(
                llvm::orc::ExecutorAddr::fromPtr(&rvdl_lattice_rank),
                llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);
            HostApiMap[Mangle("rvdl_lattice_size")] = llvm::orc::ExecutorSymbolDef(
                llvm::orc::ExecutorAddr::fromPtr(&rvdl_lattice_size),
                llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);
            HostApiMap[Mangle("rvdl_lattice_boundary")] = llvm::orc::ExecutorSymbolDef(
                llvm::orc::ExecutorAddr::fromPtr(&rvdl_lattice_boundary),
                llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);

            // --- 经典 GUI / 图形引擎 ABI ---
            HostApiMap[Mangle("rvdl_cgui_init")] = llvm::orc::ExecutorSymbolDef(
                llvm::orc::ExecutorAddr::fromPtr(&rvdl_cgui_init),
                llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);
            HostApiMap[Mangle("rvdl_cgui_should_close")] = llvm::orc::ExecutorSymbolDef(
                llvm::orc::ExecutorAddr::fromPtr(&rvdl_cgui_should_close),
                llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);
            HostApiMap[Mangle("rvdl_cgui_begin_frame")] = llvm::orc::ExecutorSymbolDef(
                llvm::orc::ExecutorAddr::fromPtr(&rvdl_cgui_begin_frame),
                llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);
            HostApiMap[Mangle("rvdl_cgui_end_frame")] = llvm::orc::ExecutorSymbolDef(
                llvm::orc::ExecutorAddr::fromPtr(&rvdl_cgui_end_frame),
                llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);
            HostApiMap[Mangle("rvdl_cgui_button")] = llvm::orc::ExecutorSymbolDef(
                llvm::orc::ExecutorAddr::fromPtr(&rvdl_cgui_button),
                llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);
            HostApiMap[Mangle("rvdl_cgui_text")] = llvm::orc::ExecutorSymbolDef(
                llvm::orc::ExecutorAddr::fromPtr(&rvdl_cgui_text),
                llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);
            HostApiMap[Mangle("rvdl_cgui_text_int")] = llvm::orc::ExecutorSymbolDef(
                llvm::orc::ExecutorAddr::fromPtr(&rvdl_cgui_text_int),
                llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);
            HostApiMap[Mangle("rvdl_cgui_beep")] = llvm::orc::ExecutorSymbolDef(
                llvm::orc::ExecutorAddr::fromPtr(&rvdl_cgui_beep),
                llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);
            HostApiMap[Mangle("rvdl_cgui_width")] = llvm::orc::ExecutorSymbolDef(
                llvm::orc::ExecutorAddr::fromPtr(&rvdl_cgui_width),
                llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);
            HostApiMap[Mangle("rvdl_cgui_height")] = llvm::orc::ExecutorSymbolDef(
                llvm::orc::ExecutorAddr::fromPtr(&rvdl_cgui_height),
                llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);
            HostApiMap[Mangle("rvdl_cgui_panel")] = llvm::orc::ExecutorSymbolDef(
                llvm::orc::ExecutorAddr::fromPtr(&rvdl_cgui_panel),
                llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);
            HostApiMap[Mangle("rvdl_cgui_panel_end")] = llvm::orc::ExecutorSymbolDef(
                llvm::orc::ExecutorAddr::fromPtr(&rvdl_cgui_panel_end),
                llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);
            HostApiMap[Mangle("rvdl_cgui_row")] = llvm::orc::ExecutorSymbolDef(
                llvm::orc::ExecutorAddr::fromPtr(&rvdl_cgui_row),
                llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);
            HostApiMap[Mangle("rvdl_cgui_mouse_x")] = llvm::orc::ExecutorSymbolDef(
                llvm::orc::ExecutorAddr::fromPtr(&rvdl_cgui_mouse_x),
                llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);
            HostApiMap[Mangle("rvdl_cgui_mouse_y")] = llvm::orc::ExecutorSymbolDef(
                llvm::orc::ExecutorAddr::fromPtr(&rvdl_cgui_mouse_y),
                llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);
            HostApiMap[Mangle("rvdl_cgui_mouse_left_clicked")] = llvm::orc::ExecutorSymbolDef(
                llvm::orc::ExecutorAddr::fromPtr(&rvdl_cgui_mouse_left_clicked),
                llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);
            HostApiMap[Mangle("rvdl_cgfx_rect")] = llvm::orc::ExecutorSymbolDef(
                llvm::orc::ExecutorAddr::fromPtr(&rvdl_cgfx_rect),
                llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);
            HostApiMap[Mangle("rvdl_cgfx_line")] = llvm::orc::ExecutorSymbolDef(
                llvm::orc::ExecutorAddr::fromPtr(&rvdl_cgfx_line),
                llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);
            HostApiMap[Mangle("rvdl_cgfx_polyline")] = llvm::orc::ExecutorSymbolDef(
                llvm::orc::ExecutorAddr::fromPtr(&rvdl_cgfx_polyline),
                llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);
            HostApiMap[Mangle("rvdl_cgfx_ellipse")] = llvm::orc::ExecutorSymbolDef(
                llvm::orc::ExecutorAddr::fromPtr(&rvdl_cgfx_ellipse),
                llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);
            HostApiMap[Mangle("rvdl_cgfx_triangle")] = llvm::orc::ExecutorSymbolDef(
                llvm::orc::ExecutorAddr::fromPtr(&rvdl_cgfx_triangle),
                llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);
            HostApiMap[Mangle("rvdl_cgfx_rect_a")] = llvm::orc::ExecutorSymbolDef(
                llvm::orc::ExecutorAddr::fromPtr(&rvdl_cgfx_rect_a),
                llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);
            HostApiMap[Mangle("rvdl_cgfx_line_a")] = llvm::orc::ExecutorSymbolDef(
                llvm::orc::ExecutorAddr::fromPtr(&rvdl_cgfx_line_a),
                llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);
            HostApiMap[Mangle("rvdl_cgfx_polyline_a")] = llvm::orc::ExecutorSymbolDef(
                llvm::orc::ExecutorAddr::fromPtr(&rvdl_cgfx_polyline_a),
                llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);
            HostApiMap[Mangle("rvdl_cgfx_ellipse_a")] = llvm::orc::ExecutorSymbolDef(
                llvm::orc::ExecutorAddr::fromPtr(&rvdl_cgfx_ellipse_a),
                llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);
            HostApiMap[Mangle("rvdl_cgfx_triangle_a")] = llvm::orc::ExecutorSymbolDef(
                llvm::orc::ExecutorAddr::fromPtr(&rvdl_cgfx_triangle_a),
                llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);

            // --- 量子图形引擎 qgfx_*（PEQG）---
            HostApiMap[Mangle("rvdl_qgfx_frame_new")] = llvm::orc::ExecutorSymbolDef(
                llvm::orc::ExecutorAddr::fromPtr(&rvdl_qgfx_frame_new),
                llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);
            HostApiMap[Mangle("rvdl_qgfx_frame_capture")] = llvm::orc::ExecutorSymbolDef(
                llvm::orc::ExecutorAddr::fromPtr(&rvdl_qgfx_frame_capture),
                llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);
            HostApiMap[Mangle("rvdl_qgfx_frame_set_amplitudes")] = llvm::orc::ExecutorSymbolDef(
                llvm::orc::ExecutorAddr::fromPtr(&rvdl_qgfx_frame_set_amplitudes),
                llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);
            HostApiMap[Mangle("rvdl_qgfx_frame_release")] = llvm::orc::ExecutorSymbolDef(
                llvm::orc::ExecutorAddr::fromPtr(&rvdl_qgfx_frame_release),
                llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);
            HostApiMap[Mangle("rvdl_qgfx_frame_residual")] = llvm::orc::ExecutorSymbolDef(
                llvm::orc::ExecutorAddr::fromPtr(&rvdl_qgfx_frame_residual),
                llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);
            HostApiMap[Mangle("rvdl_qgfx_frame_num_qubits")] = llvm::orc::ExecutorSymbolDef(
                llvm::orc::ExecutorAddr::fromPtr(&rvdl_qgfx_frame_num_qubits),
                llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);
            HostApiMap[Mangle("rvdl_qgfx_frame_basis_size")] = llvm::orc::ExecutorSymbolDef(
                llvm::orc::ExecutorAddr::fromPtr(&rvdl_qgfx_frame_basis_size),
                llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);
            HostApiMap[Mangle("rvdl_qgfx_projection_name")] = llvm::orc::ExecutorSymbolDef(
                llvm::orc::ExecutorAddr::fromPtr(&rvdl_qgfx_projection_name),
                llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);
            HostApiMap[Mangle("rvdl_qgfx_projection_source_dims")] = llvm::orc::ExecutorSymbolDef(
                llvm::orc::ExecutorAddr::fromPtr(&rvdl_qgfx_projection_source_dims),
                llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);
            HostApiMap[Mangle("rvdl_qgfx_projection_target_dims")] = llvm::orc::ExecutorSymbolDef(
                llvm::orc::ExecutorAddr::fromPtr(&rvdl_qgfx_projection_target_dims),
                llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);
            HostApiMap[Mangle("rvdl_qgfx_concurrence")] = llvm::orc::ExecutorSymbolDef(
                llvm::orc::ExecutorAddr::fromPtr(&rvdl_qgfx_concurrence),
                llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);
            HostApiMap[Mangle("rvdl_qgfx_quaternionic")] = llvm::orc::ExecutorSymbolDef(
                llvm::orc::ExecutorAddr::fromPtr(&rvdl_qgfx_quaternionic),
                llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);
            HostApiMap[Mangle("rvdl_qgfx_entropy")] = llvm::orc::ExecutorSymbolDef(
                llvm::orc::ExecutorAddr::fromPtr(&rvdl_qgfx_entropy),
                llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);
            HostApiMap[Mangle("rvdl_qgfx_barcode")] = llvm::orc::ExecutorSymbolDef(
                llvm::orc::ExecutorAddr::fromPtr(&rvdl_qgfx_barcode),
                llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);
            HostApiMap[Mangle("rvdl_qgfx_same_class")] = llvm::orc::ExecutorSymbolDef(
                llvm::orc::ExecutorAddr::fromPtr(&rvdl_qgfx_same_class),
                llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);
            HostApiMap[Mangle("rvdl_qgfx_amplitude")] = llvm::orc::ExecutorSymbolDef(
                llvm::orc::ExecutorAddr::fromPtr(&rvdl_qgfx_amplitude),
                llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);
            HostApiMap[Mangle("rvdl_qgfx_amplitude_phase")] = llvm::orc::ExecutorSymbolDef(
                llvm::orc::ExecutorAddr::fromPtr(&rvdl_qgfx_amplitude_phase),
                llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);
            HostApiMap[Mangle("rvdl_qgfx_interference")] = llvm::orc::ExecutorSymbolDef(
                llvm::orc::ExecutorAddr::fromPtr(&rvdl_qgfx_interference),
                llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);
            HostApiMap[Mangle("rvdl_qgfx_phase_hue")] = llvm::orc::ExecutorSymbolDef(
                llvm::orc::ExecutorAddr::fromPtr(&rvdl_qgfx_phase_hue),
                llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);
            HostApiMap[Mangle("rvdl_qgfx_nested_shells")] = llvm::orc::ExecutorSymbolDef(
                llvm::orc::ExecutorAddr::fromPtr(&rvdl_qgfx_nested_shells),
                llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);
            HostApiMap[Mangle("rvdl_qgfx_nested_mass")] = llvm::orc::ExecutorSymbolDef(
                llvm::orc::ExecutorAddr::fromPtr(&rvdl_qgfx_nested_mass),
                llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);
            HostApiMap[Mangle("rvdl_qgfx_bloch")] = llvm::orc::ExecutorSymbolDef(
                llvm::orc::ExecutorAddr::fromPtr(&rvdl_qgfx_bloch),
                llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);
            HostApiMap[Mangle("rvdl_qgfx_probability")] = llvm::orc::ExecutorSymbolDef(
                llvm::orc::ExecutorAddr::fromPtr(&rvdl_qgfx_probability),
                llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);

            // --- 量子 UI 库 qqui_*（PCQW）---
            HostApiMap[Mangle("rvdl_qqui_qubit_glyph")] = llvm::orc::ExecutorSymbolDef(
                llvm::orc::ExecutorAddr::fromPtr(&rvdl_qqui_qubit_glyph),
                llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);
            HostApiMap[Mangle("rvdl_qqui_qubit_row")] = llvm::orc::ExecutorSymbolDef(
                llvm::orc::ExecutorAddr::fromPtr(&rvdl_qqui_qubit_row),
                llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);
            HostApiMap[Mangle("rvdl_qqui_entangle_link")] = llvm::orc::ExecutorSymbolDef(
                llvm::orc::ExecutorAddr::fromPtr(&rvdl_qqui_entangle_link),
                llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);
            HostApiMap[Mangle("rvdl_qqui_uncertainty_bin")] = llvm::orc::ExecutorSymbolDef(
                llvm::orc::ExecutorAddr::fromPtr(&rvdl_qqui_uncertainty_bin),
                llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);
            HostApiMap[Mangle("rvdl_qqui_wigner_slice")] = llvm::orc::ExecutorSymbolDef(
                llvm::orc::ExecutorAddr::fromPtr(&rvdl_qqui_wigner_slice),
                llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);
            HostApiMap[Mangle("rvdl_qqui_nested_bar")] = llvm::orc::ExecutorSymbolDef(
                llvm::orc::ExecutorAddr::fromPtr(&rvdl_qqui_nested_bar),
                llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);
            HostApiMap[Mangle("rvdl_qqui_amplitude_strip")] = llvm::orc::ExecutorSymbolDef(
                llvm::orc::ExecutorAddr::fromPtr(&rvdl_qqui_amplitude_strip),
                llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);
            HostApiMap[Mangle("rvdl_qqui_state_card")] = llvm::orc::ExecutorSymbolDef(
                llvm::orc::ExecutorAddr::fromPtr(&rvdl_qqui_state_card),
                llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);

            // --- 多维标签函数执行拓扑（@layer）调度入口 ---
            HostApiMap[Mangle("rvdl_runtime_run_topology")] = llvm::orc::ExecutorSymbolDef(
                llvm::orc::ExecutorAddr::fromPtr(&rvdl_runtime_run_topology),
                llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);


            auto Err = MainDylib.define(llvm::orc::absoluteSymbols(HostApiMap));
            if (Err)
            {
                handle_llvm_error(std::move(Err));
            }
        }
    };
}