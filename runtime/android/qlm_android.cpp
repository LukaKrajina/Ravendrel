// ============================================================================
// qlm_android.cpp — Android 轻量 QLM 推理层（.rvdlm → 文本生成）
//
// 背景：桌面 daemon 的推理入口在 runtime/include/qml/Inference.hpp
//   （rvdl_qlm_load / rvdl_encode_string / rvdl_qlm_forward / rvdl_decode_string），
//   但那个头的 include 面是桌面级的 —— qhal/QM.hpp 会拖进四种物理后端驱动
//   （Karma_real / AtomicVaporORAM …）与 src/QObject.hpp。
//
// 本文件的取舍：
//   - 不 include Inference.hpp，而是**按最小依赖面**重建同一条推理路径：
//       IQuantumBackend（接口）+ QVM（纯 C++ 态矢量模拟）+
//       qml/RVDLModelFormat.hpp（.rvdlm 反序列化）+ qlm/LF2C2.hpp（上下文压缩）。
//     全部无 LLVM / 无 Kokkos / 无 Vulkan，可在 aarch64 Android 上纯 CPU 跑。
//   - **推理内核与 Inference.hpp 逐行一致**（门序列、tokenizer 表、
//     LF2C2 上下文编码）。训练（Transmitter）与推理两侧编码必须同分布，
//     任何一侧改动都要同步另一侧 —— 改动这里时请先 diff Inference.hpp。
//   - QObject 结构与 runtime_android.cpp 相同（重复声明是刻意的：
//     避免引入 src/QObject.hpp 的依赖链）。
//
// 构建：
//   - Android：加入 runtime/android/CMakeLists.txt 的 rvdl_rt 库（本文件），
//     定义 QLM_USE_QVM=1 使用与上游一致的 QVM 后端（需 Kokkos 头）。
//   - 桌面自检（无 Kokkos 环境）：不定义 __ANDROID__ 且不定义 QLM_USE_QVM，
//     用本文件自带的纯 C++ 态矢量后端 BasicQVM（~150 行，零第三方依赖）——
//     门语义与 QVM 一致（Rz(θ)=diag(1,e^{iθ})），足以验证推理内核本身：
//       clang++ -std=c++20 -D_USE_MATH_DEFINES -O2 -I runtime/include \
//               runtime/android/qlm_android.cpp -o qlm_selftest
//       ./qlm_selftest model_interop_out/toy.rvdlm
// ============================================================================

#ifdef __ANDROID__
#include <jni.h>
#endif

#include <string>
#include <vector>
#include <cstring>
#include <cstdint>
#include <cstdio>
#include <complex>
#include <random>
#include <iostream>
#include <unordered_map>

#include "qhal/IQuantumBackend.hpp"
// QVM.hpp 经 utils/Ga.hpp 无条件拉 Kokkos —— 只有提供 Kokkos 头的构建路径
// 才定义 QLM_USE_QVM；否则用零依赖的 BasicQVM（同目录共享头）。
#ifdef QLM_USE_QVM
#include "qhal/QVM.hpp"
#else
#include "BasicQVM.hpp"
#endif

#include "qml/RVDLModelFormat.hpp"
#include "qml/QubitAllocator.hpp"
#include "qlm/LF2C2.hpp"

// 后端实现见 BasicQVM.hpp（无 Kokkos 时）或 qhal/QVM.hpp（QLM_USE_QVM 时）。

// ─── 全局后端指针 ────────────────────────────────────────────────────────
// Android 下由 runtime_android.cpp 的 nativeInit 创建（与本文件同 .so）。
// 桌面自检模式下本文件自己定义并创建。
#ifdef __ANDROID__
extern qhal::IQuantumBackend *global_qm;
#else
qhal::IQuantumBackend *global_qm = nullptr;
#endif

// ─── QObject（与 runtime_android.cpp / Inference.hpp 同构）───────────────
enum QObjectDataKind : int
{
    QOBJ_DATA_NONE = 0,
    QOBJ_DATA_SHARED = 1, // std::shared_ptr<rvdl::QObject>*
    QOBJ_DATA_STRING = 2  // std::string*
};

struct QObject
{
    std::vector<size_t> hardware_ids;
    void *qlm_data = nullptr;
    int data_kind = QOBJ_DATA_NONE;
};

struct QModelJob
{
    void *prompt_data;
    int epochs;
    double lr;
};

