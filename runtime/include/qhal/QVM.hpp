#pragma once
#include <vector>
#include <complex>
#include <math.h>
#include <random>
#include <iostream>
#include <stdexcept>
#include <algorithm>
#include <memory>
#include <unordered_map>
#include <array>
#include <functional>
#include <mutex>
#include "IQuantumBackend.hpp"
#include "v/Karma.hpp"
#include "v/KarmaBus.hpp"
#include "v/MpsTensor.hpp"
#include "v/ThermalSimulation.hpp"
#include "../utils/Ga.hpp"
#include "../utils/NumericHealth.hpp"
namespace qhal
{
    enum class BackendExecutionPolicy
    {
        LIMDD_Compressed,
        Polyhedral_Graph,
        Dense_StateVector,
        Tensor_Network_MPS
    };

    struct MemoryGuard
    {
        static constexpr size_t DENSE_MAX_QUBITS = 20;

        static size_t calculate_dense_bytes(size_t num_qubits, size_t max_gates, size_t prec_bytes = 16)
        {
            size_t state_size = (1ULL << num_qubits) * prec_bytes;
            size_t gate_workspace = max_gates * 16 * prec_bytes;
            return state_size + gate_workspace;
        }

        static bool should_use_dense(size_t num_qubits, size_t available_vram_bytes)
        {
            return num_qubits <= DENSE_MAX_QUBITS &&
                   calculate_dense_bytes(num_qubits, 100) <= available_vram_bytes;
        }

        static BackendExecutionPolicy select_optimal_policy(size_t num_qubits, bool contains_magic_states, size_t available_vram_bytes)
        {
            if (!contains_magic_states)
            {
                return BackendExecutionPolicy::Polyhedral_Graph;
            }

            if (should_use_dense(num_qubits, available_vram_bytes))
            {
                return BackendExecutionPolicy::Dense_StateVector;
            }

            return BackendExecutionPolicy::Tensor_Network_MPS;
        }
    };

    struct Multivector3D
    {
        alignas(64) std::array<double, 8> blades;

        Multivector3D operator*(const Multivector3D &v) const
        {
            Multivector3D res;
            const auto &u = blades;
            const auto &w = v.blades;

            res.blades[0] = u[0] * w[0] + u[1] * w[1] + u[2] * w[2] + u[3] * w[3] - u[4] * w[4] - u[5] * w[5] - u[6] * w[6] - u[7] * w[7];
            res.blades[1] = u[0] * w[1] + u[1] * w[0] - u[2] * w[4] + u[3] * w[6] + u[4] * w[2] - u[5] * w[7] - u[6] * w[3] - u[7] * w[5];
            res.blades[2] = u[0] * w[2] + u[1] * w[4] + u[2] * w[0] - u[3] * w[5] - u[4] * w[1] + u[5] * w[3] - u[6] * w[7] - u[7] * w[6];
            res.blades[3] = u[0] * w[3] - u[1] * w[6] + u[2] * w[5] + u[3] * w[0] - u[4] * w[7] - u[5] * w[2] + u[6] * w[1] - u[7] * w[4];
            res.blades[4] = u[0] * w[4] + u[1] * w[2] - u[2] * w[1] + u[3] * w[7] + u[4] * w[0] - u[5] * w[6] + u[6] * w[5] + u[7] * w[3];
            res.blades[5] = u[0] * w[5] + u[1] * w[7] + u[2] * w[3] - u[3] * w[2] + u[4] * w[6] + u[5] * w[0] - u[6] * w[4] + u[7] * w[1];
            res.blades[6] = u[0] * w[6] - u[1] * w[3] + u[2] * w[7] + u[3] * w[1] - u[4] * w[5] + u[5] * w[4] + u[6] * w[0] + u[7] * w[2];
            res.blades[7] = u[0] * w[7] + u[1] * w[5] + u[2] * w[6] + u[3] * w[4] + u[4] * w[3] + u[5] * w[1] + u[6] * w[2] + u[7] * w[0];

            return res;
        }

        static Multivector3D pauli_rotor(int blade_axis, double angle)
        {
            Multivector3D r{};
            r.blades[0] = std::cos(angle / 2.0);
            r.blades[blade_axis] = -std::sin(angle / 2.0);
            return r;
        }
    };

    class SymplecticIntegrator
    {
    public:
        static void apply_symplectic_step(IQuantumBackend *backend, size_t pos_qubit, size_t mom_qubit, double eta, double lambda)
        {
            backend->apply_cnot(mom_qubit, pos_qubit);
            backend->apply_rz(pos_qubit, eta * lambda);
            backend->apply_cnot(mom_qubit, pos_qubit);
        }
    };

    enum class GradientMethod
    {
        ParameterShift,
        HadamardTest,
        DirectHadamardTest
    };

    class QADEngine
    {
    public:
        static GradientMethod select_gradient_strategy(bool is_clifford, size_t derivative_order)
        {
            if (derivative_order > 1)
            {
                return GradientMethod::HadamardTest;
            }
            if (is_clifford)
            {
                return GradientMethod::DirectHadamardTest;
            }
            return GradientMethod::ParameterShift;
        }

        static double evaluate_k_fold_hadamard(IQuantumBackend *backend, std::function<void(IQuantumBackend *)> circuit)
        {
            backend->allocate_qubits(1);
            size_t ancilla = 0;
            backend->apply_h(ancilla);
            circuit(backend);
            int m = backend->measure(ancilla);
            backend->release_qubit(ancilla);
            return (m == 0) ? 1.0 : -1.0;
        }
    };

    struct StateNode
    {
        uint32_t qmPtr_index = 0;
        std::unique_ptr<StateNode> left;
        std::unique_ptr<StateNode> right;
        bool is_leaf = false;
    };

    class AmplitudeCodebook
    {
    private:
        std::vector<std::complex<double>> centroids;
        std::vector<size_t> cluster_counts;
        std::unordered_map<uint64_t, uint32_t> spatial_grid;
        double tolerance;

        uint64_t get_grid_key(std::complex<double> amp) const
        {
            int64_t r_bin = static_cast<int64_t>(std::round(std::real(amp) / tolerance));
            int64_t i_bin = static_cast<int64_t>(std::round(std::imag(amp) / tolerance));

            return (static_cast<uint64_t>(static_cast<uint32_t>(r_bin)) << 32) |
                   static_cast<uint32_t>(i_bin);
        }

    public:
        AmplitudeCodebook(double epsilon = 1e-6) : tolerance(epsilon) {}

