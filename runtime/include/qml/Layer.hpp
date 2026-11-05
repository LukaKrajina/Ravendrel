#pragma once
#include "../numqk/Numqk.hpp"
#include "../qhal/IQuantumBackend.hpp"
#include "../../src/QObject.hpp"
#include <memory>
#include <functional>
#include <vector>

namespace qml
{
    template <typename T>
    class ParameterShiftBackward : public numqk::AutogradNode<T>
    {
    private:
        std::shared_ptr<rvdl::QObject> input_data;
        numqk::Tensor<T> input_params;
        numqk::Tensor<T> output_val;
        qhal::IQuantumBackend *backend;
        std::function<T(qhal::IQuantumBackend *, std::shared_ptr<rvdl::QObject>, numqk::Tensor<T> &)> circuit_eval_fn;

    public:
        ParameterShiftBackward(std::shared_ptr<rvdl::QObject> data, numqk::Tensor<T> in, numqk::Tensor<T> out,
                               qhal::IQuantumBackend *be,
                               std::function<T(qhal::IQuantumBackend *, std::shared_ptr<rvdl::QObject>, numqk::Tensor<T> &)> fn)
            : input_data(data), input_params(in), output_val(out), backend(be), circuit_eval_fn(fn) {}

        void apply_backward(const numqk::Tensor<T> &grad_output) override
        {
            if (!input_params.gets_gradients()) return;

            numqk::Tensor<T> local_grad(input_params.get_shape(), false);
            T *l_grad = local_grad.data();
            T *p_data = input_params.data();
            const T *g_out = grad_output.data();
            
            for (size_t i = 0; i < input_params.size(); ++i)
            {
                double original_theta = p_data[i];
                p_data[i] = original_theta + (M_PI / 2.0);
                T eval_plus = circuit_eval_fn(backend, input_data, input_params);
                
                p_data[i] = original_theta - (M_PI / 2.0);
                T eval_minus = circuit_eval_fn(backend, input_data, input_params);
                
                p_data[i] = original_theta;
                l_grad[i] = T(0.5) * (eval_plus - eval_minus) * g_out[0];
            }

            input_params.accumulate_grad(local_grad);
            if (input_params.get_grad_fn())
            {
                input_params.get_grad_fn()->apply_backward(*(input_params.get_grad()));
            }
        }
    };

    template <typename T>
    class QuantumLayer
    {
    private:
        qhal::IQuantumBackend *backend;
        std::function<T(qhal::IQuantumBackend *, std::shared_ptr<rvdl::QObject>, numqk::Tensor<T> &)> circuit_fn;

    public:
        QuantumLayer(qhal::IQuantumBackend *be, std::function<T(qhal::IQuantumBackend *, std::shared_ptr<rvdl::QObject>, numqk::Tensor<T> &)> fn)
            : backend(be), circuit_fn(fn) {}

        numqk::Tensor<T> forward(std::shared_ptr<rvdl::QObject> input_data, numqk::Tensor<T> &params)
        {
            T expectation_val = circuit_fn(backend, input_data, params);
            numqk::Tensor<T> result({1}, params.gets_gradients());
            result.data()[0] = expectation_val;

            if (params.gets_gradients())
            {
                auto backward_node = std::make_shared<ParameterShiftBackward<T>>(input_data, params, result, backend, circuit_fn);
                result.set_grad_fn(backward_node);
            }
            return result;
        }
    };

