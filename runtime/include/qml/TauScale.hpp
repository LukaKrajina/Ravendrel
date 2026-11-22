#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <tuple>
#include <vector>

namespace numqk
{
    namespace tau
    {


        struct TauLayers
        {
            double state = 0.0;
            double gate = 0.0;
            double measure = 0.0;
            double comm = 0.0;
        };

        struct TauWeights
        {
            double w_state   = 1.0;
            double w_gate    = 1.0;
            double w_measure = 1.0;
            double w_comm    = 1.0;
        };





        inline double compose(const TauLayers &t, const TauWeights &w)
        {
            return w.w_state * t.state + w.w_gate * t.gate + w.w_measure * t.measure +
                w.w_comm * t.comm;
        }


        enum class AlphaDomain : std::int32_t
        {
            Mobile = 0,
            Autonomous = 1,
            Ai = 2,
        };




        inline double alpha_of(AlphaDomain d)
        {
            switch (d)
            {
            case AlphaDomain::Mobile:     return 1.3;
            case AlphaDomain::Autonomous: return 1.5;
            case AlphaDomain::Ai:         return 10.0;
            default:                      return 1.0;
            }
        }

        inline double alpha_preset(std::int32_t preset)
        {
            return alpha_of(static_cast<AlphaDomain>(preset < 0 ? -1 : preset > 2 ? -1 : preset));
        }

        inline double generational(double tau, double alpha)
        {
            if (!(alpha > 0.0)) return tau;
            return tau / alpha;
        }

        inline double anneal(double budget, double alpha)
        {
            return generational(budget, alpha);
        }







        inline double critical_path(const std::vector<double> &cost,
                                    const std::vector<std::pair<std::int32_t, std::int32_t>> &edges)
        {
            const std::int32_t n = static_cast<std::int32_t>(cost.size());
            if (n <= 0) return 0.0;

            std::vector<std::int32_t> indeg(n, 0);
            std::vector<std::vector<std::int32_t>> succ(n);
            for (const auto &e : edges)
            {
                const std::int32_t u = e.first, v = e.second;
                if (u < 0 || u >= n || v < 0 || v >= n || u == v) continue;
                succ[u].push_back(v);
                indeg[v]++;
            }

            std::vector<double> dp(n, 0.0);
            std::vector<std::int32_t> queue;
            queue.reserve(static_cast<size_t>(n));
            for (std::int32_t i = 0; i < n; i++) if (indeg[i] == 0) queue.push_back(i);
            for (size_t head = 0; head < queue.size(); head++)
            {
                const std::int32_t u = queue[head];
                dp[u] += cost[u];
                for (const std::int32_t v : succ[u])
                {
                    dp[v] = std::max(dp[v], dp[u]);
                    if (--indeg[v] == 0) queue.push_back(v);
                }
            }

            double result = 0.0;
            for (std::int32_t i = 0; i < n; i++) result = std::max(result, dp[i]);
            return result;
        }






        inline double fold_depth(double L, double T)
        {
            if (!(T > 0.0)) return L;
            if (T >= L) return 1.0;
            return std::ceil(L / T);
        }









        inline double gear_ratio(double e_cross, double e_intra)
        {
            if (e_intra > 0.0) return e_cross / e_intra;
            if (e_cross > 0.0) return 9.007199254740992e15;
            return 0.0;
        }









        inline double route_cost(double depth, double gear, double t_gate, double t_comm)
        {
            const double d = std::max(0.0, depth);
            const double g = std::max(0.0, gear);
            return d * t_gate + d * g * t_comm;
        }








        inline double route_price(double value, double cost, double lambda)
        {
            return value - lambda * cost;
        }








        inline std::tuple<double, double, double> budget_route(const std::vector<double> &values,
                                                            const std::vector<double> &costs,
                                                            double budget,
                                                            std::vector<std::int32_t> &chosen)
        {
            chosen.clear();
            const size_t n = values.size();
            if (n == 0 || costs.size() != n || budget < 0.0) return {0.0, 0.0, 0.0};

            double total_cost = 0.0;
            double max_ratio  = 0.0;
            for (size_t i = 0; i < n; i++)
            {
                total_cost += costs[i];
                if (costs[i] > 0.0) max_ratio = std::max(max_ratio, values[i] / costs[i]);
            }

            if (total_cost <= budget)
            {
                for (size_t i = 0; i < n; i++) chosen.push_back(static_cast<std::int32_t>(i));
                double vsum = 0.0;
                for (double x : values) vsum += x;
                return {0.0, vsum, total_cost};
            }

            double lo = 0.0, hi = max_ratio + 1.0;
            std::vector<bool> pick(n, false);
            for (int iter = 0; iter < 80; iter++)
            {
                const double mid = 0.5 * (lo + hi);
                double c = 0.0;
                for (size_t i = 0; i < n; i++)
                {
                    pick[i] = values[i] - mid * costs[i] > 0.0;
                    if (pick[i]) c += costs[i];
                }
                if (c <= budget)
                    hi = mid;
                else
                    lo = mid;
            }

            const double lambda = hi;
            double vsum = 0.0, csum = 0.0;
            for (size_t i = 0; i < n; i++)
            {
                pick[i] = values[i] - lambda * costs[i] > 0.0;
                if (pick[i])
                {
                    chosen.push_back(static_cast<std::int32_t>(i));
                    vsum += values[i];
                    csum += costs[i];
                }
            }
            if (csum > budget && lo < hi)
            {
                chosen.clear();
                vsum = 0.0; csum = 0.0;
                for (size_t i = 0; i < n; i++)
                {
                    pick[i] = values[i] - lo * costs[i] > 0.0;
                    if (pick[i])
                    {
                        chosen.push_back(static_cast<std::int32_t>(i));
                        vsum += values[i];
                        csum += costs[i];
                    }
                }
            }
            return {csum <= budget ? lambda : lo, vsum, csum};
        }
    }
}