        uint32_t quantize_and_store(std::complex<double> amp)
        {
            uint64_t grid_key = get_grid_key(amp);
            auto it = spatial_grid.find(grid_key);

            if (it == spatial_grid.end())
            {
                uint32_t new_index = static_cast<uint32_t>(centroids.size());
                centroids.push_back(amp);
                cluster_counts.push_back(1);
                spatial_grid[grid_key] = new_index;
                return new_index;
            }

            uint32_t cluster_idx = it->second;
            size_t current_count = cluster_counts[cluster_idx];
            std::complex<double> current_centroid = centroids[cluster_idx];
            centroids[cluster_idx] = current_centroid + (amp - current_centroid) / static_cast<double>(current_count + 1);
            cluster_counts[cluster_idx]++;
            return cluster_idx;
        }

        std::complex<double> retrieve(uint32_t index) const
        {
            return centroids[index];
        }

        void print_compression_stats(size_t total_leaf_nodes) const
        {
            size_t uncompressed_bytes = total_leaf_nodes * sizeof(std::complex<double>);
            size_t compressed_bytes = total_leaf_nodes * sizeof(uint32_t) + centroids.size() * sizeof(std::complex<double>);
            std::cout << "[QMDD] Unique Centroids: " << centroids.size() << "\n";
            std::cout << "[QMDD] RAM Saved: " << (uncompressed_bytes - compressed_bytes) / 1024 / 1024 << " MB\n";
        }
        
        std::vector<std::complex<double>> merge_topk(const AmplitudeCodebook& other, size_t k) const {
            struct Ranked { size_t count; std::complex<double> amp; };
            std::vector<Ranked> all;
            all.reserve(centroids.size() + other.centroids.size());
            for (size_t i = 0; i < centroids.size(); ++i)
                all.push_back({cluster_counts[i], centroids[i]});
            for (size_t i = 0; i < other.centroids.size(); ++i)
                all.push_back({other.cluster_counts[i], other.centroids[i]});
            std::sort(all.begin(), all.end(),
                      [](const Ranked& a, const Ranked& b) { return a.count > b.count; });
            std::vector<std::complex<double>> top;
            size_t n = std::min(k, all.size());
            top.reserve(n);
            for (size_t i = 0; i < n; ++i) top.push_back(all[i].amp);
            return top;
        }
    };

    class RVDL_RT_API QVM : public IQuantumBackend
    {
    private:
        size_t num_qubits = 0;
        BackendExecutionPolicy active_policy = BackendExecutionPolicy::Polyhedral_Graph;
        bool maintains_stabilizer_polytope = true;
        size_t vram_budget = 8ULL * 1024 * 1024 * 1024;

        std::unique_ptr<StateNode> root;
        AmplitudeCodebook cb;
        std::mt19937 rng;
        std::uniform_real_distribution<double> dist;
        std::vector<bool> is_qubit_allocated;
        std::vector<bool> is_qubit_locked;

        mutable std::recursive_mutex state_mutex_;

        mutable std::vector<std::complex<double>, ga::cpu::AlignedAllocator<std::complex<double>>> dense_state;

        mps::MpsState mps_state;

        static constexpr size_t HOST_GATE_MAX_QUBITS = 16;
        using DeviceSV = ga::gpu::DeviceStateVector<Kokkos::DefaultExecutionSpace,
                                                    typename Kokkos::DefaultExecutionSpace::memory_space>;
        DeviceSV device_state_;
        mutable bool device_has_state_ = false;
        mutable bool host_synced_ = true;

        LimTDD virtualized_state;
        QuantumCircuitCache qcc;
        IsoQGNN qgnn;
        VirtualizationEngine gv_engine;
        ErrorBudgetGame budget_allocator;

        TeleportationProtocol teleporter;
        PuncturedQECC qecc;
        QRQT_SecureChannel crypto_channel;
        QCPRAGM_Allocator resource_game;
        TimeAwarePartitioner partitioner;
        
        VirtualThermalController thermal;

        void check_lock(size_t target)
        {
            if (target >= is_qubit_locked.size())
            {
                throw std::runtime_error("Hardware Error: qubit id out of range: " +
                                         std::to_string(target) + " >= " +
                                         std::to_string(is_qubit_locked.size()));
            }
            if (is_qubit_locked[target])
            {
                throw std::runtime_error("Hardware Error: Attempted to mutate a locked qubit.");
            }
        }

        void reset_qubit_to_ground(size_t qubit_id)
        {
            if (measure(qubit_id) == 1)
                apply_x(qubit_id);
        }

        void resize_state_down(size_t new_n)
        {
            if (active_policy == BackendExecutionPolicy::Dense_StateVector)
            {
                dense_state.resize(size_t(1) << new_n);
            }
            else if (active_policy == BackendExecutionPolicy::Tensor_Network_MPS)
            {
                if (mps_state.num_qubits() > new_n)
                    mps_state.drop_qubits(mps_state.num_qubits() - new_n);
            }
            else
            {
                for (size_t k = num_qubits; k > new_n; --k)
                {
                    if (root)
                        root = std::move(root->left);
                    else
                        break;
                }
                if (new_n == 0)
                    root.reset();
            }
        }

        void shrink_trailing_qubits()
        {
            size_t new_n = num_qubits;
            while (new_n > 0 && !is_qubit_allocated[new_n - 1])
                --new_n;
            if (new_n == num_qubits)
                return;

            resize_state_down(new_n);
            num_qubits = new_n;
            is_qubit_allocated.resize(new_n);
            is_qubit_locked.resize(new_n);
        }

        double calculate_norm(const StateNode *node) const
        {
            if (!node)
                return 0.0;
            if (node->is_leaf)
            {
                return std::norm(cb.retrieve(node->qmPtr_index));
            }
            return calculate_norm(node->left.get()) + calculate_norm(node->right.get());
        }

        double calculate_prob_1(const StateNode *node, int current_level, int target_level)
        {
            if (!node)
                return 0.0;

            if (current_level == target_level)
            {
                return calculate_norm(node->right.get());
            }

            return calculate_prob_1(node->left.get(), current_level - 1, target_level) +
                   calculate_prob_1(node->right.get(), current_level - 1, target_level);
        }

        void collapse_and_normalize(StateNode *node, int current_level, int target_level, int measured_val, double norm_factor)
        {
            if (!node)
                return;

            if (current_level == target_level)
            {
                if (measured_val == 1)
                {
                    node->left.reset();
                }
                else
                {
                    node->right.reset();
                }
            }

            if (node->is_leaf)
            {
                std::complex<double> old_amp = cb.retrieve(node->qmPtr_index);
                std::complex<double> new_amp = old_amp * norm_factor;
                node->qmPtr_index = cb.quantize_and_store(new_amp);
                return;
            }

            collapse_and_normalize(node->left.get(), current_level - 1, target_level, measured_val, norm_factor);
            collapse_and_normalize(node->right.get(), current_level - 1, target_level, measured_val, norm_factor);
        }

