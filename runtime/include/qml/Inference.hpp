#pragma once

#include <iostream>
#include <string>
#include <vector>
#include <fstream>
#include <cstring>
#include <unordered_map>
#include <cmath>
#include <atomic>
#include <mutex>
#include <algorithm>

#include "../qhal/Export.hpp"

#ifndef RVDL_HOST_EXPORT
#define RVDL_HOST_EXPORT extern "C" RVDL_RT_API
#endif

#include "../qhal/QM.hpp"
#include "../qlm/LF2C2.hpp"   // 无损特征快速上下文压缩（上下文编码，训练/推理共用）
#include "QubitAllocator.hpp" // rvdl_alloc_qubit_id / rvdl_free_qubit_id / next_available_qubit
#include "../../src/QObject.hpp"
#include "../qml/RVDLModelFormat.hpp"

#ifndef RVDL_ABI_TYPES_DEFINED
#define RVDL_ABI_TYPES_DEFINED

enum QObjectDataKind : int
{
    QOBJ_DATA_NONE = 0,
    QOBJ_DATA_SHARED = 1, // std::shared_ptr<rvdl::QObject>*
    QOBJ_DATA_STRING = 2  // std::string*
};

// 注意：此结构体是全局命名空间 ::QObject，与 Qt 的 ::QObject 同名。
// 若导出其符号（隐式析构 _ZN7QObjectD2Ev）会与 Qt6Core 冲突，导致 Qt 对象
// 析构被错误绑定到本结构体而触发 free(): invalid pointer 崩溃。
// 用 visibility("hidden") 隐藏符号，仅通过 extern "C" 接口以指针传递。
struct __attribute__((visibility("hidden"))) QObject
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

#endif

// 全局 qubit id 分配器（next_available_qubit / rvdl_alloc_qubit_id / rvdl_free_qubit_id）
// 已拆分到 QubitAllocator.hpp（供 src/QObject.hpp 与本头共用，打破互相包含）。

extern RVDL_RT_API qhal::IQuantumBackend *global_qm;
extern RVDL_RT_API std::string global_string_buffer;

namespace qqnt
{
    class Tokenizer
    {
    private:
        std::unordered_map<uint8_t, std::string> id_to_token;
        std::unordered_map<std::string, uint8_t> token_to_id;

    public:
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

        std::string decode(uint8_t id)
        {
            return id_to_token[id];
        }

        std::vector<uint8_t> encode(const std::string &text)
        {
            std::vector<uint8_t> tokens;
            for (char c : text)
            {
                tokens.push_back(static_cast<uint8_t>(c));
            }
            return tokens;
        }
    };

    static Tokenizer global_tokenizer;
}

extern "C"
{
    RVDL_HOST_EXPORT QModelJob *rvdl_qlm_load(const char *filepath);
    RVDL_HOST_EXPORT QObject *rvdl_encode_string(const char *prompt);
    RVDL_HOST_EXPORT void rvdl_qlm_forward(QModelJob *model, QObject *input);
    RVDL_HOST_EXPORT const char *rvdl_decode_string(QObject *output);
}

#if defined(RVDL_RT_BUILD)
QModelJob *rvdl_qlm_load(const char *filepath)
{
    std::cout << "[QML Inference] Deserializing model from: " << filepath << "\n";
    QModelJob *job = new QModelJob();
    try
    {
        auto loaded_model = qml::ModelExporter<double>::load(filepath);
        if (loaded_model.tensor.size() == 0)
        {
            std::cerr << "[QML Inference Error] QKM file contains no parameters: " << filepath << "\n";
            delete job;
            return nullptr;
        }
        auto *weights = new std::vector<double>(loaded_model.tensor.size());
        std::memcpy(weights->data(), loaded_model.tensor.data(), loaded_model.tensor.size() * sizeof(double));
        job->prompt_data = weights;
        std::cout << "[QML Inference] Target Model Online.\n";

        for (const auto &kv : loaded_model.metadata)
        {
            std::cout << "                -> " << kv.first << ": " << kv.second << "\n";
        }

        std::cout << "                -> Active Parameters: " << weights->size() << "\n";
    }
    catch (const std::exception &e)
    {
        // 错误路径：模型解析失败返回 nullptr，绝不静默替换成假权重
        // （旧实现用 4 个魔数继续"推理"，安静地产出看起来正常的垃圾 token）。
        // 调用方 rvdl_qlm_forward 会检测 nullptr 并响亮报错。
        std::cerr << "[QML Inference Error] Failed to parse QKM file '" << filepath << "': " << e.what() << "\n";
        delete job;
        return nullptr;
    }

    return job;
}

QObject *rvdl_encode_string(const char *prompt)
{
    std::cout << "[QQNT] Tokenizing semantic prompt via Quantum-Native Tokenizer...\n";
    std::string prompt_str(prompt);
    std::vector<uint8_t> tokens = qqnt::global_tokenizer.encode(prompt_str);
    QObject *tensor = new QObject();
    size_t n_q = 8 * 2;
    for (size_t i = 0; i < n_q; ++i)
    {
        size_t q_id = rvdl_alloc_qubit_id();
        tensor->hardware_ids.push_back(q_id);
        if (global_qm)
            global_qm->allocate_qubit(q_id);
    }

    tensor->qlm_data = new std::string(prompt);
    tensor->data_kind = QOBJ_DATA_STRING;
    return tensor;
}

