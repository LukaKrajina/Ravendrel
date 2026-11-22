#pragma once

#include "Export.hpp"

#include <cmath>
#include <cstdint>

#ifndef RVDL_HOST_EXPORT
#define RVDL_HOST_EXPORT extern "C" RVDL_RT_API
#endif

namespace qhal
{
    namespace qms
    {
        enum ChannelModel : std::int32_t
        {
            CH_DEPOLARIZING = 0,
            CH_DEPHASING = 1,
            CH_AMPLITUDE_DAMPING = 2,
            CH_PAULI = 3,
        };

        struct PtmSpectrum
        {
            double lx;
            double ly;
            double lz;
        };

        inline PtmSpectrum ptm_spectrum(std::int32_t model, double p, double q)
        {
            PtmSpectrum s{1.0, 1.0, 1.0};

            switch (model)
            {
            case CH_DEPOLARIZING:
            {
                double l = 1.0 - p;
                s = {l, l, l};
                break;
            }

            case CH_DEPHASING:
            {
                double l = 1.0 - 2.0 * p;
                s = {l, l, 1.0};
                break;
            }

            case CH_AMPLITUDE_DAMPING:
            {
                double g = p;
                if (g < 0.0) g = 0.0;
                if (g > 1.0) g = 1.0;
                double r = std::sqrt(1.0 - g);
                s = {r, r, 1.0 - g};
                break;
            }

            case CH_PAULI:
            {
                double px = p;
                double py = q;
                double pz = 1.0 - p - q;
                double sum = px + py + pz;
                if (sum > 1.0)
                {
                    double k = 1.0 / sum;
                    px *= k;
                    py *= k;
                    pz *= k;
                }
                else if (sum < 0.0)
                {
                    px = py = pz = 0.0;
                }
                const double pI = 1.0 - (px + py + pz);
                s.lx = (pI + px) - (py + pz);
                s.ly = (pI + py) - (px + pz);
                s.lz = (pI + pz) - (px + py);
                break;
            }

            default:
                s = {1.0, 1.0, 1.0};
                break;
            }

            return s;
        }















        inline double spectral_gap(std::int32_t model, double p, double q)
        {
            PtmSpectrum s = ptm_spectrum(model, p, q);
            double m = std::fabs(s.lx);
            if (std::fabs(s.ly) > m) m = std::fabs(s.ly);
            if (std::fabs(s.lz) > m) m = std::fabs(s.lz);
            double gap = 1.0 - m;
            return (gap < 0.0) ? 0.0 : gap;
        }

        inline double mixing_bound(double gap, double n, double eps)
        {
            if (eps <= 0.0) eps = 1e-12;
            if (n <= 1.0)   n   = 2.0;
            if (gap <= 0.0) return INFINITY;
            return std::log(n / eps) / gap;
        }

        inline double variance_contraction_time(double gap, double eps)
        {
            if (eps <= 0.0) eps = 1e-12;
            if (gap <= 0.0) return INFINITY;
            return std::log(1.0 / eps) / (2.0 * gap);
        }
    }
}

RVDL_HOST_EXPORT double rvdl_qms_gap(std::int32_t model, double p, double q);
RVDL_HOST_EXPORT double rvdl_mix_bound(double gap, double n, double eps);
RVDL_HOST_EXPORT double rvdl_qms_conc(double gap, double eps);