        std::complex<double> retrieve_amplitude(const StateNode *node, size_t target_state, int current_qubit_level) const
        {
            if (!node)
                return {0.0, 0.0};
            if (node->is_leaf)
            {
                return cb.retrieve(node->qmPtr_index);
            }
            size_t bit_mask = 1ULL << current_qubit_level;
            bool is_one = (target_state & bit_mask) != 0;
            if (is_one)
            {
                return retrieve_amplitude(node->right.get(), target_state, current_qubit_level - 1);
            }
            else
            {
                return retrieve_amplitude(node->left.get(), target_state, current_qubit_level - 1);
            }
        }

        std::unique_ptr<StateNode> apply_linear_combination(
            const StateNode *n0, const StateNode *n1,
            std::complex<double> alpha, std::complex<double> beta,
            int current_level)
        {
            if (current_level < 0)
            {
                std::complex<double> val0 = n0 ? cb.retrieve(n0->qmPtr_index) : std::complex<double>(0.0, 0.0);
                std::complex<double> val1 = n1 ? cb.retrieve(n1->qmPtr_index) : std::complex<double>(0.0, 0.0);
                std::complex<double> result = alpha * val0 + beta * val1;
                if (std::norm(result) < 1e-12)
                    return nullptr;
                auto leaf = std::make_unique<StateNode>();
                leaf->is_leaf = true;
                leaf->qmPtr_index = cb.quantize_and_store(result);
                return leaf;
            }

            const StateNode *n0_left = n0 ? n0->left.get() : nullptr;
            const StateNode *n0_right = n0 ? n0->right.get() : nullptr;
            const StateNode *n1_left = n1 ? n1->left.get() : nullptr;
            const StateNode *n1_right = n1 ? n1->right.get() : nullptr;

            auto left_child = apply_linear_combination(n0_left, n1_left, alpha, beta, current_level - 1);
            auto right_child = apply_linear_combination(n0_right, n1_right, alpha, beta, current_level - 1);

            if (!left_child && !right_child)
                return nullptr;

            auto node = std::make_unique<StateNode>();
            node->left = std::move(left_child);
            node->right = std::move(right_child);
            return node;
        }

        void swap_branches_for_cnot(std::unique_ptr<StateNode> &n0, std::unique_ptr<StateNode> &n1, int current_level, int control)
        {
            if (!n0 && !n1)
                return;

            if (current_level == control)
            {
                if (!n0 && n1 && n1->right)
                    n0 = std::make_unique<StateNode>();
                if (!n1 && n0 && n0->right)
                    n1 = std::make_unique<StateNode>();

                if (n0 && n1)
                {
                    std::swap(n0->right, n1->right);
                }

                if (n0 && !n0->left && !n0->right)
                    n0.reset();
                if (n1 && !n1->left && !n1->right)
                    n1.reset();
                return;
            }

            if (!n0)
                n0 = std::make_unique<StateNode>();
            if (!n1)
                n1 = std::make_unique<StateNode>();

            swap_branches_for_cnot(n0->left, n1->left, current_level - 1, control);
            swap_branches_for_cnot(n0->right, n1->right, current_level - 1, control);

            if (n0 && !n0->left && !n0->right)
                n0.reset();
            if (n1 && !n1->left && !n1->right)
                n1.reset();
        }

        void apply_x_recursive(StateNode *node, int current_qubit_level, int target_qubit)
        {
            if (!node || node->is_leaf)
                return;
            if (current_qubit_level == target_qubit)
            {
                std::swap(node->left, node->right);
                return;
            }
            apply_x_recursive(node->left.get(), current_qubit_level - 1, target_qubit);
            apply_x_recursive(node->right.get(), current_qubit_level - 1, target_qubit);
        }

        void scale_subtree(StateNode *node, std::complex<double> phase)
        {
            if (!node)
                return;
            if (node->is_leaf)
            {
                node->qmPtr_index = cb.quantize_and_store(cb.retrieve(node->qmPtr_index) * phase);
                return;
            }
            scale_subtree(node->left.get(), phase);
            scale_subtree(node->right.get(), phase);
        }

        void apply_rz_recursive(StateNode *node, int current_level, int target, std::complex<double> phase)
        {
            if (!node || node->is_leaf)
                return;
            if (current_level == target)
            {
                scale_subtree(node->right.get(), phase);
                return;
            }
            apply_rz_recursive(node->left.get(), current_level - 1, target, phase);
            apply_rz_recursive(node->right.get(), current_level - 1, target, phase);
        }

        void apply_h_recursive(std::unique_ptr<StateNode> &node, int current_level, int target_qubit)
        {
            if (!node)
                return;

            if (current_level == target_qubit)
            {
                double inv_sqrt2 = 1.0 / std::sqrt(2.0);
                auto new_left = apply_linear_combination(
                    node->left.get(), node->right.get(),
                    inv_sqrt2, inv_sqrt2,
                    current_level - 1);
                auto new_right = apply_linear_combination(
                    node->left.get(), node->right.get(),
                    inv_sqrt2, -inv_sqrt2,
                    current_level - 1);

                node->left = std::move(new_left);
                node->right = std::move(new_right);

                if (!node->left && !node->right)
                    node.reset();
                return;
            }

            apply_h_recursive(node->left, current_level - 1, target_qubit);
            apply_h_recursive(node->right, current_level - 1, target_qubit);
            if (!node->left && !node->right)
                node.reset();
        }

        void apply_basis_recursive(std::unique_ptr<StateNode> &node, int level, int target,
                                   std::complex<double> a, std::complex<double> b,
                                   std::complex<double> c, std::complex<double> d)
        {
            if (!node)
                return;

            if (level == target)
            {
                auto new_left = apply_linear_combination(node->left.get(), node->right.get(), a, b, level - 1);
                auto new_right = apply_linear_combination(node->left.get(), node->right.get(), c, d, level - 1);
                node->left = std::move(new_left);
                node->right = std::move(new_right);
                if (!node->left && !node->right)
                    node.reset();
                return;
            }

            apply_basis_recursive(node->left, level - 1, target, a, b, c, d);
            apply_basis_recursive(node->right, level - 1, target, a, b, c, d);
            if (!node->left && !node->right)
                node.reset();
        }

