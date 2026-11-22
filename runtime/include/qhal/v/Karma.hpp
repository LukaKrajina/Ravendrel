#pragma once
#include <vector>
#include <complex>
#include <unordered_map>
#include <string>
#include <memory>
#include <functional>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <random>
#include <sstream>
#include <iostream>
#include <iomanip>
#include "../IQuantumBackend.hpp"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#ifndef M_PI_2
#define M_PI_2 1.5707963267948966
#endif

namespace qhal
{

    class CliffordTableau
    {
    private:
        size_t n_;
        std::vector<uint64_t> x_;
        std::vector<uint64_t> z_;
        std::vector<uint8_t> r_;
        static std::pair<uint8_t, uint8_t> pauli_mult(uint8_t b, uint8_t a)
        {
            static const int8_t RESULT[4][4] = {
                {0, 1, 2, 3},
                {1, 0, 3, 2},
                {2, 3, 0, 1},
                {3, 2, 1, 0},
            };
            static const int8_t PHASE[4][4] = {
                {0, 0, 0, 0},
                {0, 0, 3, 1},
                {0, 1, 0, 3},
                {0, 3, 1, 0},
            };
            return {static_cast<uint8_t>(RESULT[b][a]), static_cast<uint8_t>(PHASE[b][a])};
        }

        static int popcount(uint64_t v)
        {
#if defined(_MSC_VER)
            return static_cast<int>(__popcnt64(v));
#else
            return __builtin_popcountll(v);
#endif
        }

    public:
        explicit CliffordTableau(size_t n)
            : n_(n), x_(2 * n, 0), z_(2 * n, 0), r_(2 * n, 0)
        {
            for (size_t i = 0; i < n; ++i)
            {
                x_[i] = 1ULL << i;
                z_[n + i] = 1ULL << i;
            }
        }

        size_t num_qubits() const { return n_; }

        void rowsum(size_t h, size_t i)
        {
            uint64_t xh = x_[h], zh = z_[h];
            uint64_t xi = x_[i], zi = z_[i];
            uint64_t nx = 0, nz = 0;
            int phase = 0;

            for (size_t k = 0; k < n_; ++k)
            {
                int xhk = (xh >> k) & 1, zhk = (zh >> k) & 1;
                int xik = (xi >> k) & 1, zik = (zi >> k) & 1;
                uint8_t pb = static_cast<uint8_t>(xik + 2 * zik);
                uint8_t pa = static_cast<uint8_t>(xhk + 2 * zhk);
                auto [res, dp] = pauli_mult(pb, pa);
                nx |= static_cast<uint64_t>(res & 1) << k;
                nz |= static_cast<uint64_t>((res >> 1) & 1) << k;
                phase += dp;
            }
            x_[i] = nx;
            z_[i] = nz;
            r_[i] = static_cast<uint8_t>((r_[i] + r_[h] + phase) & 3);
        }

        void apply_cnot(size_t a, size_t b)
        {
            for (size_t i = 0; i < 2 * n_; ++i)
            {
                uint64_t xa = (x_[i] >> a) & 1;
                uint64_t zb = (z_[i] >> b) & 1;
                r_[i] ^= static_cast<uint8_t>(xa & zb & (1 ^ ((x_[i] >> b) & 1) ^ ((z_[i] >> a) & 1)));
                x_[i] ^= xa << b;
                z_[i] ^= zb << a;
            }
        }

        void apply_h(size_t q)
        {
            for (size_t i = 0; i < 2 * n_; ++i)
            {
                uint64_t xq = (x_[i] >> q) & 1;
                uint64_t zq = (z_[i] >> q) & 1;
                r_[i] ^= static_cast<uint8_t>(xq & zq);
                x_[i] = (x_[i] & ~(1ULL << q)) | (zq << q);
                z_[i] = (z_[i] & ~(1ULL << q)) | (xq << q);
            }
        }

        void apply_s(size_t q)
        {
            for (size_t i = 0; i < 2 * n_; ++i)
            {
                uint64_t xq = (x_[i] >> q) & 1;
                uint64_t zq = (z_[i] >> q) & 1;
                r_[i] ^= static_cast<uint8_t>(xq & zq);
                z_[i] ^= xq << q;
            }
        }

