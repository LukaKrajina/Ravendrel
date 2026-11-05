#pragma once
//
// QGT.hpp —— 完整量子几何张量（Fubini-Study 度量）与自然梯度
//
// 把 numqk/QuantumGeometry.hpp 接入 qml 层：用后端态矢量 + parameter-shift
// 计算**完整** QGT（含对角与非对角元），替代 QLM 中旧的对角 Fubini-Study
// 近似（estimate_fs_diagonal）。对应 TQNF 设计法则「几何-贫瘠高原对偶律」。
//
// 依赖后端维护态矢量（如 QVM 的 get_state_vector()）；对真实量子机（QM）
// 应回退到对角近似或 Hadamard 测试估测重叠。
//
#include "../numqk/QuantumGeometry.hpp"
#include "../qhal/IQuantumBackend.hpp"
#include <functional>
#include <vector>
#include <complex>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace qml
{

    // ── 从后端读取完整态矢量 → ket（numqk::CTensor）──────────────────
    // 仅对维护态矢量的后端（如 QVM）有意义；无态矢量时返回空张量。
    inline numqk::CTensor state_ket(const qhal::IQuantumBackend *be)
    {
        if (!be)
            return numqk::CTensor({0}, false);
        std::vector<std::complex<double>> sv = be->get_state_vector();
        numqk::CTensor ket({sv.size()}, false);
        for (size_t i = 0; i < sv.size(); ++i)
            ket.data()[i] = sv[i];
        return ket;
    }

    // ── 量子几何张量（Fubini-Study 度量）───────────────────────
    // state_fn(params) -> 态矢量 ket（应把参数化电路作用在 |0..0⟩ 上）。
    // 对每个参数 θ_i 做 ±π/2 parameter-shift 得到导数态 |∂i ψ⟩，
    // 再由 numqk::fubini_study 组装完整度量 G_ij = Re[⟨∂i|∂j⟩-⟨∂i|ψ⟩⟨ψ|∂j⟩]。
    //
    // 注：±π/2 移位对 QVM 的 Rz(θ)=diag(1, e^{iθ}) 约定是精确导数；若状态
    // 准备函数用别的旋转约定，请改用对应的移位量。
    // params 会被临时移位并在结束时恢复，调用后 params 与入参一致。
    template <typename T>
    numqk::Tensor<double> quantum_geometric_tensor(
        std::function<numqk::CTensor(const numqk::Tensor<T> &)> state_fn,
        numqk::Tensor<T> &params)
    {
        const size_t n = params.size();
        numqk::CTensor psi = state_fn(params);

        std::vector<numqk::CTensor> dpsi;
        dpsi.reserve(n);
        for (size_t i = 0; i < n; ++i)
        {
            const T orig = params.data()[i];

            params.data()[i] = orig + T(M_PI / 2.0);
            numqk::CTensor plus = state_fn(params);

            params.data()[i] = orig - T(M_PI / 2.0);
            numqk::CTensor minus = state_fn(params);

            params.data()[i] = orig; // 恢复

            numqk::CTensor d({psi.size()}, false);
            for (size_t k = 0; k < psi.size(); ++k)
                d.data()[k] = T(0.5) * (plus.data()[k] - minus.data()[k]);
            dpsi.push_back(std::move(d));
        }
        return numqk::fubini_study(dpsi, psi);
    }

    // ── 自然梯度一步 ───────────────────────────────────────────
    // 给定欧氏梯度 ∇L（grad）与完整 QGT，返回自然梯度步 Δθ = G⁺ ∇L。
    // 调用方自行执行 θ -= lr · Δθ。
    inline numqk::Tensor<double> natural_gradient_step(
        const numqk::Tensor<double> &qgt, const numqk::Tensor<double> &grad)
    {
        return numqk::natural_gradient(qgt, grad);
    }

    // ── 动力学李代数诊断（ansatz 可训练性，Ragone 等）────────────────
    // generators 为门的哈密顿项（Hermitian 矩阵，如 Pauli 串），返回 DLA 维数。
    inline size_t ansatz_dla_dimension(const std::vector<numqk::CTensor> &generators,
                                       size_t max_dim = 256)
    {
        return numqk::dla_dimension(generators, max_dim);
    }

}