void rvdl_qlm_forward(QModelJob *model, QObject *input)
{
    if (!model)
    {
        // qlm_load 返回 nullptr（解析失败）时的错误路径：中止推理并把错误文本
        // 写入输出序列，经 rvdl_decode_string 返回给 API 调用方（stderr 已由 load 输出）。
        std::cerr << "[QML Inference Error] rvdl_qlm_forward called without a loaded model.\n";
        if (input && input->qlm_data)
            *static_cast<std::string *>(input->qlm_data) =
                "[QML ERROR] model not loaded: qlm_load failed (see daemon stderr)";
        return;
    }
    if (!global_qm)
    {
        std::cerr << "[QML Inference Error] no active quantum backend (global_qm == nullptr).\n";
        if (input && input->qlm_data)
            *static_cast<std::string *>(input->qlm_data) =
                "[QML ERROR] no active quantum backend";
        return;
    }
    std::cout << "[QML Inference] Executing 16-Qubit Quantum Attention Generation...\n";
    auto *weights = static_cast<std::vector<double> *>(model->prompt_data);
    size_t n_q = input->hardware_ids.size();
    std::string *generated_sequence = static_cast<std::string *>(input->qlm_data);
    int max_new_tokens = 24;

    // ── 上下文：LF2C 无损特征快速上下文压缩 ─────────────────────────
    //
    // 原来：取生成串的**最后 2 个 token**，用 apply_x 打进 16 个 qubit
    //       （QLM 的 context.window = "2_Tokens"）。问题有三个：
    //         ① 窗口只有 2 个 token，撑不起任何长程依赖；
    //         ② 生成串无界增长（下面 `*generated_sequence += decoded_text`），
    //            而窗口只看最后 2 个 —— 中间全部丢掉；
    //         ③ qubit 预算是硬约束（16 qubit 已经是本仓常用规模），
    //            每多看一个 token 就要多占 8 个 qubit。
    //
    // 现在：取最近 16 个 token → 切块 → 特征 → **用特征构造上下文态**。
    //       **qubit 占用不变（16），覆盖的历史从 2 个 token 变成 16 个**，
    //       而且被压掉的那段不再是「丢弃」，而是进了特征。
    //
    // 训练侧（qml/Transmitter.cpp 的样本装配）走**同一个函数**
    // qlm::lf2c2::encode_context —— 两侧编码必须一致，否则模型是在训练
    // 分布之外被推理（train/test skew），输出是垃圾而且不报错。
    //
    // ⚠ 启用压缩上下文后，模型必须用同一套编码**重新训练**：旧权重是在
    //   「2 个原始 token 的比特」上学出来的，与「压缩特征的连续态」不是
    //   同一个输入分布。
    std::vector<uint8_t> current_tokens = qqnt::global_tokenizer.encode(*generated_sequence);
    qlm::lf2c2::encode_context(global_qm, qlm::lf2c2::ContextConfig{}, current_tokens,
                                input->hardware_ids);

    size_t param_idx = 0;
    size_t num_layers = (weights->size() > 0) ? (weights->size() / (n_q + 8)) : 1;
    if (num_layers == 0)
        num_layers = 1;

    for (size_t t = 0; t < num_layers; ++t)
    {
        double N_t = std::exp(-0.1 * static_cast<double>(t));

        for (size_t i = 0; i < n_q; ++i)
        {
            if (param_idx < weights->size())
            {
                global_qm->apply_rz(input->hardware_ids[i], (*weights)[param_idx++] * N_t);
            }
        }

        for (size_t i = 0; i < 8; ++i)
        {
            global_qm->apply_cnot(input->hardware_ids[i], input->hardware_ids[i + 8]);
            if (param_idx < weights->size())
            {
                global_qm->apply_rz(input->hardware_ids[i + 8], (*weights)[param_idx++] * N_t);
            }
        }
    }

    uint8_t measured_token = 0;
    for (size_t i = 0; i < 8; ++i)
    {
        int bit = global_qm->measure(input->hardware_ids[i + 8]);
        if (bit == 1)
        {
            measured_token |= (1 << i);
        }

        if (bit == 1)
            global_qm->apply_x(input->hardware_ids[i + 8]);
        if (global_qm->measure(input->hardware_ids[i]) == 1)
            global_qm->apply_x(input->hardware_ids[i]);
    }

    std::string decoded_text = qqnt::global_tokenizer.decode(measured_token);
    std::cout << decoded_text << std::flush;
    *generated_sequence += decoded_text;
}

const char *rvdl_decode_string(QObject *output)
{
    std::cout << "[QQNT] Returning semantically decoded quantum context...\n";
    std::string *generated_sequence = static_cast<std::string *>(output->qlm_data);

    static thread_local std::string safe_return_buffer;
    safe_return_buffer = *generated_sequence;
    global_string_buffer = safe_return_buffer;

    delete generated_sequence;
    output->qlm_data = nullptr;

    return safe_return_buffer.c_str();
}
#endif