        void apply_x(size_t q)
        {
            apply_h(q);
            apply_s(q);
            apply_s(q);
            apply_h(q);
        }

        void apply_z(size_t q)
        {
            apply_s(q);
            apply_s(q);
        }

        void apply_y(size_t q)
        {
            apply_z(q);
            apply_x(q);
        }

        int measure(size_t q, std::mt19937 &rng)
        {
            int p = -1;
            for (size_t i = n_; i < 2 * n_; ++i)
            {
                if ((x_[i] >> q) & 1)
                {
                    p = static_cast<int>(i);
                    break;
                }
            }

            if (p == -1)
            {
                int result = 0;
                for (size_t i = 0; i < n_; ++i)
                {
                    if ((x_[i] >> q) & 1)
                    {
                        result = r_[i];
                        break;
                    }
                }
                return result & 1;
            }

            int result = static_cast<int>(rng() & 1);

            for (size_t i = n_; i < 2 * n_; ++i)
            {
                if (static_cast<int>(i) != p && ((x_[i] >> q) & 1))
                    rowsum(static_cast<size_t>(p), i);
            }
            for (size_t k = 0; k < n_; ++k)
            {
                x_[p - n_] = (k == q) ? 1 : 0;
            }
            uint64_t xq = 1ULL << q;
            x_[p] = xq;
            z_[p] = 0;
            r_[p] = static_cast<uint8_t>(result);
            x_[p - n_] = xq;
            z_[p - n_] = 0;
            r_[p - n_] = static_cast<uint8_t>(result);

            return result;
        }
    };

    class LimTDD
    {
    private:
        struct Node
        {
            int var = -1;
            std::shared_ptr<Node> low = nullptr;
            std::shared_ptr<Node> high = nullptr;
            std::complex<double> weight = {1.0, 0.0};
        };

        std::shared_ptr<Node> root_;
        size_t num_qubits_ = 0;

        std::shared_ptr<Node> term(std::complex<double> w)
        {
            if (std::norm(w) < 1e-18)
                return nullptr;
            auto t = std::make_shared<Node>();
            t->var = -1;
            t->weight = w;
            return t;
        }

        struct NodeKey
        {
            int var;
            Node *low;
            Node *high;
            bool operator==(const NodeKey &o) const
            {
                return var == o.var && low == o.low && high == o.high;
            }
        };

        struct NodeKeyHash
        {
            size_t operator()(const NodeKey &k) const
            {
                size_t h = std::hash<int>{}(k.var);
                h = h * 31 + std::hash<Node *>{}(k.low);
                h = h * 31 + std::hash<Node *>{}(k.high);
                return h;
            }
        };

        std::unordered_map<NodeKey, std::shared_ptr<Node>, NodeKeyHash> unique_;

        std::shared_ptr<Node> make(int var, std::shared_ptr<Node> lo, std::shared_ptr<Node> hi)
        {
            if (lo == hi)
                return lo;
            NodeKey key{var, lo.get(), hi.get()};
            auto it = unique_.find(key);
            if (it != unique_.end())
                return it->second;
            auto n = std::make_shared<Node>();
            n->var = var;
            n->low = lo;
            n->high = hi;
            n->weight = {1.0, 0.0};
            unique_[key] = n;
            return n;
        }

        std::shared_ptr<Node> scale(std::complex<double> w, std::shared_ptr<Node> n)
        {
            if (!n)
                return nullptr;
            if (n->var == -1)
                return term(n->weight * w);
            return make(n->var, scale(w, n->low), scale(w, n->high));
        }

        std::shared_ptr<Node> add(std::complex<double> w1, std::shared_ptr<Node> n1,
                                  std::complex<double> w2, std::shared_ptr<Node> n2)
        {
            if (!n1) return scale(w2, n2);
            if (!n2) return scale(w1, n1);
            if (n1->var == n2->var)
            {
                if (n1->var == -1)
                    return term(n1->weight * w1 + n2->weight * w2);
                return make(n1->var,
                            add(w1, n1->low, w2, n2->low),
                            add(w1, n1->high, w2, n2->high));
            }
            if (n1->var < n2->var)
                return make(n1->var,
                            add(w1, n1->low, w2, n2),
                            add(w1, n1->high, w2, n2));
            return make(n2->var,
                        add(w1, n1, w2, n2->low),
                        add(w1, n1, w2, n2->high));
        }

