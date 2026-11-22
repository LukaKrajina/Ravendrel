#pragma once
#include <vector>
#include <cstdint>
#include <complex>
#include <random>
#include <cmath>
#include <memory>
#include <algorithm>
#include <stdexcept>
#include "Karma.hpp"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#ifndef M_PI_2
#define M_PI_2 1.5707963267948966
#endif

namespace qhal
{

    struct Spin1State
    {
        std::complex<double> amplitudes[3];
    };

    class TeleportationProtocol
    {
    private:
        double active_latency_ms = 0.0;
        int circuit_depth_reduction = 0;
        IQuantumBackend *backend_ = nullptr;

    public:
        void set_backend(IQuantumBackend *be) { backend_ = be; }

        std::pair<int, int> teleport(size_t state_qubit, size_t ancilla, size_t target_qubit)
        {
            if (!backend_)
                throw std::runtime_error("[KarmaBus] teleport: no backend attached.");
            backend_->apply_h(ancilla);
            backend_->apply_cnot(ancilla, target_qubit);
            backend_->apply_cnot(state_qubit, ancilla);
            backend_->apply_h(state_qubit);
            int m1 = backend_->measure(state_qubit);
            int m2 = backend_->measure(ancilla);
            if (m2 == 1) backend_->apply_x(target_qubit);
            if (m1 == 1) backend_->apply_z(target_qubit);
            return {m1, m2};
        }

        Spin1State execute_spin1_qutrit_teleportation(Spin1State &target_qutrit)
        {
            const std::complex<double> w = std::polar(1.0, 2.0 * M_PI / 3.0);
            const double inv3 = 1.0 / std::sqrt(3.0);
            Spin1State entangled = {
                inv3 * (target_qutrit.amplitudes[0] + target_qutrit.amplitudes[1] + target_qutrit.amplitudes[2]),
                inv3 * (target_qutrit.amplitudes[0] + w * target_qutrit.amplitudes[1] + std::pow(w, 2) * target_qutrit.amplitudes[2]),
                inv3 * (target_qutrit.amplitudes[0] + std::pow(w, 2) * target_qutrit.amplitudes[1] + w * target_qutrit.amplitudes[2])};
            target_qutrit = entangled;
            return entangled;
        }

        void execute_two_way_teleportation(size_t node_a, size_t node_b)
        {
            const size_t hop = (node_a > node_b ? node_a - node_b : node_b - node_a);
            const double factor = 1.0 / (1.0 + 0.25 * static_cast<double>(hop + 1));
            active_latency_ms *= factor;
            circuit_depth_reduction += 1;
        }

        double latency_ms() const { return active_latency_ms; }
        int depth_reduction() const { return circuit_depth_reduction; }
    };

    class PuncturedQECC
    {
    public:
        struct StabilizerCode
        {
            size_t n_physical = 7;
            size_t k_logical = 1;
            size_t d_distance = 3;
            std::vector<uint64_t> X = {
                0x6A,
                0x35,
                0x4E,
            };
            std::vector<uint64_t> Z = {
                0x6A, 0x35, 0x4E,
            };
        };

    private:
        StabilizerCode code_;
        std::vector<bool> punctured_;
        double critical_latency_threshold_ = 2.5;

        static uint64_t remove_bit(uint64_t mask, size_t pos)
        {
            uint64_t low = mask & ((1ULL << pos) - 1);
            uint64_t high = (mask >> (pos + 1)) << pos;
            return low | high;
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
        PuncturedQECC()
        {
            punctured_.assign(code_.n_physical, false);
        }

        const StabilizerCode &code() const { return code_; }

        void set_critical_latency_threshold(double ms) { critical_latency_threshold_ = ms; }
        double critical_latency_threshold() const { return critical_latency_threshold_; }

        void puncture(size_t position)
        {
            if (position >= code_.n_physical || punctured_[position])
                return;
            punctured_[position] = true;

            auto prune = [&](std::vector<uint64_t> &gens)
            {
                std::vector<uint64_t> kept;
                for (auto g : gens)
                {
                    if ((g >> position) & 1)
                        continue;
                    kept.push_back(remove_bit(g, position));
                }
                gens = kept;
            };
            prune(code_.X);
            prune(code_.Z);

            code_.n_physical -= 1;
            if (code_.d_distance > 1)
                code_.d_distance -= 1;
        }

        void apply_dynamic_puncturing(double real_time_latency_ms)
        {
            if (real_time_latency_ms > critical_latency_threshold_ && code_.n_physical > 1)
            {
                size_t pos = 0;
                while (pos < punctured_.size() && punctured_[pos]) ++pos;
                if (pos < punctured_.size())
                {
                    puncture(pos);
                }
            }
        }

        size_t min_weight() const
        {
            size_t w = SIZE_MAX;
            for (auto g : code_.X)
                if (g) w = std::min(w, static_cast<size_t>(popcount(g)));
            for (auto g : code_.Z)
                if (g) w = std::min(w, static_cast<size_t>(popcount(g)));
            return w == SIZE_MAX ? 0 : w;
        }
    };

