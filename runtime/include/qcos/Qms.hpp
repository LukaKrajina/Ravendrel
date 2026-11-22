#pragma once

#include <stdint.h>

namespace qcos
{
    namespace qms
    {
        inline double qms_abs(double x)
        {
            return x < 0.0 ? -x : x;
        }

        inline double qms_sqrt(double x)
        {
            if (x <= 0.0) return 0.0;
            double m = x;
            double scale = 1.0;
            while (m > 2.0) { m *= 0.25; scale *= 2.0; }
            while (m < 0.5) { m *= 4.0;  scale *= 0.5; }
            double y = (m > 1.0) ? 1.0 : 0.75;
            for (int i = 0; i < 8; ++i)
            {
                y = 0.5 * (y + m / y);
            }
            return y * scale;
        }

        inline double qms_log(double x)
        {
            if (x <= 0.0) return -1.7976931348623157e308;
            double m = x;
            double k = 0.0;
            while (m >= 2.0) { m *= 0.5; k += 1.0; }
            while (m < 1.0)  { m *= 2.0; k -= 1.0; }
            const double z  = (m - 1.0) / (m + 1.0);
            const double z2 = z * z;
            double term = z;
            double sum  = 0.0;
            for (int i = 1; i <= 19; i += 2)
            {
                sum += term / static_cast<double>(i);
                term *= z2;
            }
            const double LN2 = 0.693147180559945309417232121458;
            return k * LN2 + 2.0 * sum;
        }


        enum ChannelModel : int32_t
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

        inline PtmSpectrum ptm_spectrum(int32_t model, double p, double q)
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
                double r = qms_sqrt(1.0 - g);
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
                    px *= k; py *= k; pz *= k;
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

        inline double spectral_gap(int32_t model, double p, double q)
        {
            PtmSpectrum s = ptm_spectrum(model, p, q);
            double m = qms_abs(s.lx);
            if (qms_abs(s.ly) > m) m = qms_abs(s.ly);
            if (qms_abs(s.lz) > m) m = qms_abs(s.lz);
            double gap = 1.0 - m;
            return (gap < 0.0) ? 0.0 : gap;
        }

        inline double mixing_bound(double gap, double n, double eps)
        {
            if (eps <= 0.0) eps = 1e-12;
            if (n <= 1.0)   n   = 2.0;
            if (gap <= 0.0) return 1.7976931348623157e308;
            return qms_log(n / eps) / gap;
        }

        inline double variance_contraction_time(double gap, double eps)
        {
            if (eps <= 0.0) eps = 1e-12;
            if (gap <= 0.0) return 1.7976931348623157e308;
            return qms_log(1.0 / eps) / (2.0 * gap);
        }
    }
}