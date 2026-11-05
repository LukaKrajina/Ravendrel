#pragma once
#include <random>
#include <cmath>
#include "TachyonField.hpp"

namespace rvdl::spacetime
{

    // 高斯白噪声涨落（固定 seed）
    inline void init_gaussian_noise(TachyonField &tf, double amplitude)
    {
        std::mt19937_64 rng(tf.cfg.seed);
        std::normal_distribution<double> dist(0.0, 1.0);
        for (size_t p = 0; p < tf.phi.size(); ++p)
        {
            tf.phi[p] = amplitude * dist(rng);
            tf.pi[p] = 0.0;
        }
    }

    namespace detail
    {
        // 把展平索引映射回 axis 方向的物理坐标
        inline double coord_at(const TachyonField &tf, int i, int j, int k, int axis)
        {
            if (axis == 0)
                return i * tf.grid.dx(0);
            if (axis == 1)
                return j * tf.grid.dx(1);
            return k * tf.grid.dx(2);
        }
    }

    // 1D kink 静解
    inline void init_kink(TachyonField &tf, int axis = 0)
    {
        const double vac = tf.cfg.vacuum();
        const double kscale = std::sqrt(tf.cfg.mu2) / std::sqrt(2.0);
        const double center = 0.5 * tf.grid.length[axis];

        for (int kk = 0; kk < tf.grid.n[2]; ++kk)
            for (int j = 0; j < tf.grid.n[1]; ++j)
                for (int i = 0; i < tf.grid.n[0]; ++i)
                {
                    const int idx = tf.grid.index(i, j, kk);
                    double x = detail::coord_at(tf, i, j, kk, axis);
                    tf.phi[idx] = vac * std::tanh(kscale * (x - center));
                    tf.pi[idx] = 0.0;
                }
    }

    // kink–反 kink 对
    // 两端均趋于 -φ_vac，周期边界连续
    // 中心在 L/4、3L/4 处
    inline void init_kink_antikink_pair(TachyonField &tf, int axis = 0)
    {
        const double vac = tf.cfg.vacuum();
        const double kscale = std::sqrt(tf.cfg.mu2) / std::sqrt(2.0);
        const double L = tf.grid.length[axis];
        const double c1 = 0.25 * L;
        const double c2 = 0.75 * L;

        for (int kk = 0; kk < tf.grid.n[2]; ++kk)
            for (int j = 0; j < tf.grid.n[1]; ++j)
                for (int i = 0; i < tf.grid.n[0]; ++i)
                {
                    const int idx = tf.grid.index(i, j, kk);
                    double x = detail::coord_at(tf, i, j, kk, axis);
                    tf.phi[idx] = vac * (std::tanh(kscale * (x - c1)) -
                                         std::tanh(kscale * (x - c2)) - 1.0);
                    tf.pi[idx] = 0.0;
                }
    }
}