// ─── tokenizer（与 Inference.hpp 的 qqnt::Tokenizer 逐行一致）────────────
namespace qqnt
{
struct Tokenizer
{
    std::unordered_map<uint8_t, std::string> id_to_token;
    std::unordered_map<std::string, uint8_t> token_to_id;

    Tokenizer()
    {
        for (int i = 0; i < 128; ++i)
        {
            std::string ch(1, static_cast<char>(i));
            id_to_token[i] = ch;
            token_to_id[ch] = i;
        }
        std::vector<std::string> subwords = {
            " chaos", " fluid", " quantum", " state", " phase",
            " space", " time", " entropy", " dynamics", " prediction",
            " anomaly", " detected", " stable", " unstable", " metric",
            " tensor", " field", " particle", " wave", " collapse",
            " The", " system", " is", " exhibiting", " non-linear",
            " resonance", " pattern", " coherent", " decoherence", " matrix",
            " probability", " distribution", " entanglement", " manifold"};
        for (size_t i = 0; i < subwords.size(); ++i)
        {
            uint8_t id = 128 + i;
            id_to_token[id] = subwords[i];
            token_to_id[subwords[i]] = id;
        }
        for (int i = 128 + subwords.size(); i < 256; ++i)
        {
            char hex[5];
            snprintf(hex, sizeof(hex), "<%02X>", i);
            id_to_token[i] = std::string(hex);
            token_to_id[std::string(hex)] = i;
        }
    }

    std::string decode(uint8_t id) { return id_to_token[id]; }

    std::vector<uint8_t> encode(const std::string &text)
    {
        std::vector<uint8_t> tokens;
        for (char c : text) tokens.push_back(static_cast<uint8_t>(c));
        return tokens;
    }
};

Tokenizer &tokenizer()
{
    static Tokenizer t;
    return t;
}
} // namespace qqnt

// ─── 推理内核（与 Inference.hpp 同构，返回错误消息而非抛异常）──────────
QModelJob *qlm_load_internal(const char *filepath)
{
    std::cout << "[QML Android] Deserializing model from: " << filepath << "\n";
    QModelJob *job = new QModelJob();
    try
    {
        auto loaded_model = qml::ModelExporter<double>::load(filepath);
        if (loaded_model.tensor.size() == 0)
        {
            std::cerr << "[QML Android Error] QKM file contains no parameters: " << filepath << "\n";
            delete job;
            return nullptr;
        }
        auto *weights = new std::vector<double>(loaded_model.tensor.size());
        std::memcpy(weights->data(), loaded_model.tensor.data(), loaded_model.tensor.size() * sizeof(double));
        job->prompt_data = weights;
        std::cout << "[QML Android] Target Model Online: " << weights->size() << " parameters\n";
        for (const auto &kv : loaded_model.metadata)
            std::cout << "                -> " << kv.first << ": " << kv.second << "\n";
    }
    catch (const std::exception &e)
    {
        std::cerr << "[QML Android Error] Failed to parse QKM file '" << filepath << "': " << e.what() << "\n";
        delete job;
        return nullptr;
    }
    return job;
}

QObject *qlm_encode_internal(const char *prompt)
{
    std::string prompt_str(prompt ? prompt : "");
    QObject *tensor = new QObject();
    size_t n_q = 8 * 2;
    for (size_t i = 0; i < n_q; ++i)
    {
        size_t q_id = rvdl_alloc_qubit_id();
        tensor->hardware_ids.push_back(q_id);
        if (global_qm) global_qm->allocate_qubit(q_id);
    }
    tensor->qlm_data = new std::string(prompt_str);
    tensor->data_kind = QOBJ_DATA_STRING;
    return tensor;
}