    enum class PQKEMLevel
    {
        KYBER_512,
        FRODOKEM_1344
    };

    class QRQT_SecureChannel
    {
    private:
        PQKEMLevel active_kem;
        double memory_coherence_time_ms = 1.0;
        uint8_t received_b1 = 0;
        uint8_t received_b2 = 0;
        bool is_decoherence_free_state = false;
        std::shared_ptr<CliffordTableau> bell_;
        std::mt19937 rng_;

        void prepare_bell()
        {
            bell_->apply_h(0);
            bell_->apply_cnot(0, 1);
        }

    public:
        QRQT_SecureChannel(PQKEMLevel kem = PQKEMLevel::KYBER_512)
            : active_kem(kem), bell_(std::make_shared<CliffordTableau>(2))
        {
            rng_.seed(std::random_device{}());
            prepare_bell();
        }

        double max_distance_km() const
        {
            return (active_kem == PQKEMLevel::KYBER_512) ? 199.0 : 191.0;
        }

        void transmit_bell_basis(uint8_t bit_1, uint8_t bit_2, double fiber_distance_km)
        {
            double propagation_ms = fiber_distance_km * 0.005;
            if (fiber_distance_km > max_distance_km() || propagation_ms > memory_coherence_time_ms)
            {
                received_b1 = 0xFF;
                received_b2 = 0xFF;
                is_decoherence_free_state = false;
                return;
            }
            received_b1 = bit_1;
            received_b2 = bit_2;
            is_decoherence_free_state = (bit_1 != bit_2);
        }

        void execute_post_selection()
        {
            prepare_bell();
            int a = bell_->measure(0, rng_);
            int b = bell_->measure(1, rng_);
            bool bell_correlated = (a == b);
            if (!is_decoherence_free_state || !bell_correlated)
            {
                received_b1 = 0;
                received_b2 = 0;
                is_decoherence_free_state = false;
            }
        }

        bool accepted() const { return is_decoherence_free_state; }
        uint8_t bit1() const { return received_b1; }
        uint8_t bit2() const { return received_b2; }
    };

    struct ClientJob
    {
        size_t client_id;
        size_t requested_qubits;
        std::vector<size_t> allocated_nodes;
        size_t required_teleportations;
        double utility_score;
    };

    class QCPRAGM_Allocator
    {
    private:
        std::vector<ClientJob> network_clients;
        std::mt19937 rng;
        std::vector<std::vector<double>> latency_matrix;
        const double EPR_GENERATION_TIME_MS = 0.5;
        const double TWO_WAY_TELEPORTATION_MULTIPLIER = 0.60;
        const double LATENCY_COST_WEIGHT = 1.5;

    public:
        QCPRAGM_Allocator(size_t total_network_nodes = 100)
        {
            rng.seed(std::random_device{}());
            latency_matrix.resize(total_network_nodes, std::vector<double>(total_network_nodes, 0.0));
            for (size_t i = 0; i < total_network_nodes; ++i)
                for (size_t j = 0; j < total_network_nodes; ++j)
                    if (i != j)
                        latency_matrix[i][j] = 0.1 + std::abs(static_cast<int>(i - j)) * 0.05;
        }

        void register_client(size_t id, size_t qubits, const std::vector<size_t> &nodes, size_t teleports)
        {
            network_clients.push_back({id, qubits, nodes, teleports, 0.0});
        }

        double calculate_dynamic_latency_cost(const ClientJob &job) const
        {
            if (job.allocated_nodes.empty() || job.required_teleportations == 0)
                return 0.0;
            double raw = 0.0;
            for (size_t i = 0; i + 1 < job.allocated_nodes.size(); ++i)
            {
                size_t a = job.allocated_nodes[i];
                size_t b = job.allocated_nodes[i + 1];
                if (a < latency_matrix.size() && b < latency_matrix.size())
                    raw += latency_matrix[a][b];
            }
            double epr = job.required_teleportations * EPR_GENERATION_TIME_MS;
            return (raw + epr) * TWO_WAY_TELEPORTATION_MULTIPLIER * LATENCY_COST_WEIGHT;
        }

