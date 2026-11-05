// ============================================================================
// runtime_android.cpp — Android 轻量运行时（无 LLVM JIT）
//
// 与桌面 rvdl_rt（依赖 LLVM JIT / Compiler）不同，Android 端采用 AOT 模型：
//   - rvdl 代码在桌面端 `rvdl compile` 预编译成 aarch64-linux-android 的 .so；
//   - 本文件提供 QVM 核心 + 全部 rvdl_* 运行时符号（Qubit 层 / QObject 层 / 对象创建）；
//   - AOT 编译出的 rvdl .so 链接本运行时，通过 JNI 调用 rvdl_main。
//
// 仅依赖：QVM.hpp（纯 C++ 量子态矢量）+ CircuitTelemetry + Kokkos（可选 GPU）。
// ============================================================================
#include <jni.h>
#include <atomic>
#include <vector>
#include <complex>
#include <thread>
#include <mutex>
#include <algorithm>

// QVM 经 utils/Ga.hpp 需要 Kokkos 头：提供 Kokkos 的构建（RVDL_HAS_KOKKOS）
// 用它；否则退回零依赖的 BasicQVM（纯 CPU 态矢量，适合手机端）。
#if defined(RVDL_HAS_KOKKOS)
#include "qhal/QVM.hpp"
#else
#include "BasicQVM.hpp"
#endif
#include "qhal/CircuitTelemetry.hpp"
// ─── QObject 结构（复制自 qml/Inference.hpp，避免引入 LLVM/QLM 重依赖）───
enum QObjectDataKind : int
{
    QOBJ_DATA_NONE = 0,
    QOBJ_DATA_SHARED = 1,   // std::shared_ptr<rvdl::QObject>*
    QOBJ_DATA_STRING = 2    // std::string*
};

struct QObject
{
    std::vector<size_t> hardware_ids;
    void *qlm_data = nullptr;
    int data_kind = QOBJ_DATA_NONE;
};

// ─── 全局后端指针（Qubit 层用 ActiveBackend，QObject 层用 global_qm）───
static std::atomic<size_t> next_available_qubit{0};
static qhal::IQuantumBackend *ActiveBackend = nullptr;
qhal::IQuantumBackend *global_qm = nullptr;

// ─── 全局 qubit id 分配器：单调高水位 + 空闲 id 复用栈 ───
static std::vector<size_t> &qubit_free_list() { static std::vector<size_t> s; return s; }
static std::mutex &qubit_allocator_mutex() { static std::mutex m; return m; }
static size_t rvdl_alloc_qubit_id()
{
    std::lock_guard<std::mutex> lock(qubit_allocator_mutex());
    auto &free = qubit_free_list();
    if (!free.empty()) { size_t id = free.back(); free.pop_back(); return id; }
    return next_available_qubit.fetch_add(1);
}
static void rvdl_free_qubit_id(size_t id)
{
    std::lock_guard<std::mutex> lock(qubit_allocator_mutex());
    auto &free = qubit_free_list();
    if (std::find(free.begin(), free.end(), id) == free.end()) free.push_back(id);
}

// ============================================================================
// 对象创建（rvdl_create_*）
// ============================================================================
extern "C" void *rvdl_create_DiracState(int n)
{
    QObject *obj = new QObject();
    if (n < 1) n = 1;
    for (int i = 0; i < n; ++i)
    {
        size_t id = rvdl_alloc_qubit_id();
        obj->hardware_ids.push_back(id);
        if (ActiveBackend) ActiveBackend->allocate_qubit(id);
    }
    qhal::CircuitTelemetry::get_instance().log_object(
        "DiracState", std::vector<int>(obj->hardware_ids.begin(), obj->hardware_ids.end()));
    return obj;
}

extern "C" void *rvdl_create_BellState()
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