        std::shared_ptr<Node> apply_gate_rec(std::shared_ptr<Node> n, size_t q,
                                             std::complex<double> a, std::complex<double> b,
                                             std::complex<double> c, std::complex<double> d)
        {
            if (!n)
                return nullptr;
            if (n->var == -1)
            {
                return make(static_cast<int>(q), scale(a, n), scale(c, n));
            }
            if (n->var == static_cast<int>(q))
            {
                auto nl = add(a, n->low, b, n->high);
                auto nh = add(c, n->low, d, n->high);
                return make(static_cast<int>(q), nl, nh);
            }
            if (n->var < static_cast<int>(q))
            {
                return make(n->var,
                            apply_gate_rec(n->low, q, a, b, c, d),
                            apply_gate_rec(n->high, q, a, b, c, d));
            }
            return make(static_cast<int>(q), scale(a, n), scale(c, n));
        }

        double norm2(const std::shared_ptr<Node> &n) const
        {
            if (!n) return 0.0;
            if (n->var == -1) return std::norm(n->weight);
            return norm2(n->low) + norm2(n->high);
        }

        double prob1(const std::shared_ptr<Node> &n, size_t q) const
        {
            if (!n) return 0.0;
            if (n->var == -1) return 0.0;
            if (n->var == static_cast<int>(q)) return norm2(n->high);
            return prob1(n->low, q) + prob1(n->high, q);
        }

        void extract_rec(const std::shared_ptr<Node> &n, std::vector<int> &assign,
                         std::complex<double> w, std::vector<std::complex<double>> &out) const
        {
            if (!n) return;
            if (n->var == -1)
            {
                int idx = 0;
                for (int q = 0; q < static_cast<int>(assign.size()); ++q)
                    if (assign[q]) idx |= 1 << q;
                out[idx] += w * n->weight;
                return;
            }
            assign[n->var] = 0;
            extract_rec(n->low, assign, w, out);
            assign[n->var] = 1;
            extract_rec(n->high, assign, w, out);
        }

        void extract(std::vector<std::complex<double>> &out) const
        {
            out.assign(size_t(1) << num_qubits_, {0.0, 0.0});
            std::vector<int> assign(num_qubits_, 0);
            extract_rec(root_, assign, {1.0, 0.0}, out);
        }

        std::shared_ptr<Node> build_rec(const std::vector<std::complex<double>> &amps,
                                        size_t offset, size_t stride, int q, int n)
        {
            if (q >= n)
                return term(amps[offset]);
            auto lo = build_rec(amps, offset, stride * 2, q + 1, n);
            auto hi = build_rec(amps, offset + stride, stride * 2, q + 1, n);
            return make(q, lo, hi);
        }

        void rebuild(const std::vector<std::complex<double>> &amps)
        {
            unique_.clear();
            root_ = build_rec(amps, 0, 1, 0, static_cast<int>(num_qubits_));
        }

        void amplitude_two_qubit(size_t c, size_t t, bool flip_on_one, bool flip_on_both)
        {
            std::vector<std::complex<double>> amps;
            extract(amps);
            size_t N = amps.size();
            for (size_t idx = 0; idx < N; ++idx)
            {
                bool control = ((idx >> c) & 1) != 0;
                bool should = flip_on_both ? control : (flip_on_one && control);
                if (should)
                {
                    size_t j = idx ^ (size_t(1) << t);
                    if (idx < j)
                        std::swap(amps[idx], amps[j]);
                }
            }
            rebuild(amps);
        }

    public:
        LimTDD() : root_(term({1.0, 0.0})) {}

        void allocate(size_t n)
        {
            num_qubits_ = n;
            unique_.clear();
            std::vector<std::complex<double>> amps(size_t(1) << n, {0.0, 0.0});
            amps[0] = {1.0, 0.0};
            rebuild(amps);
        }

        void apply_tensor_contraction() { normalize(); }

        void normalize()
        {
            double n2 = norm2(root_);
            if (n2 > 1e-15)
                root_ = scale({1.0 / std::sqrt(n2), 0.0}, root_);
        }

