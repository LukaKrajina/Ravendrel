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
#ifdef QLM_USE_QVM
#include "qhal/QVM.hpp"
#else
#include "BasicQVM.hpp"
#endif

#include "qml/RVDLModelFormat.hpp"
#include "qml/QubitAllocator.hpp"
#include "qlm/LF2C2.hpp"


#ifdef __ANDROID__
extern qhal::IQuantumBackend *global_qm;
#else
qhal::IQuantumBackend *global_qm = nullptr;
#endif

enum QObjectDataKind : int
{
    QOBJ_DATA_NONE = 0,
    QOBJ_DATA_SHARED = 1,
    QOBJ_DATA_STRING = 2
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
    size_t num_qubits = 16;
};

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
}

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

        size_t n_q = 16;
        const auto qit = loaded_model.metadata.find("train.qubits");
        if (qit != loaded_model.metadata.end())
        {
            try
            {
                size_t pos = 0;
                n_q = static_cast<size_t>(std::stoul(qit->second, &pos));
                if (pos != qit->second.size())
                    throw std::invalid_argument("trailing characters");
            }
            catch (const std::exception &)
            {
                delete weights;
                throw std::runtime_error(
                    "Invalid train.qubits metadata: '" + qit->second + "'");
            }
        }
        if (n_q < 8 || n_q > 20)
        {
            delete weights;
            throw std::runtime_error(
                "Unsupported train.qubits=" + std::to_string(n_q) +
                " (expected 8..20)");
        }
        const size_t params_per_layer = n_q + 8;
        if (weights->size() == 0 || weights->size() % params_per_layer != 0)
        {
            delete weights;
            throw std::runtime_error(
                "Weight count " + std::to_string(weights->size()) +
                " is not a multiple of " + std::to_string(params_per_layer) +
                " (qubits=" + std::to_string(n_q) + ")");
        }
        job->num_qubits = n_q;
        job->prompt_data = weights;
        std::cout << "[QML Android] Target Model Online: " << weights->size()
                  << " parameters (" << n_q << " qubits, "
                  << (weights->size() / params_per_layer) << " layers)\n";
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
    const size_t n_q = (model->num_qubits >= 8) ? model->num_qubits : 16;
    std::string *generated_sequence = static_cast<std::string *>(input->qlm_data);

    while (input->hardware_ids.size() < n_q)
    {
        const size_t q_id = rvdl_alloc_qubit_id();
        input->hardware_ids.push_back(q_id);
        if (global_qm) global_qm->allocate_qubit(q_id);
    }
    std::vector<size_t> ids(input->hardware_ids.begin(),
                            input->hardware_ids.begin() + n_q);

    std::vector<uint8_t> current_tokens = qqnt::tokenizer().encode(*generated_sequence);
    qlm::lf2c2::encode_context(global_qm,
                               qlm::lf2c2::context_config_for_qubits(n_q),
                               current_tokens, ids);

    const size_t readout_base = n_q - 8;
    size_t param_idx = 0;
    size_t num_layers = (weights->size() > 0) ? (weights->size() / (n_q + 8)) : 1;
    if (num_layers == 0) num_layers = 1;

    for (size_t t = 0; t < num_layers; ++t)
    {
        double N_t = std::exp(-0.1 * static_cast<double>(t));
        for (size_t i = 0; i < n_q; ++i)
        {
            if (param_idx < weights->size())
                global_qm->apply_ry(ids[i], (*weights)[param_idx++] * N_t);
        }
        for (size_t c = 0; c + 8 < n_q; ++c)
            global_qm->apply_cnot(ids[c], ids[readout_base + (c % 8)]);
        for (size_t j = 0; j < 8; ++j)
        {
            if (param_idx < weights->size())
                global_qm->apply_rz(ids[readout_base + j], (*weights)[param_idx++] * N_t);
        }
    }

    uint8_t measured_token = 0;
    for (size_t j = 0; j < 8; ++j)
    {
        const size_t q = readout_base + j;
        const int bit = global_qm->measure(ids[q]);
        if (bit == 1) measured_token |= (1 << j);
        if (bit == 1) global_qm->apply_x(ids[q]);
    }
    for (size_t c = 0; c < readout_base; ++c)
        if (global_qm->measure(ids[c]) == 1)
            global_qm->apply_x(ids[c]);

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

extern "C"
{
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

    int qlm_ffi_forward(void *model, void *input)
    {
        return qlm_forward_internal(static_cast<QModelJob *>(model), static_cast<QObject *>(input)) ? -1 : 0;
    }

    const char *qlm_ffi_decode(void *output)
    {
        static thread_local std::string buf;
        buf = qlm_decode_internal(static_cast<QObject *>(output));
        return buf.c_str();
    }

    void qlm_ffi_free_model(void *model) { qlm_release_model_internal(static_cast<QModelJob *>(model)); }
    void qlm_ffi_free_object(void *object) { qlm_release_object_internal(static_cast<QObject *>(object)); }
}

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
    const int MAX_STEPS = 24;
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