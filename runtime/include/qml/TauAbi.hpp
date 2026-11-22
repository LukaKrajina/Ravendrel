#pragma once

#include <cstdint>

#include "qml/TauScale.hpp"

#ifndef RVDL_HOST_EXPORT
#define RVDL_HOST_EXPORT extern "C" RVDL_RT_API
#endif

extern "C"
{
    RVDL_HOST_EXPORT double rvdl_tau_alpha(double preset);
    RVDL_HOST_EXPORT double rvdl_tau_generational(double tau, double alpha);

    RVDL_HOST_EXPORT double rvdl_tau_fold_depth(double serial_depth, double tiers);
    RVDL_HOST_EXPORT double rvdl_tau_gear_ratio(double e_cross, double e_intra);
    RVDL_HOST_EXPORT double rvdl_tau_route_cost(double depth, double gear,
                                                double tau_gate, double tau_comm);

    RVDL_HOST_EXPORT double rvdl_tau_route_price(double value, double cost, double lambda);
}

#if defined(RVDL_RT_BUILD)

double rvdl_tau_alpha(double preset)
{
    return numqk::tau::alpha_preset(static_cast<std::int32_t>(preset));
}

double rvdl_tau_generational(double tau, double alpha)
{
    return numqk::tau::generational(tau, alpha);
}

double rvdl_tau_fold_depth(double serial_depth, double tiers)
{
    return numqk::tau::fold_depth(serial_depth, tiers);
}

double rvdl_tau_gear_ratio(double e_cross, double e_intra)
{
    return numqk::tau::gear_ratio(e_cross, e_intra);
}

double rvdl_tau_route_cost(double depth, double gear, double tau_gate, double tau_comm)
{
    return numqk::tau::route_cost(depth, gear, tau_gate, tau_comm);
}

double rvdl_tau_route_price(double value, double cost, double lambda)
{
    return numqk::tau::route_price(value, cost, lambda);
}

#endif