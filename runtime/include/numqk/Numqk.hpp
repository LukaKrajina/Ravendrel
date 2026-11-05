#pragma once
#include <thread>
#include <vector>
#include <array>
#include <numeric>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <cmath>
#include <complex>
#include <unordered_set>
#include <functional>
#include <algorithm>
#include <utility>
#include "nqcmath.hpp"
#include "expermath.hpp"

namespace numqk {

    template <typename T>
    class Tensor;

    template <typename T>
    class AutogradNode {
    protected:
        std::vector<std::shared_ptr<AutogradNode<T>>> next_edges;
    public:
        virtual ~AutogradNode() = default;
        
        const std::vector<std::shared_ptr<AutogradNode<T>>>& get_next_edges() const {
            return next_edges;
        }

        virtual void apply_backward(const Tensor<T>& grad_output) = 0;
    };

    template <typename T>
    class Tensor {
    private:
        std::shared_ptr<std::vector<T>> data_buffer;
        std::vector<size_t> shape;
        std::vector<size_t> strides;

        bool requires_grad;
        std::shared_ptr<Tensor<T>> grad; 
        std::shared_ptr<AutogradNode<T>> grad_fn;

        void compute_strides() {
            strides.resize(shape.size());
            size_t stride = 1;
            for (int i = shape.size() - 1; i >= 0; --i) {
                strides[i] = stride;
                stride *= shape[i];
            }
        }

        
    private:
        Tensor(std::vector<size_t> target_shape, bool requires_grad, bool init_grad_buffer) 
            : shape(target_shape), requires_grad(requires_grad), grad_fn(nullptr) {
            size_t total_size = std::accumulate(shape.begin(), shape.end(), 1, std::multiplies<size_t>());
            data_buffer = std::make_shared<std::vector<T>>(total_size, T(0));
            compute_strides();
        }

    public:
        Tensor(std::vector<size_t> target_shape, bool requires_grad = false) 
            : shape(target_shape), requires_grad(requires_grad), grad_fn(nullptr) {
            size_t total_size = std::accumulate(shape.begin(), shape.end(), 1, std::multiplies<size_t>());
            data_buffer = std::make_shared<std::vector<T>>(total_size, T(0));
            compute_strides();
            if (requires_grad) {
                grad = std::shared_ptr<Tensor<T>>(new Tensor<T>(shape, false, false));
            }
        }

        T* data() { return data_buffer->data(); }
        const T* data() const { return data_buffer->data(); }
        size_t size() const { return data_buffer->size(); }
        const std::vector<size_t>& get_shape() const { return shape; }
        
        bool gets_gradients() const { return requires_grad; }
        std::shared_ptr<Tensor<T>> get_grad() const { return grad; }
        std::shared_ptr<AutogradNode<T>> get_grad_fn() const { return grad_fn; }
        
        void set_grad_fn(std::shared_ptr<AutogradNode<T>> fn) { grad_fn = fn; }

        void accumulate_grad(const Tensor<T>& downstream_grad) {
            if (!requires_grad) return;
            if (this->shape != downstream_grad.get_shape()) {
                throw std::invalid_argument("Gradient shape mismatch during accumulation.");
            }
            T* current_grad = this->grad->data();
            const T* incoming_grad = downstream_grad.data();
            for (size_t i = 0; i < this->size(); ++i) {
                current_grad[i] += incoming_grad[i];
            }
        }

        Tensor<T> transpose() const {
            if (shape.size() != 2) throw std::invalid_argument("Transpose only supports 2D tensors currently.");
            Tensor<T> result({shape[1], shape[0]}, false);
            const T* src = this->data();
            T* dst = result.data();
            for (size_t i = 0; i < shape[0]; ++i) {
                for (size_t j = 0; j < shape[1]; ++j) {
                    dst[j * shape[0] + i] = src[i * shape[1] + j];
                }
            }
            return result;
        }
        
        Tensor<T> operator*(const Tensor<T>& other) const {
            Tensor<T> result(this->shape, false);
            const T* a = this->data();
            const T* b = other.data();
            T* c = result.data();
            for(size_t i = 0; i < this->size(); ++i) {
                c[i] = a[i] * b[i];
            }
            return result;
        }

        
        Tensor<T> matmul(const Tensor<T>& other) const;
        Tensor<T> sigmoid();

