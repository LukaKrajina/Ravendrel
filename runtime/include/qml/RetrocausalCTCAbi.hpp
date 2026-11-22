#pragma once
#include "../qhal/Export.hpp"
#include "../spacetime/RetrocausalCTC.hpp"
#include <cstdint>
#include <vector>

#ifndef RVDL_HOST_EXPORT
#define RVDL_HOST_EXPORT extern "C" RVDL_RT_API
#endif

namespace qml
{
    inline rvdl::spacetime::KaluzaKleinND make_uniform_kk(int n, double theta)
    {
        if (n < 1)
            n = 1;
        return rvdl::spacetime::KaluzaKleinND(
            std::vector<double>(static_cast<size_t>(n), 1.0),
            std::vector<double>(static_cast<size_t>(n), theta));
    }
}

extern "C"
{
    RVDL_HOST_EXPORT double rvdl_retrocausal_ctc_q_capacity(int32_t n, double theta, double mu2, double lambda);
    RVDL_HOST_EXPORT double rvdl_retrocausal_ctc_c_capacity(int32_t n, double theta, double mu2, double lambda);
    RVDL_HOST_EXPORT double rvdl_retrocausal_ctc_gain(int32_t n, double theta, double mu2, double lambda);
    RVDL_HOST_EXPORT double rvdl_retrocausal_ctc_dephasing(double theta);
}

#if defined(RVDL_RT_BUILD)
double rvdl_retrocausal_ctc_q_capacity(int32_t n, double theta, double mu2, double lambda)
{
    const auto kk = qml::make_uniform_kk(n, theta);
    return rvdl::spacetime::retrocausal_capacity(
               rvdl::spacetime::tachyon_ctc_channel(kk, mu2, lambda))
        .q_asymptotic;
}

double rvdl_retrocausal_ctc_c_capacity(int32_t n, double theta, double mu2, double lambda)
{
    const auto kk = qml::make_uniform_kk(n, theta);
    return rvdl::spacetime::retrocausal_capacity(
               rvdl::spacetime::tachyon_ctc_channel(kk, mu2, lambda))
        .c_asymptotic;
}

double rvdl_retrocausal_ctc_gain(int32_t n, double theta, double mu2, double lambda)
{
    const auto kk = qml::make_uniform_kk(n, theta);
    return rvdl::spacetime::retrocausal_capacity(
               rvdl::spacetime::tachyon_ctc_channel(kk, mu2, lambda))
        .gain;
}

double rvdl_retrocausal_ctc_dephasing(double theta)
{
    const auto kk = qml::make_uniform_kk(1, theta);
    return rvdl::spacetime::tachyon_ctc_noise(kk, 1.0, 1.0).dephasing;
}
#endif