extern "C" void *rvdl_create_QuantumRegister(int size)
{
    QObject *obj = new QObject();
    for (int i = 0; i < size; ++i)
    {
        size_t id = rvdl_alloc_qubit_id();
        obj->hardware_ids.push_back(id);
        if (ActiveBackend) ActiveBackend->allocate_qubit(id);
    }
    qhal::CircuitTelemetry::get_instance().log_object(
        "QuantumRegister", std::vector<int>(obj->hardware_ids.begin(), obj->hardware_ids.end()));
    return obj;
}

extern "C" void *rvdl_create_basis_state(double theta, double phi, int value)
{
    QObject *obj = new QObject();
    size_t q = rvdl_alloc_qubit_id();
    obj->hardware_ids = {q};
    if (ActiveBackend)
    {
        ActiveBackend->allocate_qubit(q);
        ActiveBackend->apply_basis(q, theta, phi);
        if (value != 0)
            ActiveBackend->apply_x(q);
    }
    qhal::CircuitTelemetry::get_instance().log_object(
        "BasisState", std::vector<int>{static_cast<int>(q)}, theta, phi);
    return obj;
}

// ============================================================================
// Qubit 层（__quantum__qis__* / __quantum__rt__*）
// ============================================================================
static size_t qubit_handle_id(void *qubit)
{
    return qubit ? *static_cast<size_t *>(qubit) : 0;
}

extern "C" void *__quantum__rt__qubit_allocate()
{
    if (!ActiveBackend) return nullptr;
    size_t id = rvdl_alloc_qubit_id();
    ActiveBackend->allocate_qubit(id);
    return new size_t(id);
}

extern "C" void __quantum__rt__qubit_release(void *qubit)
{
    if (!qubit) return;
    size_t *id = static_cast<size_t *>(qubit);
    if (ActiveBackend) ActiveBackend->release_qubit(*id);
    rvdl_free_qubit_id(*id);
    delete id;
}

extern "C" int __quantum__qis__measure_int(void *qubit)
{
    if (!qubit || !ActiveBackend) return 0;
    return ActiveBackend->measure(*static_cast<size_t *>(qubit));
}