// 返回 nullptr = 推理成功；非空 = 错误文本（写进输出串交给调用方）。
const char *qlm_forward_internal(QModelJob *model, QObject *input)
{
    if (!model)
    {
        if (input && input->qlm_data)
            *static_cast<std::string *>(input->qlm_data) =
                "[QML ERROR] model not loaded: qlm_load failed";
        return "[QML ERROR] model not loaded";
    }
    if (!global_qm)
    {
        if (input && input->qlm_data)
            *static_cast<std::string *>(input->qlm_data) =
                "[QML ERROR] no active quantum backend";
        return "[QML ERROR] no active quantum backend";
    }
    auto *weights = static_cast<std::vector<double> *>(model->prompt_data);
    size_t n_q = input->hardware_ids.size();
    std::string *generated_sequence = static_cast<std::string *>(input->qlm_data);

    // LF2C 无损特征快速上下文压缩（与 Inference.hpp / Transmitter 训练侧一致）
    std::vector<uint8_t> current_tokens = qqnt::tokenizer().encode(*generated_sequence);
    qlm::lf2c2::encode_context(global_qm, qlm::lf2c2::ContextConfig{}, current_tokens,
                               input->hardware_ids);

    size_t param_idx = 0;
    size_t num_layers = (weights->size() > 0) ? (weights->size() / (n_q + 8)) : 1;
    if (num_layers == 0) num_layers = 1;

    for (size_t t = 0; t < num_layers; ++t)
    {
        double N_t = std::exp(-0.1 * static_cast<double>(t));
        for (size_t i = 0; i < n_q; ++i)
        {
            if (param_idx < weights->size())
                global_qm->apply_rz(input->hardware_ids[i], (*weights)[param_idx++] * N_t);
        }
        for (size_t i = 0; i < 8; ++i)
        {
            global_qm->apply_cnot(input->hardware_ids[i], input->hardware_ids[i + 8]);
            if (param_idx < weights->size())
                global_qm->apply_rz(input->hardware_ids[i + 8], (*weights)[param_idx++] * N_t);
        }
    }

    uint8_t measured_token = 0;
    for (size_t i = 0; i < 8; ++i)
    {
        int bit = global_qm->measure(input->hardware_ids[i + 8]);
        if (bit == 1) measured_token |= (1 << i);
        if (bit == 1) global_qm->apply_x(input->hardware_ids[i + 8]);
        if (global_qm->measure(input->hardware_ids[i]) == 1)
            global_qm->apply_x(input->hardware_ids[i]);
    }

    std::string decoded_text = qqnt::tokenizer().decode(measured_token);
    *generated_sequence += decoded_text;
    return nullptr;
}

std::string qlm_decode_internal(QObject *output)
{
    std::string *generated_sequence = static_cast<std::string *>(output->qlm_data);
    std::string result = generated_sequence ? *generated_sequence : std::string();
    return result;
}

void qlm_release_model_internal(QModelJob *job)
{
    if (!job) return;
    delete static_cast<std::vector<double> *>(job->prompt_data);
    delete job;
}

void qlm_release_object_internal(QObject *obj)
{
    if (!obj) return;
    if (global_qm)
        for (size_t q : obj->hardware_ids) global_qm->release_qubit(q);
    for (size_t q : obj->hardware_ids) rvdl_free_qubit_id(q);
    if (obj->qlm_data && obj->data_kind == QOBJ_DATA_STRING)
        delete static_cast<std::string *>(obj->qlm_data);
    delete obj;
}

// ============================================================================
// FFI 接口（C ABI，供 Flutter/Dart FFI 或任意语言直接 dlopen 调用）
// ============================================================================
// 与 JNI 同一内核；Flutter 端用这套接口（不需要 Java/Kotlin 胶水）。
// 指针即句柄：qlm_ffi_load -> 模型，qlm_ffi_encode -> 输入对象；
// 用完必须 free（否则量子态/权重泄漏）。
extern "C"
{
    // 初始化全局量子后端（幂等）。返回 0 成功、-1 失败。
    int qlm_ffi_init()
    {
        if (global_qm) return 0;
#ifdef QLM_USE_QVM
        global_qm = new qhal::QVM();
#else
        static qlm_cpu::BasicQVM backend;
        global_qm = &backend;
#endif
        return global_qm ? 0 : -1;
    }

    void qlm_ffi_shutdown()
    {
#ifdef QLM_USE_QVM
        if (global_qm) delete global_qm;
#endif
        global_qm = nullptr;
    }

    void *qlm_ffi_load(const char *model_path) { return qlm_load_internal(model_path); }
    void *qlm_ffi_encode(const char *prompt) { return qlm_encode_internal(prompt); }

    // 单步前向：0 成功；-1 失败（错误原因已写入输出串，decode 后可见）。
    int qlm_ffi_forward(void *model, void *input)
    {
        return qlm_forward_internal(static_cast<QModelJob *>(model), static_cast<QObject *>(input)) ? -1 : 0;
    }

    // 返回的指针由库持有（线程局部静态缓冲）；调用方应在下一次调用前拷贝。
    const char *qlm_ffi_decode(void *output)
    {
        static thread_local std::string buf;
        buf = qlm_decode_internal(static_cast<QObject *>(output));
        return buf.c_str();
    }

    void qlm_ffi_free_model(void *model) { qlm_release_model_internal(static_cast<QModelJob *>(model)); }
    void qlm_ffi_free_object(void *object) { qlm_release_object_internal(static_cast<QObject *>(object)); }
}