        void slice(uint32_t q)
        {
            if (q >= num_qubits_) return;
            root_ = apply_gate_rec(root_, q, {1, 0}, {0, 0}, {0, 0}, {0, 0});
        }

        void apply_gate(size_t q, std::complex<double> a, std::complex<double> b,
                        std::complex<double> c, std::complex<double> d)
        {
            root_ = apply_gate_rec(root_, q, a, b, c, d);
        }

        void apply_x(size_t q) { apply_gate(q, {0, 0}, {1, 0}, {1, 0}, {0, 0}); }
        void apply_z(size_t q) { apply_gate(q, {1, 0}, {0, 0}, {0, 0}, {-1, 0}); }
        void apply_h(size_t q)
        {
            const double inv = 1.0 / std::sqrt(2.0);
            apply_gate(q, {inv, 0}, {inv, 0}, {inv, 0}, {-inv, 0});
        }
        void apply_rz(size_t q, double angle)
        {
            std::complex<double> p0(std::cos(angle / 2), -std::sin(angle / 2));
            std::complex<double> p1(std::cos(angle / 2), std::sin(angle / 2));
            apply_gate(q, p0, {0, 0}, {0, 0}, p1);
        }
        void apply_cnot(size_t c, size_t t) { amplitude_two_qubit(c, t, true, false); }
        void apply_swap(size_t a, size_t b)
        {
            std::vector<std::complex<double>> amps;
            extract(amps);
            size_t N = amps.size();
            for (size_t idx = 0; idx < N; ++idx)
            {
                size_t j = idx;
                if (((idx >> a) & 1) != ((idx >> b) & 1))
                    j = idx ^ (size_t(1) << a) ^ (size_t(1) << b);
                if (idx < j)
                    std::swap(amps[idx], amps[j]);
            }
            rebuild(amps);
        }
        void apply_toffoli(size_t c1, size_t c2, size_t t)
        {
            std::vector<std::complex<double>> amps;
            extract(amps);
            size_t N = amps.size();
            for (size_t idx = 0; idx < N; ++idx)
            {
                if (((idx >> c1) & 1) && ((idx >> c2) & 1))
                {
                    size_t j = idx ^ (size_t(1) << t);
                    if (idx < j)
                        std::swap(amps[idx], amps[j]);
                }
            }
            rebuild(amps);
        }

        int measure(size_t q, std::mt19937 &rng)
        {
            double p1 = prob1(root_, q);
            double total = norm2(root_);
            double pr = total > 1e-15 ? p1 / total : 0.0;
            std::uniform_real_distribution<double> dist(0.0, 1.0);
            int result = (dist(rng) < pr) ? 1 : 0;
            if (result == 0)
                root_ = apply_gate_rec(root_, q, {1, 0}, {0, 0}, {0, 0}, {0, 0});
            else
                root_ = apply_gate_rec(root_, q, {0, 0}, {0, 0}, {1, 0}, {0, 0});
            normalize();
            return result;
        }

        size_t num_qubits() const { return num_qubits_; }
    };

    enum class ZXColor { Z, X, Hadamard, Boundary };

    struct ZXNode
    {
        size_t id = 0;
        ZXColor color = ZXColor::Boundary;
        double phase = 0.0;
        std::vector<size_t> neighbors;
    };

    class QuantumCircuitCache
    {
    private:
        std::unordered_map<size_t, ZXNode> graph_;
        std::unordered_map<std::string, std::string> cache_;
        size_t next_id_ = 0;

        size_t fresh(ZXColor c, double phase)
        {
            ZXNode n;
            n.id = next_id_++;
            n.color = c;
            n.phase = phase;
            graph_[n.id] = n;
            return n.id;
        }

        void connect(size_t a, size_t b)
        {
            graph_[a].neighbors.push_back(b);
            graph_[b].neighbors.push_back(a);
        }

        void remove_node(size_t id)
        {
            for (auto nb : graph_[id].neighbors)
            {
                auto &v = graph_[nb].neighbors;
                v.erase(std::remove(v.begin(), v.end(), id), v.end());
            }
            graph_.erase(id);
        }

