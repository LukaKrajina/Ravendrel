#pragma once
// ============================================================================
//  量子 Markov 半群数值内核的 **freestanding** 实现
//
//  与 qhal/Qms.hpp（hosted，依赖 <cmath>）不同，本头文件把量子服务"下沉"
//  到裸机内核：不依赖 libm / libstdc++ / 异常 / RTTI，仅用 <cstdint> 与
//  手写的 sqrt / log。qcos_core 与 rvdl_shim 链接它，使纯 RVDL 内核可经
//  rvdl_qms_gap / rvdl_mix_bound / rvdl_qms_conc 直接调用谱隙服务。
//
//  数值语义与 qhal::qms 完全一致：
//     spectral_gap(model,p,q) = 1 - max(|lambda_X|,|lambda_Y|,|lambda_Z|)
//     mixing_bound(gap,n,eps)  = ln(n/eps) / gap
//     variance_contraction_time(gap,eps) = ln(1/eps) / (2*gap)
// ============================================================================

// 注意：freestanding 目标必须用 C 头 <stdint.h>（clang 对所有目标内置），
// 而非 C++ 包装头 <cstdint>（依赖 libstdc++，交叉 freestanding 目标没有）。
#include <stdint.h>

namespace qcos
{
    namespace qms
    {
        inline double qms_abs(double x)
        {
            return x < 0.0 ? -x : x;
        }

        // 平方根（牛顿迭代）：先把 x 归一到 [0.5, 2)，再 Newton 迭代，最后回乘尺度。
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

        // 自然对数：把 x 拆成 m * 2^k（m ∈ [1,2)），ln(x) = k*ln2 + ln(m)，
        // ln(m) 用 atanh 级数：z = (m-1)/(m+1)，ln(m) = 2*atanh(z)。
        inline double qms_log(double x)
        {
            if (x <= 0.0) return -1.7976931348623157e308; // -inf（实际返回 -DBL_MAX）
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

        // ---------------------------------------------------------------------------
        // 噪声信道模型
        // ---------------------------------------------------------------------------

        enum ChannelModel : int32_t
        {
            CH_DEPOLARIZING      = 0,   // E(rho) = (1-p) rho + p * I/2
            CH_DEPHASING         = 1,   // E(rho) = (1-p) rho + p * Z rho Z
            CH_AMPLITUDE_DAMPING = 2,   // Kraus K0/K1，参数 g = p，q 未使用
            CH_PAULI             = 3,   // pX = p, pY = q, pZ = 1 - p - q
        };

        struct PtmSpectrum
        {
            double lx;
            double ly;
            double lz;
        };

        // PTM 的三个非平凡特征值（单比特信道，PTM 为对角阵，所以是解析值）
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