// ============================================================================
// JNI 接口（配合 runtime_android.cpp 的 com.ravendrel.RavendrelRuntime）
// ============================================================================
#ifdef __ANDROID__
extern "C"
{
    JNIEXPORT jlong JNICALL
    Java_com_ravendrel_RavendrelRuntime_nativeQlmLoad(JNIEnv *env, jobject, jstring path)
    {
        const char *chars = env->GetStringUTFChars(path, nullptr);
        QModelJob *job = qlm_load_internal(chars);
        env->ReleaseStringUTFChars(path, chars);
        return reinterpret_cast<jlong>(job);
    }

    JNIEXPORT jlong JNICALL
    Java_com_ravendrel_RavendrelRuntime_nativeQlmEncode(JNIEnv *env, jobject, jstring prompt)
    {
        const char *chars = env->GetStringUTFChars(prompt, nullptr);
        QObject *obj = qlm_encode_internal(chars);
        env->ReleaseStringUTFChars(prompt, chars);
        return reinterpret_cast<jlong>(obj);
    }

    JNIEXPORT void JNICALL
    Java_com_ravendrel_RavendrelRuntime_nativeQlmForward(JNIEnv *, jobject, jlong model, jlong input)
    {
        qlm_forward_internal(reinterpret_cast<QModelJob *>(model), reinterpret_cast<QObject *>(input));
    }

    JNIEXPORT jstring JNICALL
    Java_com_ravendrel_RavendrelRuntime_nativeQlmDecode(JNIEnv *env, jobject, jlong output)
    {
        std::string s = qlm_decode_internal(reinterpret_cast<QObject *>(output));
        return env->NewStringUTF(s.c_str());
    }

    JNIEXPORT void JNICALL
    Java_com_ravendrel_RavendrelRuntime_nativeQlmReleaseModel(JNIEnv *, jobject, jlong model)
    {
        qlm_release_model_internal(reinterpret_cast<QModelJob *>(model));
    }

    JNIEXPORT void JNICALL
    Java_com_ravendrel_RavendrelRuntime_nativeQlmReleaseObject(JNIEnv *, jobject, jlong obj)
    {
        qlm_release_object_internal(reinterpret_cast<QObject *>(obj));
    }
}
#else
// ─── 桌面自检 ────────────────────────────────────────────────────────────
// 同一份推理内核在桌面跑通 = Android 移植面已经被证明没有隐藏依赖。
// 用法： qlm_selftest <model.rvdlm> [prompt]
int main(int argc, char **argv)
{
    if (argc < 2)
    {
        std::cerr << "用法: qlm_selftest <model.rvdlm> [prompt]\n";
        return 2;
    }
    const char *model_path = argv[1];
    const char *prompt = argc > 2 ? argv[2] : "The system is exhibiting";

#ifdef QLM_USE_QVM
    global_qm = new qhal::QVM();
#else
    static qlm_cpu::BasicQVM basic;
    global_qm = &basic;
#endif

    QModelJob *m = qlm_load_internal(model_path);
    if (!m)
    {
        std::cerr << "[selftest] qlm_load FAILED\n";
        return 1;
    }

    QObject *obj = qlm_encode_internal(prompt);
    const int MAX_STEPS = 24; // 与 Inference.hpp 的 max_new_tokens 一致
    for (int i = 0; i < MAX_STEPS; ++i)
    {
        const char *err = qlm_forward_internal(m, obj);
        if (err)
        {
            std::cerr << "[selftest] forward FAILED at step " << i << ": " << err << "\n";
            return 1;
        }
    }
    std::string out = qlm_decode_internal(obj);
    std::cout << "\n[selftest] OUTPUT(" << out.size() << " bytes): [" << out << "]\n";

    qlm_release_object_internal(obj);
    qlm_release_model_internal(m);
#ifdef QLM_USE_QVM
    delete global_qm;
#endif
    std::cout << "[selftest] OK\n";
    return 0;
}
#endif