        std::string serialize() const
        {
            std::stringstream ss;
            std::vector<size_t> ids;
            for (const auto &kv : graph_) ids.push_back(kv.first);
            std::sort(ids.begin(), ids.end());
            for (auto id : ids)
            {
                const auto &n = graph_.at(id);
                ss << id << ":" << static_cast<int>(n.color) << ":"
                   << std::fixed << std::setprecision(4) << n.phase << "[";
                std::vector<size_t> nb = n.neighbors;
                std::sort(nb.begin(), nb.end());
                for (auto m : nb) ss << m << ",";
                ss << "];";
            }
            return ss.str();
        }

    public:
        QuantumCircuitCache() = default;

        void build_from_gates(const std::vector<std::pair<std::string, std::vector<size_t>>> &gates)
        {
            graph_.clear();
            next_id_ = 0;
            for (const auto &[name, qs] : gates)
            {
                if (name == "H")
                {
                    size_t h = fresh(ZXColor::Hadamard, 0.0);
                    size_t b = fresh(ZXColor::Boundary, 0.0);
                    connect(h, b);
                }
                else if (name == "Z" || name == "S" || name == "T")
                {
                    double ph = (name == "T") ? M_PI / 4 : (name == "S") ? M_PI_2 : M_PI;
                    size_t s = fresh(ZXColor::Z, ph);
                    size_t b = fresh(ZXColor::Boundary, 0.0);
                    connect(s, b);
                }
                else
                {
                    size_t s = fresh(ZXColor::X, 0.0);
                    size_t b = fresh(ZXColor::Boundary, 0.0);
                    connect(s, b);
                }
            }
        }

        std::string apply_zx_calculus_reduction()
        {
            bool changed = true;
            while (changed)
            {
                changed = false;

                for (auto it = graph_.begin(); it != graph_.end() && !changed;)
                {
                    size_t id = it->first;
                    ZXNode &n = it->second;
                    if (n.color == ZXColor::Boundary || n.color == ZXColor::Hadamard)
                    {
                        ++it;
                        continue;
                    }
                    for (size_t nb : n.neighbors)
                    {
                        auto f = graph_.find(nb);
                        if (f != graph_.end() && f->second.color == n.color)
                        {
                            n.phase = std::fmod(n.phase + f->second.phase, 2 * M_PI);
                            std::vector<size_t> others;
                            for (auto m : f->second.neighbors)
                                if (m != id) others.push_back(m);
                            remove_node(nb);
                            for (auto m : others)
                                connect(id, m);
                            changed = true;
                            break;
                        }
                    }
                    if (!changed) ++it;
                }
                if (changed) continue;

                for (auto it = graph_.begin(); it != graph_.end() && !changed;)
                {
                    size_t id = it->first;
                    ZXNode &n = it->second;
                    if (n.color != ZXColor::Boundary && n.phase == 0.0 && n.neighbors.size() == 2)
                    {
                        size_t a = n.neighbors[0], b = n.neighbors[1];
                        remove_node(id);
                        connect(a, b);
                        changed = true;
                        break;
                    }
                    ++it;
                }
                if (changed) continue;

                for (auto it = graph_.begin(); it != graph_.end() && !changed;)
                {
                    size_t id = it->first;
                    if (graph_[id].color == ZXColor::Hadamard)
                    {
                        for (size_t nb : graph_[id].neighbors)
                        {
                            auto f = graph_.find(nb);
                            if (f != graph_.end() && f->second.color == ZXColor::Hadamard)
                            {
                                size_t a = id, b = nb;
                                size_t a_other = 0, b_other = 0;
                                for (auto m : graph_[a].neighbors) if (m != b) a_other = m;
                                for (auto m : graph_[b].neighbors) if (m != a) b_other = m;
                                remove_node(a);
                                remove_node(b);
                                connect(a_other, b_other);
                                changed = true;
                                break;
                            }
                        }
                    }
                    ++it;
                }
            }
            return serialize();
        }

