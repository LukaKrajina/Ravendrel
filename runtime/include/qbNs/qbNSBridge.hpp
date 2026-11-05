#pragma once
#include <memory>
#include <string>
#include <vector>
#include <cmath>
#include <iostream>
#include "../qml/Inference.hpp"
#include "qbNs.hpp"
#include "../qlm/QLM.hpp"

namespace qbns_bridge
{
    // ─── 合成神经信号源 ─────────────────────────────────────────────
    // 在没有真实 BMI 传感器接入时，按 modality 字符串生成确定性的
    inline qbns::NeuralStream make_stream()
    {
        const size_t channels = 4;
        const size_t steps = 8;
        numqk::Tensor<double> samples({channels, steps});
        double *d = samples.data();
        for (size_t c = 0; c < channels; ++c)
        {
            for (size_t t = 0; t < steps; ++t)
            {
                d[c * steps + t] = std::sin(0.6 * static_cast<double>(c + 1) * static_cast<double>(t) + 0.5);
            }
        }
        return qbns::NeuralStream{std::move(samples), channels, steps, 1000.0};
    }

    inline qbns::SpikeTrain make_spike()
    {
        qbns::SpikeTrain s;
        s.num_channels = 4;
        s.window_duration_ms = 1000.0;
        s.channel_spikes = {
            {true, false, true, false, false, true, false, false},
            {false, true, false, false, true, false, true, false},
            {false, false, false, true, false, false, false, true},
            {true, false, false, false, false, false, true, false}};
        return s;
    }

    inline qbns::LocalFieldPotential make_lfp()
    {
        qbns::LocalFieldPotential s;
        s.sampling_rate_hz = 1000.0;
        s.values = {0.1, 0.3, 0.5, 0.7, 0.9, 1.1, 0.9, 0.7, 0.5, 0.3, 0.1, -0.1, -0.3, -0.5, -0.3, -0.1};
        return s;
    }

    inline qbns::EEGSpectrum make_eeg()
    {
        qbns::EEGSpectrum s;
        s.delta_power = 0.42;
        s.theta_power = 0.31;
        s.alpha_power = 0.58;
        s.beta_power = 0.22;
        s.gamma_power = 0.15;
        return s;
    }

    inline qbns::QuantumSensorReading make_sensor()
    {
        qbns::QuantumSensorReading s;
        s.sensor_type = "NV-Center";
        s.sensitivity_t_per_sqrt_hz = 1e-15;
        s.readings = {0.11, 0.23, 0.35, 0.47, 0.59, 0.41, 0.29, 0.17};
        return s;
    }

    // 复用 daemon 启动时探测到的计算后端（global_qm == ActiveBackend），
    // 保证编码与释放都在同一后端上，与 rvdl_encode_text 的模式完全一致。
    inline qbns::Transducer &get_transducer()
    {
        static qbns::Transducer instance(global_qm);
        return instance;
    }
}

extern "C"
{
    RVDL_HOST_EXPORT void *rvdl_mind_read(const char *modality);
    RVDL_HOST_EXPORT void rvdl_mind_train(void *qobj_ptr, int epochs, double lr);
    RVDL_HOST_EXPORT void rvdl_mind_feedback(void *qobj_ptr);
}

#if defined(RVDL_RT_BUILD)
void *rvdl_mind_read(const char *modality)
{
    if (!global_qm)
    {
        std::cerr << "[QBNS] mind_read: quantum backend offline.\n";
        return nullptr;
    }

    std::string m = modality ? modality : "eeg";
    qbns::Transducer &transducer = qbns_bridge::get_transducer();
    std::shared_ptr<rvdl::QDataState> encoded;

    if (m == "stream")
        encoded = transducer.amplitude_encode(qbns_bridge::make_stream());
    else if (m == "spike")
        encoded = transducer.spike_to_basis(qbns_bridge::make_spike());
    else if (m == "lfp")
        encoded = transducer.lfp_to_phase(qbns_bridge::make_lfp());
    else if (m == "eeg")
        encoded = transducer.eeg_to_entangled(qbns_bridge::make_eeg());
    else if (m == "sensor")
        encoded = transducer.sensor_to_state(qbns_bridge::make_sensor());
    else
    {
        std::cerr << "[QBNS] Unknown modality '" << m << "'. Falling back to 'eeg'.\n";
        encoded = transducer.eeg_to_entangled(qbns_bridge::make_eeg());
    }

    QObject *obj = new QObject();
    obj->hardware_ids = encoded->get_ids();
    obj->qlm_data = new std::shared_ptr<rvdl::QObject>(encoded);
    obj->data_kind = QOBJ_DATA_SHARED;
    return obj;
}

void rvdl_mind_train(void *qobj_ptr, int epochs, double lr)
{
    QObject *obj = static_cast<QObject *>(qobj_ptr);
    if (!obj || !obj->qlm_data || !global_qm)
    {
        std::cerr << "[QBNS] mind_train: invalid state object or backend.\n";
        return;
    }

    auto *sp = static_cast<std::shared_ptr<rvdl::QObject> *>(obj->qlm_data);
    if ((*sp)->qlm_data == nullptr)
    {
        (*sp)->qlm_data = new uint8_t(0);
    }

    std::vector<std::shared_ptr<rvdl::QObject>> dataset;
    dataset.push_back(*sp);

    std::cout << "[QBNS] Brain training QLM (" << epochs
              << " epochs, lr=" << lr << ")...\n";

    qlm::QLM learning_machine(global_qm, 16, 6);
    learning_machine.train_and_export(epochs, lr, "brain_trained_model.rvdlm", dataset);

    delete static_cast<uint8_t *>((*sp)->qlm_data);
    (*sp)->qlm_data = nullptr;
}

void rvdl_mind_feedback(void *qobj_ptr)
{
    QObject *obj = static_cast<QObject *>(qobj_ptr);
    if (!obj || !obj->qlm_data)
    {
        std::cerr << "[QBNS] mind_feedback: invalid state object.\n";
        return;
    }

    auto *sp = static_cast<std::shared_ptr<rvdl::QObject> *>(obj->qlm_data);
    std::vector<int> results = (*sp)->measure();

    std::cout << "[QBNS] Neurofeedback loop: measured [";
    for (size_t i = 0; i < results.size(); ++i)
    {
        if (i > 0) std::cout << ", ";
        std::cout << results[i];
    }
    std::cout << "] -> stimulating motor cortex.\n";
}
#endif