        void apply_cnot_recursive(std::unique_ptr<StateNode> &node, int current_level, int control, int target)
        {
            if (!node || node->is_leaf)
                return;

            if (current_level == target)
            {
                if (control < target)
                {
                    swap_branches_for_cnot(node->left, node->right, current_level - 1, control);
                }
                return;
            }

            if (current_level == control)
            {
                apply_x_recursive(node->right.get(), current_level - 1, target);
                return;
            }

            apply_cnot_recursive(node->left, current_level - 1, control, target);
            apply_cnot_recursive(node->right, current_level - 1, control, target);
        }

        void apply_two_qubit_gate_dense(size_t q1, size_t q2, const std::complex<double> U[4][4])
        {
            size_t total = dense_state.size();
            size_t m1 = 1ULL << q1, m2 = 1ULL << q2;
            size_t mask = m1 | m2;
            std::complex<double> v[4], w[4];

            for (size_t base = 0; base < total; ++base)
            {
                if (base & mask)
                    continue;
                size_t i00 = base;
                size_t i01 = base | m2;
                size_t i10 = base | m1;
                size_t i11 = base | m1 | m2;

                v[0] = dense_state[i00];
                v[1] = dense_state[i01];
                v[2] = dense_state[i10];
                v[3] = dense_state[i11];

                for (int r = 0; r < 4; ++r)
                {
                    w[r] = std::complex<double>(0.0, 0.0);
                    for (int c = 0; c < 4; ++c)
                        w[r] += U[r][c] * v[c];
                }

                dense_state[i00] = w[0];
                dense_state[i01] = w[1];
                dense_state[i10] = w[2];
                dense_state[i11] = w[3];
            }
        }

    public:
        QVM()
        {
            std::random_device rd;
            rng = std::mt19937(rd());
            dist = std::uniform_real_distribution<double>(0.0, 1.0);
            budget_allocator.execute_ibr_allocation(1.0);
            crypto_channel = QRQT_SecureChannel(PQKEMLevel::KYBER_512);
        }

        void set_vram_budget(size_t vram_bytes)
        {
            active_policy = MemoryGuard::select_optimal_policy(num_qubits, !maintains_stabilizer_polytope, vram_bytes);
            std::cout << "[QVM Router] Active Policy: " << static_cast<int>(active_policy) << std::endl;
        }

        BackendExecutionPolicy get_current_policy() const
        {
            return active_policy;
        }

        void allocate_qubits(size_t n) override
        {
            std::lock_guard<std::recursive_mutex> lock(state_mutex_);
            if (n <= num_qubits)
                return;
            is_qubit_allocated.resize(n, true);
            is_qubit_locked.resize(n, false);

            const bool can_dense = MemoryGuard::should_use_dense(n, vram_budget);

            if (can_dense)
            {
                const size_t old_dense = dense_state.size();
                dense_state.resize(1ULL << n, {0.0, 0.0});
#pragma omp parallel for schedule(static)
                for (size_t i = old_dense; i < dense_state.size(); ++i)
                    dense_state[i] = {0.0, 0.0};
            }
            else
            {
                const size_t old_n = num_qubits;
                if (mps_state.num_qubits() == 0)
                {
                    if (old_n > 0 && dense_state.size() >= (size_t(1) << old_n))
                    {
                        std::vector<mps::Complex> amps(dense_state.begin(),
                                                      dense_state.begin() + (size_t(1) << old_n));
                        mps_state = mps::MpsState::from_amps(amps, old_n, 16);
                    }
                    else
                    {
                        mps_state = mps::MpsState(old_n, 16);
                    }
                }
                if (mps_state.num_qubits() < n)
                    mps_state.append_qubits(n - mps_state.num_qubits());

                active_policy = BackendExecutionPolicy::Tensor_Network_MPS;
            }

            if (num_qubits == 0)
            {
                num_qubits = n;
                root = std::make_unique<StateNode>();
                StateNode *current = root.get();
                for (int t = num_qubits - 1; t >= 0; --t)
                {
                    current->left = std::make_unique<StateNode>();
                    current = current->left.get();
                }
                current->is_leaf = true;
                current->qmPtr_index = cb.quantize_and_store({1.0, 0.0});
                return;
            }

            while (num_qubits < n)
            {
                auto new_root = std::make_unique<StateNode>();
                new_root->left = std::move(root);
                root = std::move(new_root);
                num_qubits++;
            }
        }

        void allocate_qubit(size_t qubit_id) override
        {
            std::lock_guard<std::recursive_mutex> lock(state_mutex_);
            allocate_qubits(qubit_id + 1);
            if (qubit_id < is_qubit_allocated.size())
                is_qubit_allocated[qubit_id] = true;
        }

        void release_qubit(size_t qubit_id) override
        {
            std::lock_guard<std::recursive_mutex> lock(state_mutex_);
            if (qubit_id >= is_qubit_allocated.size())
                return;

            if (is_qubit_locked[qubit_id])
            {
                throw std::runtime_error("Lifecycle Error: Cannot release a locked qubit.");
            }

            reset_qubit_to_ground(qubit_id);

            is_qubit_allocated[qubit_id] = false;

            shrink_trailing_qubits();
        }

        void lock_hardware_id(size_t qubit_id) override
        {
            std::lock_guard<std::recursive_mutex> lock(state_mutex_);
            if (is_qubit_locked[qubit_id])
            {
                throw std::runtime_error("Concurrency Error: Qubit is already locked by another view.");
            }
            is_qubit_locked[qubit_id] = true;
        }

        void unlock_hardware_id(size_t qubit_id) override
        {
            std::lock_guard<std::recursive_mutex> lock(state_mutex_);
            is_qubit_locked[qubit_id] = false;
        }

        void apply_ga_rotor(size_t target, int blade_axis, double angle)
        {
            switch (blade_axis)
            {
            case 1:
                apply_h(target);
                apply_rz(target, angle);
                apply_h(target);
                break;
            case 2:
                apply_rz(target, M_PI / 2.0);
                apply_h(target);
                apply_rz(target, angle);
                apply_h(target);
                apply_rz(target, -M_PI / 2.0);
                break;
            case 3:
            default:
                apply_rz(target, angle);
                break;
            }
        }

        bool use_device_gate() const
        {
            return active_policy == BackendExecutionPolicy::Dense_StateVector &&
                   num_qubits > HOST_GATE_MAX_QUBITS &&
                   num_qubits <= MemoryGuard::DENSE_MAX_QUBITS &&
                   rvdl::gpu_backend_available();
        }

        void ensure_device_uploaded()
        {
            if (device_has_state_ && host_synced_)
                return;
            device_state_.upload(dense_state.data(), dense_state.size());
            device_has_state_ = true;
            host_synced_ = true;
        }

