#pragma once
#include <vector>
#include <complex>
#include <cmath>
#include <random>
#include "SphericalSlice.hpp"

namespace rvdl::spacetime
{

    struct NeuralFieldParams
    {
        double tau = 1.0; // 时间常数
        double D = 0.05;  // 扩散系数
        double mu = 2.0;  // 双阱参数
        double dt = 0.01;
        double I = 0.0;  // 常数外部输入
        int L = 16;      // 球谐截断
        int Ntheta = 32; // 高斯-勒让德节点数
        int Nphi = 64;   // 格点数
        double R = 1.0;  // 皮层球面半径
        uint64_t seed = 42u;
        double noise = 1e-3; // 初始涨落幅值
    };

    // 神经场序参量（意识状态的连续场刻画）
    struct NeuralFieldOrderParameter
    {
        double mean_field = 0.0;          // 空间平均场（对应旧 arousal）
        double symmetry_breaking = 0.0;   // 双稳态破缺程度
        double domain_wall_density = 0.0; // 空间结构复杂度
        double variance = 0.0;            // 空间方差
    };

    class NeuralField
    {
    public:
        explicit NeuralField(const NeuralFieldParams &p) : p_(p), slice_(p.Ntheta, p.Nphi, p.R)
        {
            u_.assign(slice_.size(), 0.0);
            std::mt19937_64 rng(p.seed);
            std::normal_distribution<double> dist(0.0, 1.0);
            for (double &v : u_)
                v = p.noise * dist(rng);
        }

        // 谱空间 IMEX 前向欧拉：扩散项隐式对角化，非线性显式
        void step()
        {
            std::vector<double> N(u_.size());
            for (size_t i = 0; i < u_.size(); ++i)
            {
                double v = u_[i];
                N[i] = (p_.mu - 1.0) * v - v * v * v + p_.I;
            }
            std::vector<std::complex<double>> cu, cN;
            slice_.forward(u_, p_.L, cu);
            slice_.forward(N, p_.L, cN);

            const double r2 = p_.R * p_.R;
            const double c = p_.dt / p_.tau;
            for (int l = 0; l <= p_.L; ++l)
                for (int m = -l; m <= l; ++m)
                {
                    size_t idx = static_cast<size_t>(l * l + (l + m));
                    double k2 = l * (l + 1) / r2;
                    double denom = 1.0 + c * p_.D * k2;
                    cu[idx] = (cu[idx] + c * cN[idx]) / denom;
                }
            slice_.backward(cu, p_.L, u_);
        }

        NeuralFieldOrderParameter order_parameter() const
        {
            NeuralFieldOrderParameter op;
            const double dph = 2.0 * spherical::PI / p_.Nphi;
            double total = 0.0, sum = 0.0;
            for (int i = 0; i < p_.Ntheta; ++i)
            {
                double w = slice_.gw[i] * dph;
                for (int j = 0; j < p_.Nphi; ++j)
                {
                    total += w;
                    sum += u_[slice_.index(i, j)] * w;
                }
            }
            op.mean_field = sum / total;
            op.symmetry_breaking = std::abs(op.mean_field);

            double var = 0.0;
            for (int i = 0; i < p_.Ntheta; ++i)
            {
                double w = slice_.gw[i] * dph;
                for (int j = 0; j < p_.Nphi; ++j)
                {
                    double dv = u_[slice_.index(i, j)] - op.mean_field;
                    var += dv * dv * w;
                }
            }
            op.variance = var / total;

            // 畴壁密度
            std::vector<std::complex<double>> cu;
            slice_.forward(u_, p_.L, cu);
            const double r2 = p_.R * p_.R;
            double g2 = 0.0;
            for (int l = 0; l <= p_.L; ++l)
                for (int m = -l; m <= l; ++m)
                    g2 += (l * (l + 1) / r2) * std::norm(cu[static_cast<size_t>(l * l + (l + m))]);
            op.domain_wall_density = g2;
            return op;
        }

        std::vector<double> &field() { return u_; }
        const std::vector<double> &field() const { return u_; }
        const SphericalSlice &slice() const { return slice_; }
        const NeuralFieldParams &params() const { return p_; }

    private:
        NeuralFieldParams p_;
        SphericalSlice slice_;
        std::vector<double> u_;
    };
}