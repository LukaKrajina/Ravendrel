#pragma once
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

            params.data()[i] = orig;

            numqk::CTensor d({psi.size()}, false);
            for (size_t k = 0; k < psi.size(); ++k)
                d.data()[k] = T(0.5) * (plus.data()[k] - minus.data()[k]);
            dpsi.push_back(std::move(d));
        }
        return numqk::fubini_study(dpsi, psi);
    }

    inline numqk::Tensor<double> natural_gradient_step(
        const numqk::Tensor<double> &qgt, const numqk::Tensor<double> &grad)
    {
        return numqk::natural_gradient(qgt, grad);
    }

    inline size_t ansatz_dla_dimension(const std::vector<numqk::CTensor> &generators,
                                       size_t max_dim = 256)
    {
        return numqk::dla_dimension(generators, max_dim);
    }

}