        void sync_device_to_host() const
        {
            if (!device_has_state_ || host_synced_)
                return;
            device_state_.download(dense_state.data(), dense_state.size());
            host_synced_ = true;
        }

        void invalidate_device()
        {
            host_synced_ = false;
        }

        void apply_h(size_t target) override
        {
            std::lock_guard<std::recursive_mutex> lock(state_mutex_);
            check_lock(target);
            if (active_policy == BackendExecutionPolicy::LIMDD_Compressed)
            {
                virtualized_state.apply_tensor_contraction();
            }
            else if (active_policy == BackendExecutionPolicy::Dense_StateVector)
            {
                if (use_device_gate())
                {
                    ensure_device_uploaded();
                    const double inv = 1.0 / std::sqrt(2.0);
                    std::vector<std::vector<std::complex<double>>> h_mat = {
                        {{inv, 0.0}, {inv, 0.0}},
                        {{inv, 0.0}, {-inv, 0.0}}};
                    ga::gpu::QuantumGate gate{{static_cast<int>(target)}, h_mat};
                    device_state_.apply_gate(gate, static_cast<uint32_t>(num_qubits));
                    host_synced_ = false;
                }
                else
                {
                    const size_t mask = 1ULL << target;
                    const double inv = 1.0 / std::sqrt(2.0);
                    for (size_t i = 0; i < dense_state.size(); ++i)
                    {
                        if (i & mask)
                            continue;
                        const size_t j = i | mask;
                        const std::complex<double> a = dense_state[i];
                        const std::complex<double> b = dense_state[j];
                        dense_state[i] = (a + b) * inv;
                        dense_state[j] = (a - b) * inv;
                    }
                    invalidate_device();
                }
            }
            else if (active_policy == BackendExecutionPolicy::Tensor_Network_MPS)
            {
                const double inv = 1.0 / std::sqrt(2.0);
                const mps::Complex H[4] = {{inv, 0.0}, {inv, 0.0}, {inv, 0.0}, {-inv, 0.0}};
                mps_state.apply_single(target, H);
            }
            else
            {
                apply_h_recursive(root, num_qubits - 1, target);
            }
        }

        void apply_x(size_t target) override
        {
            std::lock_guard<std::recursive_mutex> lock(state_mutex_);
            check_lock(target);
            if (active_policy == BackendExecutionPolicy::Dense_StateVector)
            {
                if (use_device_gate())
                {
                    ensure_device_uploaded();
                    std::vector<std::vector<std::complex<double>>> x_mat = {
                        {{0.0, 0.0}, {1.0, 0.0}},
                        {{1.0, 0.0}, {0.0, 0.0}}};
                    ga::gpu::QuantumGate gate{{static_cast<int>(target)}, x_mat};
                    device_state_.apply_gate(gate, static_cast<uint32_t>(num_qubits));
                    host_synced_ = false;
                }
                else
                {
                    const size_t mask = 1ULL << target;
                    for (size_t i = 0; i < dense_state.size(); ++i)
                    {
                        if (i & mask)
                            continue;
                        std::swap(dense_state[i], dense_state[i | mask]);
                    }
                    invalidate_device();
                }
            }
            else if (active_policy == BackendExecutionPolicy::Tensor_Network_MPS)
            {
                const mps::Complex X[4] = {{0.0, 0.0}, {1.0, 0.0}, {1.0, 0.0}, {0.0, 0.0}};
                mps_state.apply_single(target, X);
            }
            else
            {
                apply_x_recursive(root.get(), num_qubits - 1, target);
            }
        }

        void apply_z(size_t target) override
        {
            std::lock_guard<std::recursive_mutex> lock(state_mutex_);
            check_lock(target);
            if (active_policy == BackendExecutionPolicy::Dense_StateVector)
            {
                size_t total = dense_state.size();
                size_t mask = 1ULL << target;
                for (size_t i = 0; i < total; ++i)
                    if (i & mask)
                        dense_state[i] = -dense_state[i];
            }
            else if (active_policy == BackendExecutionPolicy::Tensor_Network_MPS)
            {
                const mps::Complex Z[4] = {{1.0, 0.0}, {0.0, 0.0}, {0.0, 0.0}, {-1.0, 0.0}};
                mps_state.apply_single(target, Z);
            }
            else
            {
                apply_rz_recursive(root.get(), static_cast<int>(num_qubits) - 1,
                                   static_cast<int>(target), {-1.0, 0.0});
            }
        }

        void apply_rz(size_t target, double angle) override
        {
            std::lock_guard<std::recursive_mutex> lock(state_mutex_);
            check_lock(target);
            if (std::fmod(angle, M_PI_2) != 0.0 && maintains_stabilizer_polytope)
            {
                const bool can_dense = MemoryGuard::should_use_dense(num_qubits, vram_budget);
                if (can_dense)
                {
                    size_t total = 1ULL << num_qubits;
                    for (size_t i = 0; i < total; ++i)
                        dense_state[i] = peek_state(i);
                }
                else
                {
                    if (mps_state.num_qubits() == 0)
                        mps_state = mps::MpsState(num_qubits, 16);
                }
                maintains_stabilizer_polytope = false;
                set_vram_budget(vram_budget);
            }

            std::complex<double> phase(std::cos(angle), std::sin(angle));

            if (active_policy == BackendExecutionPolicy::Dense_StateVector)
            {
                if (use_device_gate())
                {
                    ensure_device_uploaded();
                    std::vector<std::vector<std::complex<double>>> rz_mat = {
                        {{1.0, 0.0}, {0.0, 0.0}},
                        {{0.0, 0.0}, {phase.real(), phase.imag()}}};
                    ga::gpu::QuantumGate gate{{static_cast<int>(target)}, rz_mat};
                    device_state_.apply_gate(gate, static_cast<uint32_t>(num_qubits));
                    host_synced_ = false;
                }
                else
                {
                    size_t total = dense_state.size();
                    size_t mask = 1ULL << target;
                    for (size_t i = 0; i < total; ++i)
                        if (i & mask)
                            dense_state[i] *= phase;
                    invalidate_device();
                }
            }
            else if (active_policy == BackendExecutionPolicy::Tensor_Network_MPS)
            {
                const mps::Complex Rz[4] = {{1.0, 0.0}, {0.0, 0.0},
                                            {0.0, 0.0}, {phase.real(), phase.imag()}};
                mps_state.apply_single(target, Rz);
            }
            else
            {
                apply_rz_recursive(root.get(), static_cast<int>(num_qubits) - 1,
                                   static_cast<int>(target), phase);
            }
        }