        std::string weisfeiler_leman_hash()
        {
            std::unordered_map<size_t, size_t> label;
            std::hash<std::string> h;
            for (const auto &[id, n] : graph_)
            {
                std::stringstream s;
                s << static_cast<int>(n.color) << "_" << n.neighbors.size();
                label[id] = h(s.str());
            }
            for (int iter = 0; iter < 5; ++iter)
            {
                std::unordered_map<size_t, size_t> next;
                bool changed = false;
                for (const auto &[id, n] : graph_)
                {
                    std::vector<size_t> nb;
                    for (auto m : n.neighbors) nb.push_back(label[m]);
                    std::sort(nb.begin(), nb.end());
                    std::stringstream s;
                    s << label[id] << "|";
                    for (auto l : nb) s << l << ",";
                    size_t nh = h(s.str());
                    next[id] = nh;
                    if (nh != label[id]) changed = true;
                }
                label = next;
                if (!changed) break;
            }
            std::vector<size_t> vals;
            for (const auto &[id, l] : label) vals.push_back(l);
            std::sort(vals.begin(), vals.end());
            std::stringstream out;
            out << "WL_";
            for (auto l : vals) out << std::hex << l;
            return out.str();
        }

        bool lookup(const std::string &circuit_key)
        {
            return cache_.find(circuit_key) != cache_.end();
        }
    };

    struct InteractionGate
    {
        int control_qubit;
        int target_qubit;
        double interaction_strength;
    };

    class IsoQGNN
    {
    private:
        std::vector<double> scalar_features_;
        std::vector<std::array<double, 3>> coords_;
        std::vector<std::vector<size_t>> adj_;
        std::vector<double> W_s_;
        std::vector<double> W_v_;
        std::unordered_map<int, int> atom_to_qubit_;
        std::vector<InteractionGate> schedule_;

    public:
        IsoQGNN(size_t hidden = 64)
        {
            W_s_.assign(hidden, 0.05);
            W_v_.assign(hidden, 0.05);
        }

        void map_molecular_geometry(const std::vector<std::pair<int, int>> &chemical_bonds)
        {
            atom_to_qubit_.clear();
            schedule_.clear();
            int qubit = 0;
            for (const auto &bond : chemical_bonds)
            {
                if (!atom_to_qubit_.count(bond.first)) atom_to_qubit_[bond.first] = qubit++;
                if (!atom_to_qubit_.count(bond.second)) atom_to_qubit_[bond.second] = qubit++;
            }
            size_t N = atom_to_qubit_.size();
            scalar_features_.assign(N, 1.0);
            coords_.assign(N, {0.0, 0.0, 0.0});
            adj_.assign(N, {});
            for (const auto &bond : chemical_bonds)
            {
                size_t a = atom_to_qubit_[bond.first];
                size_t b = atom_to_qubit_[bond.second];
                adj_[a].push_back(b);
                adj_[b].push_back(a);
            }
        }

