#pragma once

#include "numqk/pb.hpp"

#include <cmath>
#include <cstddef>
#include <vector>

namespace numqk
{
    namespace moe
    {
        struct MoETopology
        {
            double pB = 0.0;
            size_t groups = 0;
            size_t experts_per = 0;
            size_t virtual_experts = 0;
            size_t materialized = 0;
            double savings = 1.0;
        };

        inline MoETopology auto_moe_topology(double pB)
        {
            MoETopology t;
            t.pB = pB;
            if (pB <= 0.0)
                pB = 0.5;

            double g = 2.0 + std::floor(std::log2(pB));
            if (g < 2.0)
                g = 2.0;
            if (g > 4.0)
                g = 4.0;
            t.groups = static_cast<size_t>(g);

            double k = std::round(2.0 * pB);
            if (k < 2.0)
                k = 2.0;
            if (k > 8.0)
                k = 8.0;
            t.experts_per = static_cast<size_t>(k);

            t.virtual_experts = t.groups * t.experts_per;
            t.materialized = t.groups + t.experts_per;
            t.savings =
                static_cast<double>(t.virtual_experts) / static_cast<double>(t.materialized);
            return t;
        }

        inline double curvature_gate(double g)
        {
            if (g > 100.0)
                return 0.0;
            if (g < -100.0)
                return 1.0;
            return 1.0 / (1.0 + std::exp(g));
        }

        struct ProductKeyRoute
        {
            size_t group = 0;
            size_t expert = 0;
            double weight = 0.0;
        };

        inline ProductKeyRoute product_key_route(const double* group_gates, size_t G,
                                                 const double* intra_gates, size_t K)
        {
            ProductKeyRoute r;
            double gb = -1e300, kb = -1e300;
            for (size_t i = 0; i < G; ++i)
                if (group_gates[i] > gb)
                {
                    gb = group_gates[i];
                    r.group = i;
                }
            for (size_t j = 0; j < K; ++j)
                if (intra_gates[j] > kb)
                {
                    kb = intra_gates[j];
                    r.expert = j;
                }
            r.weight = gb * kb;
            return r;
        }

        struct FactorizedExperts
        {
            std::vector<numqk::pb::PointParams> u_blocks;
            std::vector<numqk::pb::PointParams> v_blocks;
            size_t d = 0;
        };

        inline FactorizedExperts factorize_experts(const std::vector<double>& W, size_t G, size_t K,
                                                   size_t d, size_t block, size_t rank, size_t q)
        {
            FactorizedExperts fe;
            fe.d = d;
            for (size_t g = 0; g < G; ++g)
            {
                std::vector<double> u(d, 0.0);
                for (size_t k = 0; k < K; ++k)
                    for (size_t i = 0; i < d; ++i)
                        u[i] += W[(g * K + k) * d + i] / static_cast<double>(K);
                fe.u_blocks.push_back(numqk::pb::compress(u, block, rank, q));
            }
            for (size_t k = 0; k < K; ++k)
            {
                std::vector<double> v(d, 0.0);
                for (size_t g = 0; g < G; ++g)
                {
                    const std::vector<double> u = numqk::pb::decompress(fe.u_blocks[g]);
                    for (size_t i = 0; i < d; ++i)
                        v[i] += (W[(g * K + k) * d + i] - u[i]) / static_cast<double>(G);
                }
                fe.v_blocks.push_back(numqk::pb::compress(v, block, rank, q));
            }
            return fe;
        }

        inline std::vector<double> rebuild_expert(const FactorizedExperts& fe, size_t g, size_t k)
        {
            std::vector<double> u = numqk::pb::decompress(fe.u_blocks[g]);
            const std::vector<double> v = numqk::pb::decompress(fe.v_blocks[k]);
            for (size_t i = 0; i < fe.d; ++i)
                u[i] += v[i];
            return u;
        }


        inline double inversion(double x)
        {
            return 1.0 / x;
        }

        inline double polymer_weight(double beta, double h, double len)
        {
            return std::pow(2.0 * std::cosh(beta * h), len);
        }

        inline double dual_fuse(double expert_a, double expert_b, double g, double theta)
        {
            const double wa = curvature_gate(g);
            const double wb = 1.0 - wa;
            const double a = expert_a * theta;
            const double b = inversion(expert_b + 1e-3) * polymer_weight(1.0, 0.5, 3.0);
            return wa * a + wb * b;
        }

        struct DualRoute
        {
            size_t group = 0;
            size_t expert_a = 0;
            size_t expert_b = 0;
            double value = 0.0;
            double weight_a = 0.5;
            double weight_b = 0.5;
        };

        inline DualRoute dual_moe_forward(const double* activations, size_t G, size_t K,
                                          const double* group_gates, const double* intra_gates,
                                          double g, double theta)
        {
            DualRoute dr;
            double gb = -1e300;
            for (size_t i = 0; i < G; ++i)
                if (group_gates[i] > gb)
                {
                    gb = group_gates[i];
                    dr.group = i;
                }
            size_t e1 = 0, e2 = 1;
            if (intra_gates[e2] > intra_gates[e1])
            {
                size_t t = e1;
                e1 = e2;
                e2 = t;
            }
            for (size_t j = 2; j < K; ++j)
            {
                if (intra_gates[j] > intra_gates[e1])
                {
                    e2 = e1;
                    e1 = j;
                }
                else if (intra_gates[j] > intra_gates[e2])
                {
                    e2 = j;
                }
            }
            dr.expert_a = e1;
            dr.expert_b = e2;
            const double a = activations[dr.group * K + e1];
            const double b = activations[dr.group * K + e2];
            dr.weight_a = curvature_gate(g);
            dr.weight_b = 1.0 - dr.weight_a;
            dr.value = dr.weight_a * (a * theta) +
                       dr.weight_b * (inversion(b + 1e-3) * polymer_weight(1.0, 0.5, 3.0));
            return dr;
        }
    }
}