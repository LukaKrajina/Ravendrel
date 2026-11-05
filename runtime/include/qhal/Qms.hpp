#pragma once
// ============================================================================
//  量子 Markov 半群 数值内核
//  只要存在谱隙，即得非交换 p-Poincare 不等式。
//  并由此恢复次指数集中不等式与Lipschitz 直径估计。
//
//  具体到 Ravendrel：QVM 的噪声信道正是一个离散时间 QMS。本头文件把"谱隙"
//  这一核心量做成可调用的内建函数，用于自适应决定 shots 数与纠错码距。
//
//  单比特信道在 Pauli 转移矩阵（PTM）表示下是 4x4 实矩阵，对其角化可得
//  特征值 {1, lambda_X, lambda_Y, lambda_Z}。谱隙定义为
//
//      lambda = 1 - max(|lambda_X|, |lambda_Y|, |lambda_Z|)
//
//  对下列标准信道模型，PTM 恰好是对角阵，因此谱隙是**精确解析值**，
//  无需数值迭代：
//
//    去极化  Depolarizing    : PTM = diag(1, 1-p, 1-p, 1-p)
//    去相位  Dephasing       : PTM = diag(1, 1-2p, 1-2p, 1)
//    振幅阻尼 Amplitude damp.: PTM = diag(1, sqrt(1-g), sqrt(1-g), 1-g)
//    Pauli 信道              : PTM = diag(1, lX, lY, lZ)
//        其中 lX = (pI+pX)-(pY+pZ)，lY、lZ 同理循环对称
// ============================================================================

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
        // ---- 噪声信道模型 -----------------------------------------------------
        enum ChannelModel : std::int32_t
        {
            CH_DEPOLARIZING      = 0,   // E(rho) = (1-p) rho + p * I/2
            CH_DEPHASING         = 1,   // E(rho) = (1-p) rho + p * Z rho Z
            CH_AMPLITUDE_DAMPING = 2,   // Kraus K0/K1，参数 g = p，q 未使用
            CH_PAULI             = 3,   // pX = p, pY = q, pZ = 1 - p - q
        };

        // PTM 的三个非平凡特征值
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
                // pI = 1 - 3p/4, pX = pY = pZ = p/4  =>  lX = lY = lZ = 1 - p
                double l = 1.0 - p;
                s = {l, l, l};
                break;
            }

            case CH_DEPHASING:
            {
                // pI = 1-p, pZ = p  =>  lX = lY = 1-2p, lZ = 1
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
                    // 归一化回合法单纯形
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
                // 未知模型：退化为恒等信道（gap = 0）
                s = {1.0, 1.0, 1.0};
                break;
            }

            return s;
        }

        /*
            谱隙 lambda = 1 - max |lambda_i|

            上式只在信道**遍历**（ergodic，即不动点唯一）时才给出真正的
            混合速率。反例 —— 去相位信道 E(rho) = (1-p)rho + p*Z rho Z 满足
                E(I) = I,  E(Z) = Z,  E(X) = (1-2p)X,  E(Y) = (1-2p)Y
            即 Z 方向被完整保留，不动点空间是二维的（所有 Z 对角态），于是
                PTM = diag(1, 1-2p, 1-2p, 1)  ->  lambda = 1 - max(...) = 0
            这在数学上是正确的：该信道不收敛到唯一稳态，谈不上"混合"。
            去极化与振幅阻尼都是遍历的（唯一不动点为最大混态），故谱隙非零。

            调用方须注意：lambda = 0 表示"该信道非遍历"，此时 mixing_bound()
            返回 INFINITY 是正确行为而非缺陷。若要让非遍历信道也得到有意义的
            混合速率，需在不动点代数的正交补上求**条件谱隙**。
        */
        inline double spectral_gap(std::int32_t model, double p, double q)
        {
            PtmSpectrum s = ptm_spectrum(model, p, q);
            double m = std::fabs(s.lx);
            if (std::fabs(s.ly) > m) m = std::fabs(s.ly);
            if (std::fabs(s.lz) > m) m = std::fabs(s.lz);
            double gap = 1.0 - m;
            return (gap < 0.0) ? 0.0 : gap;
        }

        // 混合时间上界：t_mix <= (1/lambda) * log(n / eps)
        // 其中 n = 1/pi_min 为有效状态空间规模，与既有
        // rvdl_polymer_mix_bound(n, eps) = n * log(n/eps) 同构（gap = 1 时退化一致）。
        inline double mixing_bound(double gap, double n, double eps)
        {
            if (eps <= 0.0) eps = 1e-12;
            if (n <= 1.0)   n   = 2.0;
            if (gap <= 0.0) return INFINITY;
            return std::log(n / eps) / gap;
        }

        // L2 方差收缩时间（Poincare 不等式 p=2 情形的直接推论）
        //   Var(T_t f) <= exp(-2 lambda t) * Var(f)
        // 令右端 <= eps * Var(f)，解得 t >= log(1/eps) / (2 lambda)
        inline double variance_contraction_time(double gap, double eps)
        {
            if (eps <= 0.0) eps = 1e-12;
            if (gap <= 0.0) return INFINITY;
            return std::log(1.0 / eps) / (2.0 * gap);
        }
    }
}

// ============================================================================
//  C ABI
// ============================================================================
RVDL_HOST_EXPORT double rvdl_qms_gap(std::int32_t model, double p, double q);
RVDL_HOST_EXPORT double rvdl_mix_bound(double gap, double n, double eps);
RVDL_HOST_EXPORT double rvdl_qms_conc(double gap, double eps);

// 注：rvdl_qms_gap / rvdl_mix_bound / rvdl_qms_conc 的定义已下沉到 qml/SoftLogicAbi.hpp
// （extern "C" 非 inline 定义，由 runtime_api.cpp 单点 include），避免 JIT.hpp 内
// inline 定义导致 LLVM ORC JIT 符号无法解析（"Symbols not found"）。