        // 逐元素 / 归约运算（带自动微分）
        Tensor<T> add(const Tensor<T>& other);
        Tensor<T> sub(const Tensor<T>& other);
        Tensor<T> mul(const Tensor<T>& other);
        Tensor<T> div(const Tensor<T>& other);
        Tensor<T> relu();
        Tensor<T> tanh();
        Tensor<T> exp();
        Tensor<T> sum();
        Tensor<T> mean();

        
        void backward() {
            if (!requires_grad) throw std::runtime_error("Cannot call backward on a tensor that does not require gradients.");

            // 标量损失：∂L/∂self = 1。非标量输出按元素初始化为 1。
            for (size_t i = 0; i < this->size(); ++i) {
                this->grad->data()[i] = T(1.0);
            }

            // 递归反传：每个 AutogradNode 负责 (1) 计算局部梯度、(2) accumulate_grad
            // 到输入、(3) 若输入仍需求梯度则递归触发其 backward。叶子节点（无 grad_fn）
            // 在此终止。与 MatmulBackward / SigmoidBackward 的既有约定一致。
            if (this->grad_fn) {
                this->grad_fn->apply_backward(*(this->grad));
            }
        }
    };

    template <typename T>
    class SigmoidBackward : public AutogradNode<T> {
    private:
        Tensor<T> input;
        Tensor<T> output;
    public:
        SigmoidBackward(Tensor<T> in, Tensor<T> out) : input(in), output(out) {}

        void apply_backward(const Tensor<T>& grad_output) override {
            if (!input.gets_gradients()) return;
            Tensor<T> local_grad(output.get_shape(), false);
            const T* y = output.data();
            const T* g_out = grad_output.data();
            T* l_grad = local_grad.data();

            for (size_t i = 0; i < output.size(); ++i) {
                l_grad[i] = g_out[i] * y[i] * (T(1.0) - y[i]);
            }

            input.accumulate_grad(local_grad);

            if (input.get_grad_fn()) {
                input.get_grad_fn()->apply_backward(*(input.get_grad()));
            }
        }
    };

    template <typename T>
    class MatmulBackward : public AutogradNode<T> {
    private:
        Tensor<T> A;
        Tensor<T> B;
    public:
        MatmulBackward(Tensor<T> a, Tensor<T> b) : A(a), B(b) {}

        void apply_backward(const Tensor<T>& grad_output) override {

            if (A.gets_gradients()) {
                Tensor<T> b_t = B.transpose();
                Tensor<T> grad_a = grad_output.matmul(b_t); 
                A.accumulate_grad(grad_a);

                if (A.get_grad_fn()) {
                    A.get_grad_fn()->apply_backward(*(A.get_grad()));
                }
            }

            if (B.gets_gradients()) {
                Tensor<T> a_t = A.transpose();
                Tensor<T> grad_b = a_t.matmul(grad_output);
                B.accumulate_grad(grad_b);

                if (B.get_grad_fn()) {
                    B.get_grad_fn()->apply_backward(*(B.get_grad()));
                }
            }
        }
    };

    template <typename T>
    Tensor<T> Tensor<T>::sigmoid() {
        bool track_grad = this->requires_grad;
        Tensor<T> result(this->shape, track_grad);
        
        for (size_t i = 0; i < data_buffer->size(); ++i) {
            result.data()[i] = T(1.0) / (T(1.0) + std::exp(-(*data_buffer)[i]));
        }

        if (track_grad) {
            auto backward_node = std::make_shared<SigmoidBackward<T>>(*this, result);
            result.set_grad_fn(backward_node);
        }
        return result;
    }

    template <typename T>
    Tensor<T> Tensor<T>::matmul(const Tensor<T>& other) const {
        size_t M = this->shape[0];
        size_t K = this->shape[1];
        size_t N = other.shape[1];

        bool track_grad = this->requires_grad || other.gets_gradients();
        Tensor<T> result({M, N}, track_grad);

        const T* a_ptr = this->data();
        const T* b_ptr = other.data();
        T* c_ptr = result.data();

        for (size_t i = 0; i < M; ++i) {
            for (size_t k = 0; k < K; ++k) {
                T a_ik = a_ptr[i * K + k];
                for (size_t j = 0; j < N; ++j) {
                    c_ptr[i * N + j] += a_ik * b_ptr[k * N + j];
                }
            }
        }

        if (track_grad) {
            auto backward_node = std::make_shared<MatmulBackward<T>>(*this, other);
            result.set_grad_fn(backward_node);
        }
        return result;
    }