    // ─────────────────────────────────────────────────────────────
    // 向量化 parameter-shift 反向节点
    // 电路输出 f_θ ∈ R^D（D 个 qubit 的连续期望 ⟨Z⟩），对每个分量 f_j
    // 关于参数 θ_i 求导（RZ 门的 ±π/2 移位规则）：
    //     ∂f_j/∂θ_i = 0.5 · [ f_j(θ_i + π/2) - f_j(θ_i - π/2) ]
    // 上游梯度 g_out ∈ R^D 通过内积回传：
    //     ∂L/∂θ_i = 0.5 · ⟨ Δf(θ_i), g_out ⟩
    // 这是标量 parameter-shift 在向量输出上的自然推广。
    // ─────────────────────────────────────────────────────────────
    template <typename T>
    class VectorParameterShiftBackward : public numqk::AutogradNode<T>
    {
    private:
        std::shared_ptr<rvdl::QObject> input_data;
        numqk::Tensor<T> input_params;
        numqk::Tensor<T> output_val;
        qhal::IQuantumBackend *backend;
        // 电路求值：把 D 维期望写入 out[D]，不做坍缩
        std::function<void(qhal::IQuantumBackend *,
                           std::shared_ptr<rvdl::QObject>,
                           numqk::Tensor<T> &, T *)> circuit_eval_fn;
        size_t dim_;

    public:
        VectorParameterShiftBackward(
            std::shared_ptr<rvdl::QObject> data, numqk::Tensor<T> in,
            numqk::Tensor<T> out, qhal::IQuantumBackend *be,
            std::function<void(qhal::IQuantumBackend *,
                               std::shared_ptr<rvdl::QObject>,
                               numqk::Tensor<T> &, T *)> fn,
            size_t dim)
            : input_data(data), input_params(in), output_val(out),
              backend(be), circuit_eval_fn(fn), dim_(dim) {}

        void apply_backward(const numqk::Tensor<T> &grad_output) override
        {
            if (!input_params.gets_gradients())
                return;

            numqk::Tensor<T> local_grad(input_params.get_shape(), false);
            T *l_grad = local_grad.data();
            T *p_data = input_params.data();
            const T *g_out = grad_output.data(); // 长度 dim_

            std::vector<T> f_plus(dim_), f_minus(dim_);

            for (size_t i = 0; i < input_params.size(); ++i)
            {
                double original = p_data[i];

                p_data[i] = original + (M_PI / 2.0);
                circuit_eval_fn(backend, input_data, input_params, f_plus.data());

                p_data[i] = original - (M_PI / 2.0);
                circuit_eval_fn(backend, input_data, input_params, f_minus.data());

                p_data[i] = original;

                // 内积回传
                T acc = T(0);
                for (size_t j = 0; j < dim_; ++j)
                    acc += T(0.5) * (f_plus[j] - f_minus[j]) * g_out[j];
                l_grad[i] = acc;
            }

            input_params.accumulate_grad(local_grad);
            if (input_params.get_grad_fn())
                input_params.get_grad_fn()->apply_backward(*(input_params.get_grad()));
        }
    };

    // 向量输出的量子层：返回 shape {D} 的连续期望向量，可接入 autograd
    template <typename T>
    class VectorQuantumLayer
    {
    private:
        qhal::IQuantumBackend *backend;
        std::function<void(qhal::IQuantumBackend *,
                           std::shared_ptr<rvdl::QObject>,
                           numqk::Tensor<T> &, T *)> circuit_fn;
        size_t dim_;

    public:
        VectorQuantumLayer(
            qhal::IQuantumBackend *be,
            std::function<void(qhal::IQuantumBackend *,
                               std::shared_ptr<rvdl::QObject>,
                               numqk::Tensor<T> &, T *)> fn,
            size_t dim)
            : backend(be), circuit_fn(fn), dim_(dim) {}

        numqk::Tensor<T> forward(std::shared_ptr<rvdl::QObject> input_data,
                                 numqk::Tensor<T> &params)
        {
            numqk::Tensor<T> result({dim_}, params.gets_gradients());
            circuit_fn(backend, input_data, params, result.data());

            if (params.gets_gradients())
            {
                auto node = std::make_shared<VectorParameterShiftBackward<T>>(
                    input_data, params, result, backend, circuit_fn, dim_);
                result.set_grad_fn(node);
            }
            return result;
        }
    };
}