#pragma once

#include "Inference.hpp"
#include "QAttentionAbi.hpp"
#include <cmath>

#ifndef RVDL_HOST_EXPORT
#define RVDL_HOST_EXPORT extern "C" RVDL_RT_API
#endif

extern "C"
{
    RVDL_HOST_EXPORT double rvdl_geodesic_distance(QObject *a, QObject *b);
    RVDL_HOST_EXPORT double rvdl_inversion(double R);
    RVDL_HOST_EXPORT double rvdl_hyperbolic_metric(double x);
    RVDL_HOST_EXPORT double rvdl_hyperbolic_distance(double x, double y);
}

#if defined(RVDL_RT_BUILD)
double rvdl_geodesic_distance(QObject *a, QObject *b)
{
    if (!a || !b)
        return -1.0;
    double overlap = rvdl_qattention(a, b);
    if (overlap < 0.0)
        overlap = 0.0;
    if (overlap > 1.0)
        overlap = 1.0;
    return std::acos(std::sqrt(overlap));
}

double rvdl_inversion(double R)
{
    if (R == 0.0)
        return 0.0;
    return 1.0 / R;
}

double rvdl_hyperbolic_metric(double x)
{
    const double x2 = x * x;
    if (x2 >= 1.0)
        return 0.0;
    const double denom = 1.0 - x2;
    return 4.0 / (denom * denom);
}

double rvdl_hyperbolic_distance(double x, double y)
{
    const double x2 = x * x;
    const double y2 = y * y;
    if (x2 >= 1.0 || y2 >= 1.0)
        return -1.0;
    const double dx = x - y;
    const double t = 1.0 + 2.0 * dx * dx / ((1.0 - x2) * (1.0 - y2));
    if (t < 1.0)
        return 0.0;
    return std::acosh(t);
}
#endif