        void message_passing(size_t rounds = 3)
        {
            for (size_t r = 0; r < rounds; ++r)
            {
                auto ns = scalar_features_;
                auto nc = coords_;
                for (size_t i = 0; i < scalar_features_.size(); ++i)
                {
                    double s_acc = 0.0;
                    std::array<double, 3> v_acc = {0.0, 0.0, 0.0};
                    for (auto j : adj_[i])
                    {
                        s_acc += scalar_features_[j];
                        std::array<double, 3> d = {
                            coords_[j][0] - coords_[i][0],
                            coords_[j][1] - coords_[i][1],
                            coords_[j][2] - coords_[i][2]};
                        double dist = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]) + 1e-9;
                        for (int k = 0; k < 3; ++k) v_acc[k] += d[k] / dist;
                    }
                    ns[i] = scalar_features_[i] + 0.1 * s_acc;
                    for (int k = 0; k < 3; ++k) nc[i][k] = coords_[i][k] + 0.1 * v_acc[k];
                }
                scalar_features_ = ns;
                coords_ = nc;
            }
        }

        std::vector<InteractionGate> schedule(const std::vector<std::pair<int, int>> &chemical_bonds)
        {
            schedule_.clear();
            message_passing();
            for (const auto &bond : chemical_bonds)
            {
                int qc = atom_to_qubit_[bond.first];
                int qt = atom_to_qubit_[bond.second];
                double w = std::abs(scalar_features_[qc] + scalar_features_[qt]) * 0.5;
                schedule_.push_back({qc, qt, w});
            }
            return schedule_;
        }
    };

    class VirtualizationEngine
    {
    private:
        std::vector<size_t> injected_magic_qubits_;
        double magic_fidelity_ = 1.0;

    public:
        void decompose_magic_states(size_t target_qubit)
        {
            injected_magic_qubits_.push_back(target_qubit);
            magic_fidelity_ *= (1.0 - 1e-3);
        }

        size_t magic_state_count() const { return injected_magic_qubits_.size(); }
        double fidelity() const { return magic_fidelity_; }
    };

    class ErrorBudgetGame
    {
    private:
        struct CompilationModule
        {
            std::string name;
            double weight;
            double current_tolerance;
            double utility(double tol) const { return weight * std::log(tol * 1e12); }
        };
        std::vector<CompilationModule> modules_;

    public:
        ErrorBudgetGame()
        {
            modules_ = {
                {"Logical_Operation", 1.0, 0.0},
                {"T_State_Distillation", 2.5, 0.0},
                {"Rotation_Synthesis", 1.2, 0.0}};
        }

        void execute_ibr_allocation(double total_budget)
        {
            if (modules_.empty()) return;
            double total_weight = 0.0;
            for (auto &m : modules_) total_weight += m.weight;

            for (auto &m : modules_) m.current_tolerance = total_budget / modules_.size();

            const double epsilon = 1e-9;
            for (int iter = 0; iter < 200; ++iter)
            {
                bool converged = true;
                for (size_t i = 0; i < modules_.size(); ++i)
                {
                    double others = 0.0;
                    for (size_t j = 0; j < modules_.size(); ++j)
                        if (j != i) others += modules_[j].current_tolerance;
                    double available = total_budget - others;
                    double best = total_budget * (modules_[i].weight / total_weight);
                    double next = std::min(available, best);
                    if (next < 1e-12) next = 1e-12;
                    if (std::abs(next - modules_[i].current_tolerance) > epsilon)
                        converged = false;
                    modules_[i].current_tolerance = next;
                }
                if (converged) break;
            }
        }

        double tolerance(const std::string &name) const
        {
            for (auto &m : modules_) if (m.name == name) return m.current_tolerance;
            return 0.0;
        }
    };

    class KarmaQVPU : public IQuantumBackend
    {
    private:
        LimTDD virtualized_state;
        QuantumCircuitCache qcc;
        IsoQGNN qgnn;
        VirtualizationEngine gv_engine;
        ErrorBudgetGame budget_allocator;

        size_t physical_qubit_limit = 45000;
        size_t active_qubits = 0;
        std::vector<bool> is_allocated_;
        std::vector<bool> is_locked_;
        std::mt19937 rng_{std::random_device{}()};

    public:
        KarmaQVPU()
        {
            budget_allocator.execute_ibr_allocation(1.0);
        }

        void allocate_qubits(size_t num_qubits) override
        {
            active_qubits = num_qubits;
            virtualized_state.allocate(num_qubits);
            is_allocated_.assign(num_qubits, true);
            is_locked_.assign(num_qubits, false);
        }

        void release_qubit(size_t qubit_id) override
        {
            if (qubit_id < is_allocated_.size())
                is_allocated_[qubit_id] = false;
        }

        void lock_hardware_id(size_t qubit_id) override
        {
            if (qubit_id < is_locked_.size())
                is_locked_[qubit_id] = true;
        }

        void unlock_hardware_id(size_t qubit_id) override
        {
            if (qubit_id < is_locked_.size())
                is_locked_[qubit_id] = false;
        }

        int measure(size_t qubit_id) override
        {
            return virtualized_state.measure(qubit_id, rng_);
        }

        void apply_x(size_t qubit_id) override
        {
            virtualized_state.apply_x(qubit_id);
        }

        void apply_h(size_t qubit_id) override
        {
            virtualized_state.apply_h(qubit_id);
        }

        void apply_rz(size_t qubit_id, double angle) override
        {
            virtualized_state.apply_rz(qubit_id, angle);
        }

        void apply_cnot(size_t control, size_t target) override
        {
            virtualized_state.apply_cnot(control, target);
        }

        void apply_toffoli(size_t control1, size_t control2, size_t target) override
        {
            virtualized_state.apply_toffoli(control1, control2, target);
            gv_engine.decompose_magic_states(target);
        }
    };
}