        void compute_nash_equilibrium()
        {
            if (network_clients.empty()) return;
            const double epsilon = 1e-4;
            for (int iter = 0; iter < 50; ++iter)
            {
                bool converged = true;
                for (auto &client : network_clients)
                {
                    double cost = calculate_dynamic_latency_cost(client);
                    double best = client.requested_qubits * 2.5 - cost;
                    if (std::abs(best - client.utility_score) > epsilon)
                        converged = false;
                    client.utility_score = best;
                }
                if (converged) break;
            }
        }

        void execute_bayesian_mean_field_approximation()
        {
            std::normal_distribution<double> noise(0.0, 0.1);
            bool conflict = false;
            for (size_t i = 0; i + 1 < network_clients.size(); ++i)
            {
                if (std::abs(network_clients[i].utility_score - network_clients[i + 1].utility_score) < 0.05)
                {
                    conflict = true;
                    break;
                }
            }
            if (conflict)
                for (auto &c : network_clients)
                    c.utility_score += noise(rng);
        }

        const std::vector<ClientJob> &clients() const { return network_clients; }
    };

    struct LogicNode
    {
        std::string gate_op;
        std::vector<size_t> qubit_dependencies;
    };

    class TimeAwarePartitioner
    {
    public:
        struct PartitionResult
        {
            std::vector<size_t> node_assignment;
            size_t num_partitions = 1;
            size_t cut_edges = 0;
        };

        PartitionResult execute_beam_search_lightcone(const std::vector<LogicNode> &dependency_graph)
        {
            PartitionResult res;
            if (dependency_graph.empty())
                return res;

            size_t num_qubits = 0;
            for (const auto &node : dependency_graph)
                for (size_t q : node.qubit_dependencies)
                    num_qubits = std::max(num_qubits, q + 1);

            std::vector<size_t> last_owner(num_qubits, 0);
            res.node_assignment.assign(dependency_graph.size(), 0);
            size_t num_partitions = 1;

            for (size_t i = 0; i < dependency_graph.size(); ++i)
            {
                const auto &op = dependency_graph[i];
                if (op.qubit_dependencies.empty())
                {
                    res.node_assignment[i] = 0;
                    continue;
                }
                size_t owner = last_owner[op.qubit_dependencies[0]];
                bool split = false;
                for (size_t q : op.qubit_dependencies)
                {
                    if (last_owner[q] != owner)
                        split = true;
                }
                if (split)
                {
                    owner = num_partitions++;
                    res.cut_edges += op.qubit_dependencies.size() - 1;
                }
                res.node_assignment[i] = owner;
                for (size_t q : op.qubit_dependencies)
                    last_owner[q] = owner;
            }
            res.num_partitions = num_partitions;
            return res;
        }
    };

    class KarmaBusQVPL
    {
    private:
        TeleportationProtocol teleporter;
        PuncturedQECC qecc;
        QRQT_SecureChannel crypto_channel;
        QCPRAGM_Allocator resource_game;
        TimeAwarePartitioner partitioner;

    public:
        KarmaBusQVPL()
        {
            crypto_channel = QRQT_SecureChannel(PQKEMLevel::KYBER_512);
        }

        void set_backend(IQuantumBackend *be) { teleporter.set_backend(be); }

        void establish_link(size_t node_a, size_t node_b, double latency_metric)
        {
            resource_game.compute_nash_equilibrium();
            resource_game.execute_bayesian_mean_field_approximation();
            qecc.apply_dynamic_puncturing(latency_metric);
            teleporter.execute_two_way_teleportation(node_a, node_b);
        }

        void secure_classical_transmit(uint8_t b1, uint8_t b2, double distance_km)
        {
            crypto_channel.transmit_bell_basis(b1, b2, distance_km);
            crypto_channel.execute_post_selection();
        }

        TimeAwarePartitioner::PartitionResult partition_distributed_circuit(const std::vector<LogicNode> &dag)
        {
            return partitioner.execute_beam_search_lightcone(dag);
        }

        const PuncturedQECC::StabilizerCode &qec_code() const { return qecc.code(); }
    };
}