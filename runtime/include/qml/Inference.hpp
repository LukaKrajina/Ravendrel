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
#include "../qlm/LF2C2.hpp"
#include "QubitAllocator.hpp"
#include "../../src/QObject.hpp"
#include "../qml/RVDLModelFormat.hpp"

#ifndef RVDL_ABI_TYPES_DEFINED
#define RVDL_ABI_TYPES_DEFINED

enum QObjectDataKind : int
{
    QOBJ_DATA_NONE = 0,
    QOBJ_DATA_SHARED = 1,
    QOBJ_DATA_STRING = 2
};

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
    size_t num_qubits = 16;
};

#endif


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
        std::cout << "[QML Inference] Target Model Online (" << n_q
                  << " qubits, " << (weights->size() / params_per_layer)
                  << " layers).\n";

        for (const auto &kv : loaded_model.metadata)
        {
            std::cout << "                -> " << kv.first << ": " << kv.second << "\n";
        }

        std::cout << "                -> Active Parameters: " << weights->size() << "\n";
    }
    catch (const std::exception &e)
    {
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
    const size_t n_q = (model->num_qubits >= 8) ? model->num_qubits : 16;
    std::cout << "[QML Inference] Executing " << n_q
              << "-Qubit Quantum Attention Generation...\n";
    auto *weights = static_cast<std::vector<double> *>(model->prompt_data);
    std::string *generated_sequence = static_cast<std::string *>(input->qlm_data);
    int max_new_tokens = 24;
    (void)max_new_tokens;

    while (input->hardware_ids.size() < n_q)
    {
        const size_t q_id = rvdl_alloc_qubit_id();
        input->hardware_ids.push_back(q_id);
        if (global_qm)
            global_qm->allocate_qubit(q_id);
    }
    std::vector<size_t> ids(input->hardware_ids.begin(),
                            input->hardware_ids.begin() + n_q);

    std::vector<uint8_t> current_tokens = qqnt::global_tokenizer.encode(*generated_sequence);
    qlm::lf2c2::encode_context(global_qm,
                                qlm::lf2c2::context_config_for_qubits(n_q),
                                current_tokens, ids);

    const size_t readout_base = n_q - 8;
    const size_t params_per_layer = n_q + 8;
    size_t param_idx = 0;
    size_t num_layers = (weights->size() > 0)
                            ? (weights->size() / params_per_layer)
                            : 1;
    if (num_layers == 0)
        num_layers = 1;

    for (size_t t = 0; t < num_layers; ++t)
    {
        double N_t = std::exp(-0.1 * static_cast<double>(t));

        for (size_t i = 0; i < n_q; ++i)
        {
            if (param_idx < weights->size())
            {
                global_qm->apply_ry(ids[i], (*weights)[param_idx++] * N_t);
            }
        }

        for (size_t c = 0; c + 8 < n_q; ++c)
            global_qm->apply_cnot(ids[c], ids[readout_base + (c % 8)]);
        for (size_t j = 0; j < 8; ++j)
        {
            if (param_idx < weights->size())
                global_qm->apply_rz(ids[readout_base + j],
                                     (*weights)[param_idx++] * N_t);
        }
    }

    uint8_t measured_token = 0;
    for (size_t j = 0; j < 8; ++j)
    {
        const size_t q = readout_base + j;
        const int bit = global_qm->measure(ids[q]);
        if (bit == 1)
            measured_token |= (1 << j);
        if (bit == 1)
            global_qm->apply_x(ids[q]);
    }
    for (size_t c = 0; c < readout_base; ++c)
        if (global_qm->measure(ids[c]) == 1)
            global_qm->apply_x(ids[c]);

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