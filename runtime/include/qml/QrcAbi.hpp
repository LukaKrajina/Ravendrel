#pragma once
//
// QrcAbi.hpp —— QRC 量子储备池的 C ABI 桥接层
//
// 把 qml::QuantumReservoir（P1）暴露为 rvdl 语言层可调用的 extern "C" 函数，
// 对应 ir.ts 声明的 @rvdl_qrc_new / @rvdl_qrc_train / @rvdl_qrc_probe / @rvdl_qrc_predict。
//
// 语义：
//   rvdl_qrc_new(qubits, layers)      -> 创建储备池（out_dim=1，标量预测演示）
//   rvdl_qrc_train(res, epochs, lr)   -> 用合成正弦时序训练线性读出（验证无贫瘠高原）
//   rvdl_qrc_probe(res, data)         -> 测量 data 的量子态作为输入，返回 ⟨Z⟩ 特征
//                                       编码回 QObject（num_qubits 个 qubit）
//   rvdl_qrc_predict(res, data)       -> 返回读出预测（out_dim 个 qubit 编码）
//
#include "Reservoir.hpp"
#include "Inference.hpp"

// ABI 层的 QReservoir 句柄（不透明；隐藏符号，避免与 Qt 的 ::QObject 等冲突）
struct __attribute__((visibility("hidden"))) QReservoirHandle
{
    qml::QuantumReservoir *impl = nullptr;
};

extern "C"
{
    RVDL_HOST_EXPORT QReservoirHandle *rvdl_qrc_new(int32_t qubits, int32_t layers);
    // out_dim 可显式指定（原 rvdl_qrc_new 固定 out_dim=1，仅够标量演示任务）。
    RVDL_HOST_EXPORT QReservoirHandle *rvdl_qrc_new_ex(int32_t qubits, int32_t layers, int32_t out_dim);
    RVDL_HOST_EXPORT void rvdl_qrc_train(QReservoirHandle *res, int32_t epochs, double lr);
    // 用真实训练数据训练（原 rvdl_qrc_train 只跑合成正弦序列，仅够自检）。
    RVDL_HOST_EXPORT void rvdl_qrc_train_ex(QReservoirHandle *res, int32_t epochs, double lr,
                                           const double *inputs, const double *targets,
                                           int32_t n_samples, int32_t n_features, int32_t n_outputs);
    RVDL_HOST_EXPORT QObject *rvdl_qrc_probe(QReservoirHandle *res, QObject *data);
    RVDL_HOST_EXPORT QObject *rvdl_qrc_predict(QReservoirHandle *res, QObject *data);
    RVDL_HOST_EXPORT void rvdl_qrc_release(QReservoirHandle *res);
}

#if defined(RVDL_RT_BUILD)

// out_dim 可显式指定：原先固定为 1（只够标量时序预测这类演示任务），
// 现由调用方按真实任务的输出维度给出（多维回归 / 多类读出等）。
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

// 兼容旧的两参数签名：out_dim 缺省为 1（标量输出）。
QReservoirHandle *rvdl_qrc_new(int32_t qubits, int32_t layers)
{
    return rvdl_qrc_new_ex(qubits, layers, 1);
}

// 用真实训练数据训练线性读出。
// inputs  : 扁平的 n_samples × n_features（行主序）
// targets : 扁平的 n_samples × n_outputs
// 这是生产路径：调用方提供自己的时序/回归数据。
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

// 兼容旧签名：用合成正弦序列训练（自检 / 验证无贫瘠高原的演示任务，
// 非生产路径 —— 真实数据请用 rvdl_qrc_train_ex）。
void rvdl_qrc_train(QReservoirHandle *res, int32_t epochs, double lr)
{
    if (!res || !res->impl)
        return;

    // 合成时序训练数据：正弦预测任务（QRC 经典演示，验证无贫瘠高原）
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
    // 把 ⟨Z⟩ 特征向量编码为 QObject（每维一个 qubit，相位编码）
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
            // 特征 [-1,1] -> 相位 [0, 2π]
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

    // 从 data 的量子态测量经典 bits 作为储备池输入
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

    auto pred = res->impl->predict(input); // out_dim 维
    return encode_features_to_object(pred);
}

void rvdl_qrc_release(QReservoirHandle *res)
{
    if (!res)
        return;
    // 析构 QuantumReservoir：回收储备池 qubit（坍缩到 |0⟩ 后 release）
    delete res->impl;
    res->impl = nullptr;
    delete res;
    std::cout << "[QRC ABI] Reservoir released.\n";
}

#endif