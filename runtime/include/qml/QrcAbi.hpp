#pragma once
#include "Reservoir.hpp"
#include "Inference.hpp"

struct __attribute__((visibility("hidden"))) QReservoirHandle
{
    qml::QuantumReservoir *impl = nullptr;
};

extern "C"
{
    RVDL_HOST_EXPORT QReservoirHandle *rvdl_qrc_new(int32_t qubits, int32_t layers);
    RVDL_HOST_EXPORT QReservoirHandle *rvdl_qrc_new_ex(int32_t qubits, int32_t layers, int32_t out_dim);
    RVDL_HOST_EXPORT void rvdl_qrc_train(QReservoirHandle *res, int32_t epochs, double lr);
    RVDL_HOST_EXPORT void rvdl_qrc_train_ex(QReservoirHandle *res, int32_t epochs, double lr,
                                           const double *inputs, const double *targets,
                                           int32_t n_samples, int32_t n_features, int32_t n_outputs);
    RVDL_HOST_EXPORT QObject *rvdl_qrc_probe(QReservoirHandle *res, QObject *data);
    RVDL_HOST_EXPORT QObject *rvdl_qrc_predict(QReservoirHandle *res, QObject *data);
    RVDL_HOST_EXPORT void rvdl_qrc_release(QReservoirHandle *res);
}

#if defined(RVDL_RT_BUILD)

QReservoirHandle *rvdl_qrc_new_ex(int32_t qubits, int32_t layers, int32_t out_dim)
{
    if (!global_qm || qubits <= 0 || layers <= 0 || out_dim <= 0)
        return nullptr;
    auto *h = new QReservoirHandle();
    h->impl = new qml::QuantumReservoir(global_qm,
                                        static_cast<size_t>(qubits),
                                        static_cast<size_t>(layers),
                                        static_cast<size_t>(out_dim));
    std::cout << "[QRC ABI] Reservoir created (" << qubits << " qubits, "
              << layers << " layers, out_dim=" << out_dim << ").\n";
    return h;
}

QReservoirHandle *rvdl_qrc_new(int32_t qubits, int32_t layers)
{
    return rvdl_qrc_new_ex(qubits, layers, 1);
}

void rvdl_qrc_train_ex(QReservoirHandle *res, int32_t epochs, double lr,
                     const double *inputs, const double *targets,
                     int32_t n_samples, int32_t n_features, int32_t n_outputs)
{
    if (!res || !res->impl || !inputs || !targets)
        return;
    if (n_samples <= 0 || n_features <= 0 || n_outputs <= 0)
        return;

    const size_t ns = static_cast<size_t>(n_samples);
    const size_t nf = static_cast<size_t>(n_features);
    const size_t no = static_cast<size_t>(n_outputs);

    std::vector<std::vector<double>> in(ns);
    std::vector<std::vector<double>> tgt(ns);
    for (size_t s = 0; s < ns; ++s)
    {
        in[s].assign(inputs + s * nf, inputs + s * nf + nf);
        tgt[s].assign(targets + s * no, targets + s * no + no);
    }

    res->impl->train(in, tgt, epochs, lr);
}

void rvdl_qrc_train(QReservoirHandle *res, int32_t epochs, double lr)
{
    if (!res || !res->impl)
        return;

    size_t nq = res->impl->qubits();
    size_t N = 200;
    std::vector<std::vector<double>> inputs, targets;
    inputs.reserve(N);
    targets.reserve(N);
    for (size_t s = 0; s < N; ++s)
    {
        std::vector<double> in(nq, 0.0);
        for (size_t i = 0; i < nq; ++i)
            in[i] = 0.5 + 0.5 * std::sin(0.05 * static_cast<double>(s) + 0.3 * static_cast<double>(i));
        std::vector<double> tgt(1, std::sin(0.05 * static_cast<double>(s + 1)));
        inputs.push_back(std::move(in));
        targets.push_back(std::move(tgt));
    }

    res->impl->train(inputs, targets, epochs, lr);
}

namespace
{
    QObject *encode_features_to_object(const std::vector<double> &feats)
    {
        if (!global_qm)
            return nullptr;
        size_t n = feats.size();
        QObject *out = new QObject();
        for (size_t i = 0; i < n; ++i)
        {
            size_t qid = rvdl_alloc_qubit_id();
            out->hardware_ids.push_back(qid);
            global_qm->allocate_qubit(qid);
            double angle = (feats[i] + 1.0) * M_PI;
            global_qm->apply_h(qid);
            global_qm->apply_rz(qid, angle);
        }
        out->qlm_data = nullptr;
        out->data_kind = QOBJ_DATA_NONE;
        return out;
    }
}

QObject *rvdl_qrc_probe(QReservoirHandle *res, QObject *data)
{
    if (!res || !res->impl)
        return nullptr;
    size_t nq = res->impl->qubits();

    std::vector<double> input(nq, 0.0);
    if (data)
    {
        for (size_t i = 0; i < data->hardware_ids.size() && i < nq; ++i)
        {
            int b = global_qm->measure(data->hardware_ids[i]);
            input[i] = b ? 1.0 : 0.0;
        }
    }

    auto feats = res->impl->evolve(input);
    return encode_features_to_object(feats);
}

QObject *rvdl_qrc_predict(QReservoirHandle *res, QObject *data)
{
    if (!res || !res->impl)
        return nullptr;
    size_t nq = res->impl->qubits();

    std::vector<double> input(nq, 0.0);
    if (data)
    {
        for (size_t i = 0; i < data->hardware_ids.size() && i < nq; ++i)
        {
            int b = global_qm->measure(data->hardware_ids[i]);
            input[i] = b ? 1.0 : 0.0;
        }
    }

    auto pred = res->impl->predict(input);
    return encode_features_to_object(pred);
}

void rvdl_qrc_release(QReservoirHandle *res)
{
    if (!res)
        return;
    delete res->impl;
    res->impl = nullptr;
    delete res;
    std::cout << "[QRC ABI] Reservoir released.\n";
}

#endif