        void apply_cnot(size_t control, size_t target) override
        {
            std::lock_guard<std::recursive_mutex> lock(state_mutex_);
            check_lock(control);
            check_lock(target);
            if (active_policy == BackendExecutionPolicy::Dense_StateVector)
            {
                if (use_device_gate())
                {
                    ensure_device_uploaded();
                    std::vector<std::vector<std::complex<double>>> cnot_mat = {
                        {{1.0, 0.0}, {0.0, 0.0}, {0.0, 0.0}, {0.0, 0.0}},
                        {{0.0, 0.0}, {1.0, 0.0}, {0.0, 0.0}, {0.0, 0.0}},
                        {{0.0, 0.0}, {0.0, 0.0}, {0.0, 0.0}, {1.0, 0.0}},
                        {{0.0, 0.0}, {0.0, 0.0}, {1.0, 0.0}, {0.0, 0.0}}};
                    ga::gpu::QuantumGate gate{{static_cast<int>(control), static_cast<int>(target)}, cnot_mat};
                    device_state_.apply_gate(gate, static_cast<uint32_t>(num_qubits));
                    host_synced_ = false;
                }
                else
                {
                    const size_t cmask = 1ULL << control;
                    const size_t tmask = 1ULL << target;
                    for (size_t i = 0; i < dense_state.size(); ++i)
                    {
                        if ((i & tmask) || !(i & cmask))
                            continue;
                        std::swap(dense_state[i], dense_state[i | tmask]);
                    }
                    invalidate_device();
                }
            }
            else if (active_policy == BackendExecutionPolicy::Tensor_Network_MPS)
            {
                apply_cnot_mps(control, target);
            }
            else
            {
                apply_cnot_recursive(root, num_qubits - 1, control, target);
            }
        }

        void apply_cnot_mps(size_t control, size_t target)
        {
            static const mps::Complex CNOT[16] = {
                {1.0, 0.0}, {0.0, 0.0}, {0.0, 0.0}, {0.0, 0.0},
                {0.0, 0.0}, {1.0, 0.0}, {0.0, 0.0}, {0.0, 0.0},
                {0.0, 0.0}, {0.0, 0.0}, {0.0, 0.0}, {1.0, 0.0},
                {0.0, 0.0}, {0.0, 0.0}, {1.0, 0.0}, {0.0, 0.0}};
            static const mps::Complex SWAP[16] = {
                {1.0, 0.0}, {0.0, 0.0}, {0.0, 0.0}, {0.0, 0.0},
                {0.0, 0.0}, {0.0, 0.0}, {1.0, 0.0}, {0.0, 0.0},
                {0.0, 0.0}, {1.0, 0.0}, {0.0, 0.0}, {0.0, 0.0},
                {0.0, 0.0}, {0.0, 0.0}, {0.0, 0.0}, {1.0, 0.0}};

            if (control == target)
                return;
            if (control + 1 == target)
            {
                mps_state.apply_two(control, CNOT);
                return;
            }
            if (target + 1 == control)
            {
                mps_state.apply_two(target, SWAP);
                mps_state.apply_two(target, CNOT);
                mps_state.apply_two(target, SWAP);
                return;
            }
            if (control < target)
            {
                for (size_t q = control; q + 1 < target; ++q)
                    mps_state.apply_two(q, SWAP);
                mps_state.apply_two(target - 1, CNOT);
                for (size_t q = target - 1; q > control; --q)
                    mps_state.apply_two(q - 1, SWAP);
            }
            else
            {
                for (size_t q = control; q > target + 1; --q)
                    mps_state.apply_two(q - 1, SWAP);
                mps_state.apply_two(target, CNOT);
                for (size_t q = target + 1; q < control; ++q)
                    mps_state.apply_two(q, SWAP);
            }
        }

        void apply_toffoli(size_t c1, size_t c2, size_t target) override
        {
            std::lock_guard<std::recursive_mutex> lock(state_mutex_);
            check_lock(c1);
            check_lock(c2);
            check_lock(target);

            apply_h(target);
            apply_cnot(c2, target);
            apply_rz(target, -M_PI / 4.0);
            apply_cnot(c1, target);
            apply_rz(target, M_PI / 4.0);
            apply_cnot(c2, target);
            apply_rz(target, -M_PI / 4.0);
            apply_cnot(c1, target);
            apply_rz(c2, M_PI / 4.0);
            apply_rz(target, M_PI / 4.0);
            apply_h(target);
            apply_cnot(c1, c2);
            apply_rz(c1, M_PI / 4.0);
            apply_rz(c2, -M_PI / 4.0);
            apply_cnot(c1, c2);
        }

        void apply_braid(size_t a, size_t b) override
        {
            std::lock_guard<std::recursive_mutex> lock(state_mutex_);
            check_lock(a);
            check_lock(b);
            if (a == b)
                return;

            if (active_policy != BackendExecutionPolicy::Dense_StateVector)
            {
                size_t total = 1ULL << num_qubits;
                for (size_t i = 0; i < total; ++i)
                    dense_state[i] = peek_state(i);
                active_policy = BackendExecutionPolicy::Dense_StateVector;
            }

            const double inv_sqrt2 = 1.0 / std::sqrt(2.0);
            const std::complex<double> I(0.0, 1.0);
            std::complex<double> R[4][4] = {
                {inv_sqrt2, 0.0, inv_sqrt2 * I, 0.0},
                {0.0, inv_sqrt2, 0.0, -inv_sqrt2 * I},
                {inv_sqrt2 * I, 0.0, inv_sqrt2, 0.0},
                {0.0, -inv_sqrt2 * I, 0.0, inv_sqrt2}};
            apply_two_qubit_gate_dense(a, b, R);
        }