extern "C" void __quantum__qis__h(void *q)   { if (ActiveBackend) ActiveBackend->apply_h(qubit_handle_id(q)); }
extern "C" void __quantum__qis__x(void *q)   { if (ActiveBackend) ActiveBackend->apply_x(qubit_handle_id(q)); }
extern "C" void __quantum__qis__y(void *q)   { if (ActiveBackend) ActiveBackend->apply_y(qubit_handle_id(q)); }
extern "C" void __quantum__qis__z(void *q)   { if (ActiveBackend) ActiveBackend->apply_z(qubit_handle_id(q)); }
extern "C" void __quantum__qis__s(void *q)   { if (ActiveBackend) ActiveBackend->apply_s(qubit_handle_id(q)); }
extern "C" void __quantum__qis__t(void *q)   { if (ActiveBackend) ActiveBackend->apply_t(qubit_handle_id(q)); }
extern "C" void __quantum__qis__rz(double a, void *q) { if (ActiveBackend) ActiveBackend->apply_rz(qubit_handle_id(q), a); }
extern "C" void __quantum__qis__rx(double a, void *q) { if (ActiveBackend) ActiveBackend->apply_rx(qubit_handle_id(q), a); }
extern "C" void __quantum__qis__ry(double a, void *q) { if (ActiveBackend) ActiveBackend->apply_ry(qubit_handle_id(q), a); }
extern "C" void __quantum__qis__cnot(void *c, void *t) { if (ActiveBackend) ActiveBackend->apply_cnot(qubit_handle_id(c), qubit_handle_id(t)); }
extern "C" void __quantum__qis__toffoli(void *c1, void *c2, void *t) { if (ActiveBackend) ActiveBackend->apply_toffoli(qubit_handle_id(c1), qubit_handle_id(c2), qubit_handle_id(t)); }
extern "C" void __quantum__qis__swap(void *a, void *b) { if (ActiveBackend) ActiveBackend->apply_swap(qubit_handle_id(a), qubit_handle_id(b)); }
extern "C" void __quantum__qis__cx(void *c, void *t) { if (ActiveBackend) ActiveBackend->apply_cx(qubit_handle_id(c), qubit_handle_id(t)); }
extern "C" void __quantum__qis__ch(void *c, void *t) { if (ActiveBackend) ActiveBackend->apply_ch(qubit_handle_id(c), qubit_handle_id(t)); }
extern "C" void __quantum__qis__crz(void *c, void *t, double a) { if (ActiveBackend) ActiveBackend->apply_crz(qubit_handle_id(c), qubit_handle_id(t), a); }
extern "C" void __quantum__qis__cswap(void *c, void *a, void *b) { if (ActiveBackend) ActiveBackend->apply_cswap(qubit_handle_id(c), qubit_handle_id(a), qubit_handle_id(b)); }
extern "C" void __quantum__qis__c_toffoli(void *c, void *a, void *b, void *t) { if (ActiveBackend) ActiveBackend->apply_c_toffoli(qubit_handle_id(c), qubit_handle_id(a), qubit_handle_id(b), qubit_handle_id(t)); }
extern "C" void __quantum__qis__braid(void *a, void *b) { if (ActiveBackend) ActiveBackend->apply_braid(qubit_handle_id(a), qubit_handle_id(b)); }
extern "C" void __quantum__qis__cbraid(void *c, void *a, void *b) { if (ActiveBackend) ActiveBackend->apply_cbraid(qubit_handle_id(c), qubit_handle_id(a), qubit_handle_id(b)); }
extern "C" void __quantum__qis__apply_noise(void *q, int ch, double p) { if (ActiveBackend) ActiveBackend->apply_noise(qubit_handle_id(q), ch, p); }
extern "C" int  __quantum__qis__measure_basis(void *q, char b) { return ActiveBackend ? ActiveBackend->measure_basis(qubit_handle_id(q), b) : 0; }
extern "C" void __quantum__qis__qft(int n) { if (ActiveBackend && n > 0) ActiveBackend->apply_qft(0, static_cast<size_t>(n - 1)); }
extern "C" void __quantum__qis__iqft(int n) { if (ActiveBackend && n > 0) ActiveBackend->apply_iqft(0, static_cast<size_t>(n - 1)); }
extern "C" void __quantum__qis__cqft(void *c, int n) { if (ActiveBackend && n > 0) ActiveBackend->apply_cqft(qubit_handle_id(c), 0, static_cast<size_t>(n - 1)); }
extern "C" void rvdl_spawn(void (*fn)(void *), void *env) { if (fn) { std::thread t(fn, env); t.detach(); } }

// ============================================================================
// QObject 层（rvdl_qgate_* / rvdl_qmeasure / rvdl_qexpect_z）
// ============================================================================
static inline bool qgate_in_range(const QObject *obj, int i)
{
    return obj && i >= 0 && i < static_cast<int>(obj->hardware_ids.size());
}

extern "C" void rvdl_qgate_h(QObject *obj, int index) { if (qgate_in_range(obj, index) && global_qm) global_qm->apply_h(obj->hardware_ids[index]); }
extern "C" void rvdl_qgate_x(QObject *obj, int index) { if (qgate_in_range(obj, index) && global_qm) global_qm->apply_x(obj->hardware_ids[index]); }
extern "C" void rvdl_qgate_rz(QObject *obj, int index, double angle) { if (qgate_in_range(obj, index) && global_qm) global_qm->apply_rz(obj->hardware_ids[index], angle); }
extern "C" void rvdl_qgate_cnot(QObject *obj, int c, int t) { if (qgate_in_range(obj, c) && qgate_in_range(obj, t) && global_qm) global_qm->apply_cnot(obj->hardware_ids[c], obj->hardware_ids[t]); }
extern "C" void rvdl_qgate_cnot_pair(QObject *a, int ia, QObject *b, int ib) { if (qgate_in_range(a, ia) && qgate_in_range(b, ib) && global_qm) global_qm->apply_cnot(a->hardware_ids[ia], b->hardware_ids[ib]); }
extern "C" double rvdl_qexpect_z(QObject *obj, int index) { return (qgate_in_range(obj, index) && global_qm) ? global_qm->expectation_z(obj->hardware_ids[index]) : 0.0; }
extern "C" int rvdl_qmeasure(QObject *obj, int index) { return (qgate_in_range(obj, index) && global_qm) ? global_qm->measure(obj->hardware_ids[index]) : 0; }
extern "C" int rvdl_qobj_num_qubits(QObject *obj) { return obj ? static_cast<int>(obj->hardware_ids.size()) : 0; }