    template <typename T>
    class ElementwiseBinaryBackward : public AutogradNode<T> {
    private:
        Tensor<T> a;
        Tensor<T> b;
        std::function<T(T, T, T)> grad_a_fn;
        std::function<T(T, T, T)> grad_b_fn;
    public:
        ElementwiseBinaryBackward(Tensor<T> a_, Tensor<T> b_,
                                  std::function<T(T, T, T)> ga,
                                  std::function<T(T, T, T)> gb)
            : a(a_), b(b_), grad_a_fn(std::move(ga)), grad_b_fn(std::move(gb)) {}

        void apply_backward(const Tensor<T>& grad_output) override {
            const T* g = grad_output.data();
            const T* av = a.data();
            const T* bv = b.data();
            const size_t n = grad_output.size();

            if (a.gets_gradients()) {
                Tensor<T> local(a.get_shape(), false);
                T* l = local.data();
                for (size_t i = 0; i < n; ++i) l[i] = grad_a_fn(g[i], av[i], bv[i]);
                a.accumulate_grad(local);
                if (a.get_grad_fn()) a.get_grad_fn()->apply_backward(*(a.get_grad()));
            }
            if (b.gets_gradients()) {
                Tensor<T> local(b.get_shape(), false);
                T* l = local.data();
                for (size_t i = 0; i < n; ++i) l[i] = grad_b_fn(g[i], av[i], bv[i]);
                b.accumulate_grad(local);
                if (b.get_grad_fn()) b.get_grad_fn()->apply_backward(*(b.get_grad()));
            }
        }
    };

    template <typename T>
    class UnaryBackward : public AutogradNode<T> {
    private:
        Tensor<T> input;
        Tensor<T> output;
        std::function<T(T, T)> grad_fn;
    public:
        UnaryBackward(Tensor<T> in, Tensor<T> out, std::function<T(T, T)> fn)
            : input(in), output(out), grad_fn(std::move(fn)) {}

        void apply_backward(const Tensor<T>& grad_output) override {
            if (!input.gets_gradients()) return;
            const T* g = grad_output.data();
            const T* y = output.data();
            const size_t n = grad_output.size();
            Tensor<T> local(input.get_shape(), false);
            T* l = local.data();
            for (size_t i = 0; i < n; ++i) l[i] = grad_fn(g[i], y[i]);
            input.accumulate_grad(local);
            if (input.get_grad_fn()) input.get_grad_fn()->apply_backward(*(input.get_grad()));
        }
    };

    template <typename T>
    class ReduceBackward : public AutogradNode<T> {
    private:
        Tensor<T> input;
        T scale; // sum=1，mean=1/n
    public:
        ReduceBackward(Tensor<T> in, T s) : input(in), scale(s) {}

        void apply_backward(const Tensor<T>& grad_output) override {
            if (!input.gets_gradients()) return;
            T g = grad_output.data()[0];
            Tensor<T> local(input.get_shape(), false);
            T* l = local.data();
            for (size_t i = 0; i < input.size(); ++i) l[i] = g * scale;
            input.accumulate_grad(local);
            if (input.get_grad_fn()) input.get_grad_fn()->apply_backward(*(input.get_grad()));
        }
    };

    template <typename T>
    Tensor<T> Tensor<T>::add(const Tensor<T>& other) {
        Tensor<T> result(this->shape, this->requires_grad || other.gets_gradients());
        const T* a = this->data();
        const T* b = other.data();
        T* c = result.data();
        for (size_t i = 0; i < this->size(); ++i) c[i] = a[i] + b[i];
        if (result.gets_gradients()) {
            auto node = std::make_shared<ElementwiseBinaryBackward<T>>(
                *this, other,
                [](T g, T, T) { return g; },
                [](T g, T, T) { return g; });
            result.set_grad_fn(node);
        }
        return result;
    }

    template <typename T>
    Tensor<T> Tensor<T>::sub(const Tensor<T>& other) {
        Tensor<T> result(this->shape, this->requires_grad || other.gets_gradients());
        const T* a = this->data();
        const T* b = other.data();
        T* c = result.data();
        for (size_t i = 0; i < this->size(); ++i) c[i] = a[i] - b[i];
        if (result.gets_gradients()) {
            auto node = std::make_shared<ElementwiseBinaryBackward<T>>(
                *this, other,
                [](T g, T, T) { return g; },
                [](T g, T, T) { return -g; });
            result.set_grad_fn(node);
        }
        return result;
    }

