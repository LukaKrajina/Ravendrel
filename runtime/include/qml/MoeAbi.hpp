#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>

#include "numqk/moe.hpp"

#ifndef RVDL_HOST_EXPORT
#define RVDL_HOST_EXPORT extern "C" RVDL_RT_API
#endif

extern "C"
{
    RVDL_HOST_EXPORT double rvdl_moe_groups(double pb);
    RVDL_HOST_EXPORT double rvdl_moe_experts_per(double pb);
    RVDL_HOST_EXPORT double rvdl_moe_virtual_experts(double pb);
    RVDL_HOST_EXPORT double rvdl_moe_savings(double pb);

    RVDL_HOST_EXPORT double rvdl_curvature_gate(double g);
    RVDL_HOST_EXPORT double rvdl_dual_fuse(double expert_a, double expert_b,
                                           double g, double theta);






    RVDL_HOST_EXPORT double rvdl_dual_moe_forward(const double *activations,
                                                  const double *group_gates,
                                                  const double *intra_gates,
                                                  std::int32_t G, std::int32_t K,
                                                  double g, double theta);
}

#if defined(RVDL_RT_BUILD)

double rvdl_moe_groups(double pb)
{
    return static_cast<double>(numqk::moe::auto_moe_topology(pb).groups);
}

double rvdl_moe_experts_per(double pb)
{
    return static_cast<double>(numqk::moe::auto_moe_topology(pb).experts_per);
}

double rvdl_moe_virtual_experts(double pb)
{
    return static_cast<double>(numqk::moe::auto_moe_topology(pb).virtual_experts);
}

double rvdl_moe_savings(double pb)
{
    return numqk::moe::auto_moe_topology(pb).savings;
}

double rvdl_curvature_gate(double g)
{
    return numqk::moe::curvature_gate(g);
}

double rvdl_dual_fuse(double expert_a, double expert_b, double g, double theta)
{
    return numqk::moe::dual_fuse(expert_a, expert_b, g, theta);
}

double rvdl_dual_moe_forward(const double *activations, const double *group_gates,
                             const double *intra_gates, std::int32_t G, std::int32_t K,
                             double g, double theta)
{
    if (!activations || !group_gates || !intra_gates || G <= 0 || K <= 0)
        return 0.0;
    numqk::moe::DualRoute r = numqk::moe::dual_moe_forward(
        activations, static_cast<size_t>(G), static_cast<size_t>(K),
        group_gates, intra_gates, g, theta);
    return r.value;
}

#endif