        void apply_cbraid(size_t control, size_t a, size_t b) override
        {
            if (control == a || control == b || a == b)
            {
                IQuantumBackend::apply_cbraid(control, a, b);
                return;
            }
            std::lock_guard<std::recursive_mutex> lock(state_mutex_);
            check_lock(control);
            check_lock(a);
            check_lock(b);

            if (active_policy != BackendExecutionPolicy::Dense_StateVector)
            {
                size_t total = 1ULL << num_qubits;
                for (size_t i = 0; i < total; ++i)
                    dense_state[i] = peek_state(i);
                active_policy = BackendExecutionPolicy::Dense_StateVector;
            }

            const double inv_sqrt2 = 1.0 / std::sqrt(2.0);
            const std::complex<double> I(0.0, 1.0);
            std::complex<double> M[8][8] = {};
            for (int k = 0; k < 4; ++k)
                M[k][k] = 1.0;
            const std::complex<double> R[4][4] = {
                {inv_sqrt2, 0.0, inv_sqrt2 * I, 0.0},
                {0.0, inv_sqrt2, 0.0, -inv_sqrt2 * I},
                {inv_sqrt2 * I, 0.0, inv_sqrt2, 0.0},
                {0.0, -inv_sqrt2 * I, 0.0, inv_sqrt2}};
            for (int r = 0; r < 4; ++r)
                for (int col = 0; col < 4; ++col)
                    M[4 + r][4 + col] = R[r][col];

            size_t total = dense_state.size();
            size_t mc = 1ULL << control, ma = 1ULL << a, mb = 1ULL << b;
            size_t mask = mc | ma | mb;
            std::complex<double> v[8], w[8];

            for (size_t base = 0; base < total; ++base)
            {
                if (base & mask)
                    continue;
                size_t idx[8];
                for (int bc = 0; bc < 2; ++bc)
                    for (int ba = 0; ba < 2; ++ba)
                        for (int bb = 0; bb < 2; ++bb)
                        {
                            int k = (bc << 2) | (ba << 1) | bb;
                            idx[k] = base | (bc ? mc : 0) | (ba ? ma : 0) | (bb ? mb : 0);
                            v[k] = dense_state[idx[k]];
                        }
                for (int r = 0; r < 8; ++r)
                {
                    w[r] = std::complex<double>(0.0, 0.0);
                    for (int col = 0; col < 8; ++col)
                        w[r] += M[r][col] * v[col];
                }
                for (int k = 0; k < 8; ++k)
                    dense_state[idx[k]] = w[k];
            }
        }

        void apply_noise(size_t qubit_id, int channel, double param) override
        {
            std::lock_guard<std::recursive_mutex> lock(state_mutex_);
            check_lock(qubit_id);
            if (active_policy != BackendExecutionPolicy::Dense_StateVector)
            {
                size_t total = 1ULL << num_qubits;
                for (size_t i = 0; i < total; ++i)
                    dense_state[i] = peek_state(i);
                active_policy = BackendExecutionPolicy::Dense_StateVector;
            }

            const size_t mask = 1ULL << qubit_id;
            switch (channel)
            {
                case 0:
                {
                    std::uniform_real_distribution<double> d(0.0, 1.0);
                    double r = d(rng);
                    double px = param / 4.0;
                    if (r < px) apply_x(qubit_id);
                    else if (r < 2 * px) { apply_z(qubit_id); apply_x(qubit_id); }
                    else if (r < 3 * px) apply_z(qubit_id);
                    break;
                }
                case 1:
                {
                    std::bernoulli_distribution d(param);
                    if (d(rng)) apply_z(qubit_id);
                    break;
                }
                case 2:
                {
                    amplitude_damping_dense(qubit_id, param, mask);
                    break;
                }
                case 3:
                {
                    std::bernoulli_distribution d(param);
                    if (d(rng)) apply_x(qubit_id);
                    break;
                }
                default:
                    break;
            }
        }

        void amplitude_damping_dense(size_t target, double gamma, size_t mask)
        {
            double p1 = 0.0;
            for (size_t i = 0; i < dense_state.size(); ++i)
                if (i & mask)
                    p1 += std::norm(dense_state[i]);

            std::bernoulli_distribution d(gamma * p1);
            if (d(rng))
            {
                for (size_t i = 0; i < dense_state.size(); ++i)
                {
                    if (i & mask)
                    {
                        dense_state[i ^ mask] = dense_state[i];
                        dense_state[i] = std::complex<double>(0.0, 0.0);
                    }
                }
                if (p1 > 0.0)
                {
                    double n = 1.0 / std::sqrt(p1);
                    for (size_t i = 0; i < dense_state.size(); ++i)
                        dense_state[i] *= n;
                }
            }
            else
            {
                double denom = std::sqrt(1.0 - gamma * p1);
                if (denom <= 0.0)
                    return;
                double s = std::sqrt(1.0 - gamma) / denom;
                double s0 = 1.0 / denom;
                for (size_t i = 0; i < dense_state.size(); ++i)
                {
                    if (i & mask)
                        dense_state[i] *= s;
                    else
                        dense_state[i] *= s0;
                }
            }
        }

        void apply_basis(size_t target, double theta, double phi) override
        {
            std::lock_guard<std::recursive_mutex> lock(state_mutex_);
            check_lock(target);

            const double c_half = std::cos(theta / 2.0);
            const double s_half = std::sin(theta / 2.0);

            const std::complex<double> a(c_half, 0.0);
            const std::complex<double> d(c_half, 0.0);
            const std::complex<double> c(std::cos(phi) * s_half, std::sin(phi) * s_half);
            const std::complex<double> b(-std::cos(phi) * s_half, std::sin(phi) * s_half);

            if (active_policy == BackendExecutionPolicy::Dense_StateVector)
            {
                std::vector<std::vector<std::complex<double>>> U = {
                    {a, b},
                    {c, d}};
                ga::gpu::QuantumGate gate{{static_cast<int>(target)}, U};
                ga::gpu::KokkosInterface::offload_to_statevec(dense_state.data(), num_qubits, gate);
            }
            else
            {
                apply_basis_recursive(root, static_cast<int>(num_qubits) - 1,
                                      static_cast<int>(target), a, b, c, d);
            }
        }

        void prepare_amplitudes(const std::vector<std::complex<double>> &amps, size_t n) override
        {
            std::lock_guard<std::recursive_mutex> lock(state_mutex_);
            if (amps.size() != (size_t(1) << n))
                return;
            allocate_qubits(n);
            const bool can_dense = MemoryGuard::calculate_dense_bytes(n, 100) <= vram_budget;
            if (can_dense)
            {
                dense_state.resize(amps.size());
                for (size_t i = 0; i < amps.size(); ++i)
                    dense_state[i] = amps[i];
                active_policy = BackendExecutionPolicy::Dense_StateVector;
            }
            else
            {
                std::vector<mps::Complex> a(amps.begin(), amps.end());
                mps_state = mps::MpsState::from_amps(a, n, 16);
                active_policy = BackendExecutionPolicy::Tensor_Network_MPS;
            }
            maintains_stabilizer_polytope = false;

            device_has_state_ = false;
            host_synced_ = true;
        }