    template <typename T>
    Tensor<T> Tensor<T>::mul(const Tensor<T>& other) {
        Tensor<T> result(this->shape, this->requires_grad || other.gets_gradients());
        const T* a = this->data();
        const T* b = other.data();
        T* c = result.data();
        for (size_t i = 0; i < this->size(); ++i) c[i] = a[i] * b[i];
        if (result.gets_gradients()) {
            auto node = std::make_shared<ElementwiseBinaryBackward<T>>(
                *this, other,
                [](T g, T, T bv) { return g * bv; },
                [](T g, T av, T) { return g * av; });
            result.set_grad_fn(node);
        }
        return result;
    }

    template <typename T>
    Tensor<T> Tensor<T>::div(const Tensor<T>& other) {
        Tensor<T> result(this->shape, this->requires_grad || other.gets_gradients());
        const T* a = this->data();
        const T* b = other.data();
        T* c = result.data();
        for (size_t i = 0; i < this->size(); ++i) c[i] = a[i] / b[i];
        if (result.gets_gradients()) {
            auto node = std::make_shared<ElementwiseBinaryBackward<T>>(
                *this, other,
                [](T g, T, T bv) { return g / bv; },
                [](T g, T av, T bv) { return -g * av / (bv * bv); });
            result.set_grad_fn(node);
        }
        return result;
    }

    template <typename T>
    Tensor<T> Tensor<T>::relu() {
        Tensor<T> result(this->shape, this->requires_grad);
        const T* a = this->data();
        T* c = result.data();
        for (size_t i = 0; i < this->size(); ++i) c[i] = a[i] > T(0) ? a[i] : T(0);
        if (result.gets_gradients()) {
            auto node = std::make_shared<UnaryBackward<T>>(
                *this, result,
                [](T g, T y) { return y > T(0) ? g : T(0); });
            result.set_grad_fn(node);
        }
        return result;
    }

    template <typename T>
    Tensor<T> Tensor<T>::tanh() {
        Tensor<T> result(this->shape, this->requires_grad);
        const T* a = this->data();
        T* c = result.data();
        for (size_t i = 0; i < this->size(); ++i) c[i] = std::tanh(a[i]);
        if (result.gets_gradients()) {
            auto node = std::make_shared<UnaryBackward<T>>(
                *this, result,
                [](T g, T y) { return g * (T(1) - y * y); });
            result.set_grad_fn(node);
        }
        return result;
    }

    template <typename T>
    Tensor<T> Tensor<T>::exp() {
        Tensor<T> result(this->shape, this->requires_grad);
        const T* a = this->data();
        T* c = result.data();
        for (size_t i = 0; i < this->size(); ++i) c[i] = std::exp(a[i]);
        if (result.gets_gradients()) {
            auto node = std::make_shared<UnaryBackward<T>>(
                *this, result,
                [](T g, T y) { return g * y; });
            result.set_grad_fn(node);
        }
        return result;
    }

    template <typename T>
    Tensor<T> Tensor<T>::sum() {
        Tensor<T> result({1}, this->requires_grad);
        const T* a = this->data();
        T s = T(0);
        for (size_t i = 0; i < this->size(); ++i) s += a[i];
        result.data()[0] = s;
        if (result.gets_gradients()) {
            auto node = std::make_shared<ReduceBackward<T>>(*this, T(1));
            result.set_grad_fn(node);
        }
        return result;
    }

    template <typename T>
    Tensor<T> Tensor<T>::mean() {
        Tensor<T> result({1}, this->requires_grad);
        const T* a = this->data();
        T s = T(0);
        for (size_t i = 0; i < this->size(); ++i) s += a[i];
        result.data()[0] = s / T(this->size());
        if (result.gets_gradients()) {
            auto node = std::make_shared<ReduceBackward<T>>(*this, T(1) / T(this->size()));
            result.set_grad_fn(node);
        }
        return result;
    }

    // 原计划定在该处,后面为独立性另起它处。
    //
    // nqcmath：普通数学库（Matrix/Vector）+ 高并发并行层（Parallel：常驻
    //           线程池、动态块调度、缓存友好的分块 matmul）。
    // expermath：自研量子数学库（偏迹/熵/保真度/Fisher 信息、QFT 与
    //             Trotter 电路生成）。

}