// ============================================================================
// 对象操作（rvdl_alloc / rvdl_measure / rvdl_measure_object / rvdl_release_object）
// ============================================================================
extern "C" void rvdl_alloc(size_t num_qubits)
{
    if (ActiveBackend) ActiveBackend->allocate_qubits(num_qubits);
}

extern "C" int rvdl_measure(size_t qubit_id)
{
    return ActiveBackend ? ActiveBackend->measure(qubit_id) : -1;
}

extern "C" int rvdl_measure_object(void *ptr)
{
    QObject *obj = static_cast<QObject *>(ptr);
    if (ActiveBackend && obj && !obj->hardware_ids.empty())
        return ActiveBackend->measure(obj->hardware_ids[0]);
    return 0;
}

extern "C" void rvdl_release_object(void *ptr)
{
    QObject *obj = static_cast<QObject *>(ptr);
    if (!obj) return;
    if (ActiveBackend)
        for (size_t q : obj->hardware_ids)
            ActiveBackend->release_qubit(q);
    for (size_t q : obj->hardware_ids)
        rvdl_free_qubit_id(q);
    if (obj->qlm_data)
    {
        if (obj->data_kind == QOBJ_DATA_STRING)
            delete static_cast<std::string *>(obj->qlm_data);
        obj->qlm_data = nullptr;
        obj->data_kind = QOBJ_DATA_NONE;
    }
    delete obj;
}

// ============================================================================
// JNI 接口：Java/Kotlin 调用（com.ravendrel.RavendrelRuntime）
// ============================================================================
extern "C" JNIEXPORT void JNICALL
Java_com_ravendrel_RavendrelRuntime_nativeInit(JNIEnv *, jobject)
{
    if (ActiveBackend) return;  // 已初始化
#if defined(RVDL_HAS_KOKKOS)
    global_qm = new qhal::QVM();
#else
    static qlm_cpu::BasicQVM basic; // 无 Kokkos：纯 CPU 态矢量后端
    global_qm = &basic;
#endif
    ActiveBackend = global_qm;
}

extern "C" JNIEXPORT void JNICALL
Java_com_ravendrel_RavendrelRuntime_nativeShutdown(JNIEnv *, jobject)
{
#if defined(RVDL_HAS_KOKKOS)
    if (ActiveBackend) { delete ActiveBackend; }
#endif
    ActiveBackend = nullptr;
    global_qm = nullptr;
}

// 自检：创建一个 Bell 态并测量，返回测量结果（验证运行时可用）。
extern "C" JNIEXPORT jint JNICALL
Java_com_ravendrel_RavendrelRuntime_nativeBellMeasure(JNIEnv *, jobject)
{
    if (!ActiveBackend) return -1;
    void *bell = rvdl_create_BellState();
    int result = rvdl_measure_object(bell);
    rvdl_release_object(bell);
    return result;
}

// AOT 编译出的 rvdl 代码入口（在 librvdl_main.so 中，运行时链接）。
// rvdl_main 由 rvdl compile 生成；若脚本带 @layer 块，则为 rvdl_topology_entry。
extern "C" int32_t rvdl_main();
extern "C" int32_t rvdl_topology_entry();

extern "C" JNIEXPORT jint JNICALL
Java_com_ravendrel_RavendrelRuntime_nativeRavendrelMain(JNIEnv *, jobject)
{
    if (!ActiveBackend) return -1;
    return static_cast<jint>(rvdl_main());
}