#pragma once
#include "Numqk.hpp"
#include <algorithm>
#include <cmath>

namespace numqk {
  inline double logsumexp2(double a, double b, double beta = 1.0) {
    double m = std::max(beta * a, beta * b);
    return (m + std::log(std::exp(beta * a - m) + std::exp(beta * b - m))) / beta;
  }

  inline double mellowmax2(double a, double b, double omega = 1.0) {
    return logsumexp2(a, b, omega) - std::log(2.0) / omega;
  }

  inline double softmax_weight_a(double a, double b, double beta = 1.0) {
    double m = std::max(beta * a, beta * b);
    double ea = std::exp(beta * a - m);
    double eb = std::exp(beta * b - m);
    return ea / (ea + eb);
  }

  inline double boltzmann2(double a, double b, double beta = 1.0) {
    double wa = softmax_weight_a(a, b, beta);
    return wa * a + (1.0 - wa) * b;
  }

  inline double lukasiewicz_meet(double a, double b) {
    return std::max(0.0, a + b - 1.0);
  }
  inline double product_meet(double a, double b) { return a * b; }
  inline double godel_meet(double a, double b) { return std::min(a, b); }

  inline double surrogate_gradient(double x, double theta, double gamma) {
    double g2 = gamma * gamma;
    return std::max(0.0, (gamma - std::abs(x - theta)) / g2);
  }

  inline double tanh_quantize_scalar(double w, double alpha, int bits) {
    double Qp = (1 << (bits - 1)) - 1;
    double v = std::tanh(w) / alpha;
    v = std::max(-1.0, std::min(1.0, v));
    return (alpha / Qp) * std::round(Qp * v);
  }

  template <typename T> Tensor<T> softmax(const Tensor<T> &x, T beta = T(1.0)) {
    T m = *std::max_element(x.data(), x.data() + x.size());
    Tensor<T> w(x.get_shape());
    T s = T(0);
    for (size_t i = 0; i < x.size(); ++i) {
      w.data()[i] = std::exp(beta * (x.data()[i] - m));
      s += w.data()[i];
    }
    for (size_t i = 0; i < x.size(); ++i)
      w.data()[i] /= s;
    return w;
  }

  template <typename T> Tensor<T> logsumexp(const Tensor<T> &x, T beta = T(1.0)) {
    T m = *std::max_element(x.data(), x.data() + x.size());
    T s = T(0);
    for (size_t i = 0; i < x.size(); ++i)
      s += std::exp(beta * (x.data()[i] - m));
    Tensor<T> out({1});
    out.data()[0] = m + std::log(s) / beta;
    return out;
  }

  template <typename T>
  Tensor<T> mellowmax(const Tensor<T> &x, T omega = T(1.0)) {
    Tensor<T> out({1});
    out.data()[0] = logsumexp(x, omega).data()[0] - std::log(T(x.size())) / omega;
    return out;
  }

  template <typename T>
  Tensor<T> boltzmann_average(const Tensor<T> &x, T beta = T(1.0)) {
    Tensor<T> w = softmax(x, beta);
    T acc = T(0);
    for (size_t i = 0; i < x.size(); ++i)
      acc += w.data()[i] * x.data()[i];
    Tensor<T> out({1});
    out.data()[0] = acc;
    return out;
  }

  template <typename T>
  Tensor<T> surrogate_gradient_tensor(const Tensor<T> &x, T theta = T(0.0),
                                      T gamma = T(1.0)) {
    Tensor<T> out(x.get_shape());
    T g2 = gamma * gamma;
    for (size_t i = 0; i < x.size(); ++i)
      out.data()[i] =
          std::max(T(0), (gamma - std::abs(x.data()[i] - theta)) / g2);
    return out;
  }
}