        int measure(size_t target) override
        {
            std::lock_guard<std::recursive_mutex> lock(state_mutex_);
            if (active_policy == BackendExecutionPolicy::Dense_StateVector)
            {
                sync_device_to_host();
                size_t total_states = 1ULL << num_qubits;
                uint32_t mask = 1U << target;
                double prob_1 = 0.0;
#pragma omp parallel for reduction(+ : prob_1) schedule(static)
                for (size_t i = 0; i < total_states; ++i)
                {
                    if ((i & mask) != 0)
                    {
                        prob_1 += std::norm(dense_state[i]);
                    }
                }

                double r = dist(rng);
                int measured_val = (r <= prob_1) ? 1 : 0;
                double prob_outcome = (measured_val == 1) ? prob_1 : (1.0 - prob_1);

                if (prob_outcome < 1e-12)
                {
                    prob_outcome = 1.0;
                }

                double norm_factor = 1.0 / std::sqrt(prob_outcome);
#pragma omp parallel for schedule(static)

                for (size_t i = 0; i < total_states; ++i)
                {
                    bool is_bit_set = ((i & mask) != 0);
                    if ((measured_val == 1 && !is_bit_set) || (measured_val == 0 && is_bit_set))
                    {
                        dense_state[i] = std::complex<double>(0.0, 0.0);
                    }
                    else
                    {
                        dense_state[i] *= norm_factor;
                    }
                }

                return measured_val;
            }

            if (active_policy == BackendExecutionPolicy::Tensor_Network_MPS)
            {
                return mps_state.measure(target, rng);
            }

            double prob_1 = calculate_prob_1(root.get(), num_qubits - 1, target);
            double r = dist(rng);
            int measured_val = (r <= prob_1) ? 1 : 0;
            double prob_outcome = (measured_val == 1) ? prob_1 : (1.0 - prob_1);

            if (prob_outcome < 1e-12)
            {
                prob_outcome = 1.0;
            }

            double norm_factor = 1.0 / std::sqrt(prob_outcome);
            collapse_and_normalize(root.get(), num_qubits - 1, target, measured_val, norm_factor);

            return measured_val;
        }

        double expectation_z(size_t target) override
        {
            std::lock_guard<std::recursive_mutex> lock(state_mutex_);
            double prob_1 = 0.0;
            if (active_policy == BackendExecutionPolicy::Dense_StateVector)
            {
                sync_device_to_host();
                size_t total_states = 1ULL << num_qubits;
                uint32_t mask = 1U << target;
#pragma omp parallel for reduction(+ : prob_1) schedule(static)
                for (size_t i = 0; i < total_states; ++i)
                    if ((i & mask) != 0)
                        prob_1 += std::norm(dense_state[i]);
            }
            else if (active_policy == BackendExecutionPolicy::Tensor_Network_MPS)
            {
                return mps_state.expectation_z(target);
            }
            else
            {
                prob_1 = calculate_prob_1(root.get(), num_qubits - 1, target);
            }
            return 1.0 - 2.0 * prob_1;
        }

        std::complex<double> peek_state(size_t state_index) const
        {
            if (active_policy == BackendExecutionPolicy::Dense_StateVector && state_index < dense_state.size())
            {
                sync_device_to_host();
                return dense_state[state_index];
            }
            if (active_policy == BackendExecutionPolicy::Tensor_Network_MPS)
            {
                if (num_qubits > 20)
                    return std::complex<double>(0.0, 0.0);
                auto sv = mps_state.get_state_vector();
                if (state_index < sv.size())
                    return sv[state_index];
                return std::complex<double>(0.0, 0.0);
            }
            return retrieve_amplitude(root.get(), state_index, num_qubits - 1);
        }

        size_t get_num_qubits() const override
        {
            std::lock_guard<std::recursive_mutex> lock(state_mutex_);
            return num_qubits;
        }
        size_t get_max_qubits() const override
        {
            std::lock_guard<std::recursive_mutex> lock(state_mutex_);
            size_t dense_bits = 0;
            size_t bytes = 16;
            while (bytes * 2 <= vram_budget && dense_bits < 60)
            {
                bytes *= 2;
                ++dense_bits;
            }
            return std::max(dense_bits, size_t(50));
        }
        double get_temperature_celsius() { thermal.update(1.0); return thermal.read_celsius(); }
        double get_temperature_kelvin() { thermal.update(1.0); return thermal.read_kelvin(); }
        void set_target_temperature_celsius(double celsius) { thermal.set_target_c(celsius); }
        double get_target_temperature_celsius() const { return thermal.target_celsius(); }

        std::vector<std::complex<double>> get_amplitudes() const
        {
            if (active_policy == BackendExecutionPolicy::Tensor_Network_MPS)
            {
                if (num_qubits > 20)
                    return {};
                return mps_state.get_state_vector();
            }
            size_t n = 1ULL << num_qubits;
            std::vector<std::complex<double>> out;
            out.reserve(n);
            for (size_t i = 0; i < n; ++i)
                out.push_back(peek_state(i));
            return out;
        }

        std::vector<std::complex<double>> get_state_vector() const override
        {
            std::lock_guard<std::recursive_mutex> lock(state_mutex_);
            return get_amplitudes();
        }

        qhal::health::NumericHealth monitor_health() const
        {
            qhal::health::NumericHealth h;
            if (active_policy == BackendExecutionPolicy::Dense_StateVector)
            {
                sync_device_to_host();
                double n2 = 0.0;
                for (size_t i = 0; i < dense_state.size(); ++i)
                    n2 += std::norm(dense_state[i]);
                h.normalization_residual = std::abs(1.0 - std::sqrt(n2));
            }
            else
            {
                h.normalization_residual = std::abs(1.0 - std::sqrt(calculate_norm(root.get())));
            }
            
            h.condition_number = 1.0;
            h.healthy = h.normalization_residual < 1e-6;
            return h;
        }

        void execute_distributed_gate(size_t control_node, size_t target_node, size_t target_qubit, double link_latency_ms)
        {
            qecc.apply_dynamic_puncturing(link_latency_ms);
            teleporter.execute_two_way_teleportation(control_node, target_node);
            crypto_channel.transmit_bell_basis(1, 0, link_latency_ms * 10.0);
            crypto_channel.execute_post_selection();

            apply_h(target_qubit);
        }

        void establish_network_link(size_t remote_node_id, double latency_metric)
        {
            resource_game.compute_nash_equilibrium();
            resource_game.execute_bayesian_mean_field_approximation();
            qecc.apply_dynamic_puncturing(latency_metric);
            teleporter.execute_two_way_teleportation(0, remote_node_id);
        }

        void secure_classical_transmit(uint8_t b1, uint8_t b2, double distance_km)
        {
            crypto_channel.transmit_bell_basis(b1, b2, distance_km);
            crypto_channel.execute_post_selection();
        }
    };
}