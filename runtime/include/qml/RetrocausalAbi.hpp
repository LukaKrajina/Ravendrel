#pragma once
#include "../qhal/Export.hpp"
#include "../spacetime/RetrocausalCapacity.hpp"
#include <cstdint>

#ifndef RVDL_HOST_EXPORT
#define RVDL_HOST_EXPORT extern "C" RVDL_RT_API
#endif

namespace qml
{
    inline rvdl::spacetime::QuantumChannel make_retrocausal_channel(int kind, double p)
    {
        if (kind < 0 || kind > 5)
            return rvdl::spacetime::QuantumChannel::depolarizing(p);
        return rvdl::spacetime::QuantumChannel::make(kind, p);
    }
}

extern "C"
{
    RVDL_HOST_EXPORT double rvdl_retrocausal_imax(int32_t kind, double p);
    RVDL_HOST_EXPORT double rvdl_retrocausal_idoe(int32_t kind, double p);
    RVDL_HOST_EXPORT double rvdl_retrocausal_q_capacity(int32_t kind, double p);
    RVDL_HOST_EXPORT double rvdl_retrocausal_c_capacity(int32_t kind, double p);
    RVDL_HOST_EXPORT double rvdl_retrocausal_q_one_shot(int32_t kind, double p, double eps);
    RVDL_HOST_EXPORT double rvdl_retrocausal_gain(int32_t kind, double p);
    RVDL_HOST_EXPORT double rvdl_retrocausal_deformed(int32_t kind, double p, double q);
}

#if defined(RVDL_RT_BUILD)
double rvdl_retrocausal_imax(int32_t kind, double p)
{
    const auto N = qml::make_retrocausal_channel(kind, p);
    return rvdl::spacetime::retrocausal_info(N).imax;
}

double rvdl_retrocausal_idoe(int32_t kind, double p)
{
    const auto N = qml::make_retrocausal_channel(kind, p);
    return rvdl::spacetime::retrocausal_info(N).idoe;
}

double rvdl_retrocausal_q_capacity(int32_t kind, double p)
{
    const auto N = qml::make_retrocausal_channel(kind, p);
    return rvdl::spacetime::retrocausal_capacity(N).q_asymptotic;
}

double rvdl_retrocausal_c_capacity(int32_t kind, double p)
{
    const auto N = qml::make_retrocausal_channel(kind, p);
    return rvdl::spacetime::retrocausal_capacity(N).c_asymptotic;
}

double rvdl_retrocausal_q_one_shot(int32_t kind, double p, double eps)
{
    const auto N = qml::make_retrocausal_channel(kind, p);
    return rvdl::spacetime::retrocausal_capacity(N, eps).q_one_shot;
}

double rvdl_retrocausal_gain(int32_t kind, double p)
{
    const auto N = qml::make_retrocausal_channel(kind, p);
    return rvdl::spacetime::retrocausal_capacity(N).gain;
}

double rvdl_retrocausal_deformed(int32_t kind, double p, double q)
{
    const auto N = qml::make_retrocausal_channel(kind, p);
    return rvdl::spacetime::q_retrocausal_capacity(N, q);
}
#endif