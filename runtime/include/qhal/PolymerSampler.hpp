#pragma once
#include <vector>
#include <cmath>
#include <random>
#include <algorithm>

namespace qhal {
    struct Polymer {
        std::vector<size_t> vertices;
        double weight = 0.0;
        size_t order() const { return vertices.size(); }
    };
    
    class PolymerSampler {
    public:
        static bool satisfies_vertex_incompatibility(
                const std::vector<Polymer>& polys, size_t n_vertices, double theta) {
            std::vector<double> sum(n_vertices, 0.0);
            for (const auto& p : polys)
                for (size_t v : p.vertices)
                    sum[v] += static_cast<double>(p.order()) * p.weight;
            for (double s : sum) if (s > theta + 1e-12) return false;
            return true;
        }

        static double ferromagnetic_weight(double beta, double h, size_t cycle_len) {
            if (cycle_len == 0) return 0.0;
            double fact = 1.0;
            for (size_t k = 2; k <= cycle_len; ++k) fact *= static_cast<double>(k);
            double c   = 2.0 * std::cosh(beta * h);
            double num = 2.0 * std::cosh(beta * h * static_cast<double>(cycle_len));
            return (std::pow(beta, static_cast<double>(cycle_len)) / fact)
                * (num / std::pow(c, static_cast<double>(cycle_len)));
        }
        
        static bool step(std::vector<Polymer>& config, size_t n_vertices,
                        const std::vector<Polymer>& candidates_at,
                        const std::vector<std::vector<size_t>>& incidence,
                        std::mt19937& rng) {
            std::uniform_int_distribution<size_t> pick(0, n_vertices - 1);
            std::uniform_real_distribution<double> dist(0.0, 1.0);
            size_t v = pick(rng);

            int cur = -1;
            for (size_t i = 0; i < config.size(); ++i) {
                auto& vs = config[i].vertices;
                if (std::find(vs.begin(), vs.end(), v) != vs.end()) { cur = static_cast<int>(i); break; }
            }

            if (dist(rng) < 1.0 / 3.0) {
                if (cur >= 0) { config.erase(config.begin() + cur); return true; }
                return false;
            }

            if (v >= candidates_at.size()) return false;
            const Polymer& cand = candidates_at[v];
            double w_v = cand.weight;
            if (dist(rng) < w_v / 2.0) {
                config.push_back(cand);
                return true;
            }
            return false;
        }

        static double mixing_time_bound(size_t n_vertices, double eps) {
            return static_cast<double>(n_vertices)
                * std::log(static_cast<double>(n_vertices) / eps);
        }
    };
}