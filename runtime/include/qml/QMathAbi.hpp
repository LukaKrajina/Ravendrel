#pragma once

#include "QStateAbi.hpp"
#include "../numqk/nqcmath.hpp"
#include "../numqk/expermath.hpp"

#include <complex>
#include <cstdint>
#include <cmath>
#include <vector>

#ifndef RVDL_HOST_EXPORT
#define RVDL_HOST_EXPORT extern "C" RVDL_RT_API
#endif

namespace qml
{
    inline numqk::nqcmath::MatrixX<std::complex<double>> rho_matrix_of(
        qhal::IQuantumBackend *be, const std::vector<size_t> &ids)
    {
        numqk::CTensor rho = reduced_density_of(be, ids);
        if (rho.size() == 0 || rho.get_shape().size() != 2)
            return numqk::nqcmath::MatrixX<std::complex<double>>();
        const size_t d = rho.get_shape()[0];
        if (d == 0 || rho.get_shape()[1] != d)
            return numqk::nqcmath::MatrixX<std::complex<double>>();
        numqk::nqcmath::MatrixX<std::complex<double>> m(d, d);
        for (size_t i = 0; i < d * d; ++i)
            m.data()[i] = rho.data()[i];
        return m;
    }
}

extern "C"
{

    RVDL_HOST_EXPORT double rvdl_qstate_purity(QObject *obj);
    RVDL_HOST_EXPORT double rvdl_qstate_linear_entropy(QObject *obj);
    RVDL_HOST_EXPORT double rvdl_qstate_renyi_entropy(QObject *obj, double alpha);


    RVDL_HOST_EXPORT double rvdl_math_dot(const double *a, const double *b, std::int32_t n);
    RVDL_HOST_EXPORT double rvdl_math_norm(const double *a, std::int32_t n);
}

#if defined(RVDL_RT_BUILD)

double rvdl_qstate_purity(QObject *obj)
{
    if (!obj || !global_qm)
        return -1.0;
    const numqk::nqcmath::MatrixX<std::complex<double>> m =
        qml::rho_matrix_of(global_qm, obj->hardware_ids);
    if (m.rows() == 0)
        return -1.0;
    return numqk::expermath::purity(m);
}

double rvdl_qstate_linear_entropy(QObject *obj)
{
    if (!obj || !global_qm)
        return -1.0;
    const numqk::nqcmath::MatrixX<std::complex<double>> m =
        qml::rho_matrix_of(global_qm, obj->hardware_ids);
    if (m.rows() == 0)
        return -1.0;
    return numqk::expermath::linear_entropy(m);
}

double rvdl_qstate_renyi_entropy(QObject *obj, double alpha)
{
    if (!obj || !global_qm)
        return -1.0;
    const numqk::nqcmath::MatrixX<std::complex<double>> m =
        qml::rho_matrix_of(global_qm, obj->hardware_ids);
    if (m.rows() == 0)
        return -1.0;
    return numqk::expermath::renyi_entropy(m, alpha);
}

double rvdl_math_dot(const double *a, const double *b, std::int32_t n)
{
    if (!a || !b || n <= 0)
        return 0.0;
    return numqk::nqcmath::Parallel::parallel_reduce<double>(
        static_cast<size_t>(n), 0.0,
        [&](size_t i) { return a[i] * b[i]; },
        [](double x, double y) { return x + y; });
}

double rvdl_math_norm(const double *a, std::int32_t n)
{
    if (!a || n <= 0)
        return 0.0;
    const double sq = numqk::nqcmath::Parallel::parallel_reduce<double>(
        static_cast<size_t>(n), 0.0,
        [&](size_t i) { return a[i] * a[i]; },
        [](double x, double y) { return x + y; });
    return std::sqrt(sq);
}

#endif