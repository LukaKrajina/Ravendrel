#pragma once

#include "../numqk/SoftLogic.hpp"
#include "../qbNs/SpikeBeliefPropagation.hpp"
#include "../qhal/PolymerSampler.hpp"
#include "../qhal/Qms.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>

#ifndef RVDL_HOST_EXPORT
#define RVDL_HOST_EXPORT extern "C" RVDL_RT_API
#endif

extern "C"
{
    RVDL_HOST_EXPORT double rvdl_surrogate(double x, double theta, double gamma);
    RVDL_HOST_EXPORT double rvdl_tanh_quantize(double w, double alpha, int bits);
    RVDL_HOST_EXPORT double rvdl_lif_step(double v, double current, double tau, double theta);
    RVDL_HOST_EXPORT double rvdl_mellowmax2(double a, double b, double omega);
    RVDL_HOST_EXPORT double rvdl_logsumexp2(double a, double b, double beta);
    RVDL_HOST_EXPORT double rvdl_boltzmann2(double a, double b, double beta);
    RVDL_HOST_EXPORT double rvdl_tnorm_luk(double a, double b);
    RVDL_HOST_EXPORT double rvdl_tnorm_prod(double a, double b);
    RVDL_HOST_EXPORT double rvdl_tnorm_godel(double a, double b);
    RVDL_HOST_EXPORT double rvdl_polymer_weight(double beta, double h, double len);
    RVDL_HOST_EXPORT double rvdl_polymer_mix_bound(double n, double eps);
    RVDL_HOST_EXPORT double rvdl_qms_gap(std::int32_t model, double p, double q);
    RVDL_HOST_EXPORT double rvdl_mix_bound(double gap, double n, double eps);
    RVDL_HOST_EXPORT double rvdl_qms_conc(double gap, double eps);
}

#if defined(RVDL_RT_BUILD)

double rvdl_surrogate(double x, double theta, double gamma)
{
    return numqk::surrogate_gradient(x, theta, gamma);
}

double rvdl_tanh_quantize(double w, double alpha, int bits)
{
    return numqk::tanh_quantize_scalar(w, alpha, bits);
}

double rvdl_lif_step(double v, double current, double tau, double theta)
{
    return qbns::lif_step(v, current, tau, theta);
}

double rvdl_mellowmax2(double a, double b, double omega)
{
    return numqk::mellowmax2(a, b, omega);
}

double rvdl_logsumexp2(double a, double b, double beta)
{
    return numqk::logsumexp2(a, b, beta);
}

double rvdl_boltzmann2(double a, double b, double beta)
{
    return numqk::boltzmann2(a, b, beta);
}

double rvdl_tnorm_luk(double a, double b) { return numqk::lukasiewicz_meet(a, b); }
double rvdl_tnorm_prod(double a, double b) { return numqk::product_meet(a, b); }
double rvdl_tnorm_godel(double a, double b) { return numqk::godel_meet(a, b); }

double rvdl_polymer_weight(double beta, double h, double len)
{
    return qhal::PolymerSampler::ferromagnetic_weight(beta, h, static_cast<size_t>(len));
}

double rvdl_polymer_mix_bound(double n, double eps)
{
    return n * std::log(n / eps);
}

double rvdl_qms_gap(std::int32_t model, double p, double q)
{
    return qhal::qms::spectral_gap(model, p, q);
}

double rvdl_mix_bound(double gap, double n, double eps)
{
    return qhal::qms::mixing_bound(gap, n, eps);
}

double rvdl_qms_conc(double gap, double eps)
{
    return qhal::qms::variance_contraction_time(gap, eps);
}

#endif