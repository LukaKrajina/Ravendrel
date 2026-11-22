#pragma once
#include <vector>
#include <complex>
#include <memory>
#include <stdexcept>
#include <thread>
#include <iostream>

#ifdef __AVX512F__
#include <immintrin.h>
#endif

#include "Kokkos_Core.hpp"
#include "NuclearNorm.hpp"
#include "../qhal/GpuBackend.hpp"

namespace ga
{
    namespace cpu
    {
        template <typename T>
        class AlignedAllocator
        {
        public:
            using value_type = T;
            AlignedAllocator() = default;
            template <typename U>
            constexpr AlignedAllocator(const AlignedAllocator<U> &) noexcept {}

            T *allocate(std::size_t n)
            {
                void *ptr = nullptr;
                size_t alignment = 64;
                size_t size = n * sizeof(T);
#if defined(_MSC_VER) || defined(__MINGW32__)
                ptr = _aligned_malloc(size, alignment);
#else
                if (posix_memalign(&ptr, alignment, size) != 0)
                    throw std::bad_alloc();
#endif
                if (!ptr)
                    throw std::bad_alloc();
                return static_cast<T *>(ptr);
            }

            void deallocate(T *p, std::size_t) noexcept
            {
#if defined(_MSC_VER) || defined(__MINGW32__)
                _aligned_free(p);
#else
                free(p);
#endif
            }
        };

        struct NUMAContext
        {
            static void pin_thread_to_core(std::thread &t, int core_id)
            {
#if defined(__linux__) && !defined(__ANDROID__)
                cpu_set_t cpuset;
                CPU_ZERO(&cpuset);
                CPU_SET(core_id, &cpuset);
                pthread_setaffinity_np(t.native_handle(), sizeof(cpu_set_t), &cpuset);
#endif
            }

            template <typename T>
            static void first_touch_initialization(std::vector<T, AlignedAllocator<T>> &state_vector)
            {
                size_t size = state_vector.size();
#pragma omp parallel for schedule(static)
                for (size_t i = 0; i < size; ++i)
                {
                    state_vector[i] = T(0);
                }
            }
        };
    }

    namespace simd
    {
        class VectorEngine
        {
        public:
            static void apply_fma_rotation(
                std::complex<float> *__restrict state,
                size_t length,
                float cos_theta,
                float sin_theta)
            {
#if defined(RVDL_SIMD_AVX512)
                __m512 v_cos = _mm512_set1_ps(cos_theta);
                __m512 v_sin = _mm512_set1_ps(sin_theta);
                for (size_t i = 0; i < length * 2; i += 16)
                {
                    _mm_prefetch(reinterpret_cast<const char *>(&state[i + 32]), _MM_HINT_T0);
                    __m512 v_state = _mm512_load_ps(reinterpret_cast<float *>(&state[i]));
                    __m512 v_state_swapped = _mm512_permute_ps(v_state, _MM_SHUFFLE(2, 3, 0, 1));
                    __m512 v_res1 = _mm512_mul_ps(v_state, v_cos);
                    __m512 v_final = _mm512_fmadd_ps(v_state_swapped, v_sin, v_res1);
                    _mm512_store_ps(reinterpret_cast<float *>(&state[i]), v_final);
                }
#elif defined(RVDL_SIMD_LASX) || defined(RVDL_SIMD_LSX)
#pragma omp simd
                for (size_t i = 0; i < length; ++i)
                {
                    std::complex<float> val = state[i];
                    state[i] = std::complex<float>(
                        val.real() * cos_theta - val.imag() * sin_theta,
                        val.real() * sin_theta + val.imag() * cos_theta);
                }
#else
                for (size_t i = 0; i < length; ++i)
                {
                    std::complex<float> val = state[i];
                    state[i] = std::complex<float>(
                        val.real() * cos_theta - val.imag() * sin_theta,
                        val.real() * sin_theta + val.imag() * cos_theta);
                }
#endif
            }
        };
    }

    namespace gpu
    {
        using Complex64 = Kokkos::complex<double>;
        using View1D = Kokkos::View<Complex64 *>;
        using View2D = Kokkos::View<Complex64 **>;

        struct DiaQLayout
        {
            Kokkos::View<int *> active_diagonals;
            Kokkos::View<Complex64 *> matrix_data;
            size_t matrix_size;
            size_t num_diagonals;

            void optimize_for_spgemm(const std::vector<std::vector<std::complex<double>>> &dense_matrix)
            {
                matrix_size = dense_matrix.size();
                int n = static_cast<int>(matrix_size);

                std::vector<int> h_diags;
                std::vector<Complex64> h_data;

                for (int d = -n + 1; d < n; ++d)
                {
                    bool is_active = false;
                    std::vector<Complex64> diag_elements(n, {0.0, 0.0});

                    for (int i = 0; i < n; ++i)
                    {
                        int j = i + d;
                        if (j >= 0 && j < n && std::abs(dense_matrix[i][j]) > 1e-12)
                        {
                            is_active = true;
                            diag_elements[i] = Complex64(dense_matrix[i][j].real(), dense_matrix[i][j].imag());
                        }
                    }

                    if (is_active)
                    {
                        h_diags.push_back(d);
                        h_data.insert(h_data.end(), diag_elements.begin(), diag_elements.end());
                    }
                }

                num_diagonals = h_diags.size();
                active_diagonals = Kokkos::View<int *>("ActiveDiagonals", num_diagonals);
                matrix_data = Kokkos::View<Complex64 *>("MatrixData", h_data.size());
                auto mirror_diags = Kokkos::create_mirror_view(active_diagonals);
                for (size_t i = 0; i < num_diagonals; ++i)
                    mirror_diags(i) = h_diags[i];
                Kokkos::deep_copy(active_diagonals, mirror_diags);
                auto mirror_data = Kokkos::create_mirror_view(matrix_data);
                for (size_t i = 0; i < h_data.size(); ++i)
                    mirror_data(i) = h_data[i];
                Kokkos::deep_copy(matrix_data, mirror_data);
                std::cout << "[Kokkos DiaQ] Packed " << matrix_size << "x" << matrix_size
                          << " matrix into " << num_diagonals << " active diagonals.\n";
            }
        };

        struct QuantumGate
        {
            std::vector<int> targets;
            std::vector<std::vector<std::complex<double>>> matrix;
        };

        class DAGOptimizer
        {
        public:
            static void sparsity_aware_gate_fusion(std::vector<QuantumGate> &gate_sequence)
            {
                if (gate_sequence.size() < 2)
                    return;
                std::vector<QuantumGate> fused_sequence;
                QuantumGate current_accumulator = gate_sequence[0];
                for (size_t i = 1; i < gate_sequence.size(); ++i)
                {
                    if (current_accumulator.targets == gate_sequence[i].targets)
                    {
                        size_t dim = current_accumulator.matrix.size();
                        std::vector<std::vector<std::complex<double>>> fused_matrix(dim, std::vector<std::complex<double>>(dim, 0.0));

                        for (size_t row = 0; row < dim; ++row)
                        {
                            for (size_t col = 0; col < dim; ++col)
                            {
                                for (size_t k = 0; k < dim; ++k)
                                {
                                    fused_matrix[row][col] += gate_sequence[i].matrix[row][k] * current_accumulator.matrix[k][col];
                                }
                            }
                        }
                        current_accumulator.matrix = std::move(fused_matrix);
                    }
                    else
                    {
                        fused_sequence.push_back(current_accumulator);
                        current_accumulator = gate_sequence[i];
                    }
                }
                fused_sequence.push_back(current_accumulator);

                std::cout << "[DAG Fusion] Reduced DAG sequence from "
                          << gate_sequence.size() << " to " << fused_sequence.size() << " operations.\n";
                gate_sequence = std::move(fused_sequence);
            }

            inline static double poa_bound_ = 2.5;

            static double potential_function(const std::vector<QuantumGate>& seq) {
                double phi = 0.0;
                for (const auto& g : seq) phi += channel_importance(g);
                return phi;
            }

            static double channel_importance(const QuantumGate& g) {
                double fro2 = 0.0;
                for (const auto& row : g.matrix)
                    for (const auto& c : row) fro2 += std::norm(c);
                return fro2;
            }

            static double icm_importance(const std::vector<std::vector<double>>& R, size_t f) {
                return icm_channel_importance(R, f);
            }
            
            static double price_of_anarchy_bound() { return poa_bound_; }
            static void set_price_of_anarchy_bound(double bound) { poa_bound_ = bound; }
        };

        class KokkosInterface
        {
        public:
            template <typename ExecSpace, typename MemSpace>
            static void offload_to_statevec_impl(std::complex<double> *host_state_vector, uint32_t num_qubits, const QuantumGate &gate)
            {
                constexpr size_t MAX_TARGETS = 8;
                size_t sv_size = 1ULL << num_qubits;
                size_t n_targets = gate.targets.size();
                size_t dim = gate.matrix.size();

                if (n_targets == 0 || n_targets > MAX_TARGETS)
                    throw std::invalid_argument("offload_to_statevec: unsupported target count.");
                if (dim != (1ULL << n_targets))
                    throw std::invalid_argument("offload_to_statevec: matrix dim must equal 2^n_targets.");

                uint32_t mask = 0;
                int targets[MAX_TARGETS];
                for (size_t i = 0; i < n_targets; ++i)
                {
                    targets[i] = gate.targets[i];
                    mask |= (1U << targets[i]);
                }

                using SVView = Kokkos::View<Complex64 *, MemSpace>;
                using SVHost = typename SVView::host_mirror_type;
                using MView = Kokkos::View<Complex64 **, MemSpace>;
                using MHost = typename MView::host_mirror_type;

                static SVView cached_d_sv;
                static SVHost cached_h_sv;
                static MView cached_d_matrix;
                static MHost cached_h_matrix;
                static size_t cached_sv_size = 0, cached_dim = 0;

                if (cached_sv_size != sv_size || cached_d_sv.data() == nullptr)
                {
                    cached_d_sv = SVView("DeviceStateVector", sv_size);
                    cached_h_sv = Kokkos::create_mirror_view(cached_d_sv);
                    cached_sv_size = sv_size;
                }
                if (cached_dim != dim || cached_d_matrix.data() == nullptr)
                {
                    cached_d_matrix = MView("DeviceGateMatrix", dim, dim);
                    cached_h_matrix = Kokkos::create_mirror_view(cached_d_matrix);
                    cached_dim = dim;
                }

                SVView d_sv = cached_d_sv;
                SVHost h_sv = cached_h_sv;
                for (size_t i = 0; i < sv_size; ++i)
                    h_sv(i) = Complex64(host_state_vector[i].real(), host_state_vector[i].imag());
                Kokkos::deep_copy(d_sv, h_sv);

                MView d_matrix = cached_d_matrix;
                MHost h_matrix = cached_h_matrix;
                for (size_t r = 0; r < dim; ++r)
                    for (size_t c = 0; c < dim; ++c)
                        h_matrix(r, c) = Complex64(gate.matrix[r][c].real(), gate.matrix[r][c].imag());
                Kokkos::deep_copy(d_matrix, h_matrix);

                const size_t num_groups = sv_size >> n_targets;
                const uint32_t nq = num_qubits;
                const uint32_t nt = static_cast<uint32_t>(n_targets);
                const uint32_t dm = static_cast<uint32_t>(dim);
                const uint32_t mmask = mask;

                Kokkos::parallel_for("ApplyQuantumGate", Kokkos::RangePolicy<ExecSpace>(0, num_groups), KOKKOS_LAMBDA(const int g) {
                    uint32_t base = 0, bit = 0;
                    for (uint32_t pos = 0; pos < nq; ++pos)
                    {
                        if (!(mmask & (1U << pos)))
                        {
                            if (g & (1U << bit))
                                base |= (1U << pos);
                            ++bit;
                        }
                    }

                    double vr[MAX_TARGETS], vi[MAX_TARGETS];
                    for (uint32_t c = 0; c < dm; ++c)
                    {
                        uint32_t idx = base;
                        for (uint32_t t = 0; t < nt; ++t)
                            if (c & (1U << (nt - 1 - t)))
                                idx |= (1U << targets[t]);
                        vr[c] = d_sv(idx).real();
                        vi[c] = d_sv(idx).imag();
                    }

                    for (uint32_t r = 0; r < dm; ++r)
                    {
                        double re = 0.0, im = 0.0;
                        for (uint32_t c = 0; c < dm; ++c)
                        {
                            const Complex64 m = d_matrix(r, c);
                            re += m.real() * vr[c] - m.imag() * vi[c];
                            im += m.real() * vi[c] + m.imag() * vr[c];
                        }
                        uint32_t idx = base;
                        for (uint32_t t = 0; t < nt; ++t)
                            if (r & (1U << (nt - 1 - t)))
                                idx |= (1U << targets[t]);
                        d_sv(idx) = Complex64(re, im);
                    }
                });

                Kokkos::fence();
                Kokkos::deep_copy(h_sv, d_sv);
                for (size_t i = 0; i < sv_size; ++i)
                    host_state_vector[i] = std::complex<double>(h_sv(i).real(), h_sv(i).imag());
            }

            static void offload_to_statevec(std::complex<double> *host_state_vector, uint32_t num_qubits, const QuantumGate &gate)
            {
#if defined(KOKKOS_ENABLE_CUDA)
                if (rvdl::gpu_backend_available())
                    offload_to_statevec_impl<Kokkos::Cuda, Kokkos::CudaSpace>(host_state_vector, num_qubits, gate);
                else
                    offload_to_statevec_impl<Kokkos::DefaultExecutionSpace, typename Kokkos::DefaultExecutionSpace::memory_space>(host_state_vector, num_qubits, gate);
#else
                offload_to_statevec_impl<Kokkos::DefaultExecutionSpace, typename Kokkos::DefaultExecutionSpace::memory_space>(host_state_vector, num_qubits, gate);
#endif
            }
        };

        template <typename ExecSpace, typename MemSpace>
        class DeviceStateVector
        {
            static constexpr size_t MAX_TARGETS = 8;

            Kokkos::View<Complex64 *, MemSpace> d_sv_;
            mutable typename Kokkos::View<Complex64 *, MemSpace>::host_mirror_type h_sv_;
            Kokkos::View<Complex64 **, MemSpace> d_matrix_;
            mutable typename Kokkos::View<Complex64 **, MemSpace>::host_mirror_type h_matrix_;
            size_t size_ = 0;
            size_t matrix_dim_ = 0;

        public:
            size_t size() const { return size_; }
            bool valid() const { return d_sv_.data() != nullptr; }

            void resize(size_t n)
            {
                if (n == size_ && d_sv_.data())
                    return;
                d_sv_ = Kokkos::View<Complex64 *, MemSpace>("DeviceStateVector", n);
                h_sv_ = Kokkos::create_mirror_view(d_sv_);
                size_ = n;
            }

            void upload(const std::complex<double> *host, size_t n)
            {
                resize(n);
                for (size_t i = 0; i < n; ++i)
                    h_sv_(i) = Complex64(host[i].real(), host[i].imag());
                Kokkos::deep_copy(d_sv_, h_sv_);
            }

            void download(std::complex<double> *host, size_t n) const
            {
                Kokkos::deep_copy(h_sv_, d_sv_);
                for (size_t i = 0; i < n; ++i)
                    host[i] = std::complex<double>(h_sv_(i).real(), h_sv_(i).imag());
            }

            void apply_gate(const QuantumGate &gate, uint32_t num_qubits)
            {
                const size_t n_targets = gate.targets.size();
                const size_t dim = gate.matrix.size();
                if (n_targets == 0 || n_targets > MAX_TARGETS)
                    throw std::invalid_argument("DeviceStateVector::apply_gate: unsupported target count.");
                if (dim != (1ULL << n_targets))
                    throw std::invalid_argument("DeviceStateVector::apply_gate: matrix dim must equal 2^n_targets.");

                uint32_t mask = 0;
                int targets[MAX_TARGETS];
                for (size_t i = 0; i < n_targets; ++i)
                {
                    targets[i] = gate.targets[i];
                    mask |= (1U << targets[i]);
                }

                if (dim != matrix_dim_ || !d_matrix_.data())
                {
                    d_matrix_ = Kokkos::View<Complex64 **, MemSpace>("DeviceGateMatrix", dim, dim);
                    h_matrix_ = Kokkos::create_mirror_view(d_matrix_);
                    matrix_dim_ = dim;
                }
                for (size_t r = 0; r < dim; ++r)
                    for (size_t c = 0; c < dim; ++c)
                        h_matrix_(r, c) = Complex64(gate.matrix[r][c].real(), gate.matrix[r][c].imag());
                Kokkos::deep_copy(d_matrix_, h_matrix_);

                const size_t sv_size = 1ULL << num_qubits;
                const size_t num_groups = sv_size >> n_targets;
                const uint32_t nq = num_qubits;
                const uint32_t nt = static_cast<uint32_t>(n_targets);
                const uint32_t dm = static_cast<uint32_t>(dim);
                const uint32_t mmask = mask;

                Kokkos::View<Complex64 *, MemSpace> sv = d_sv_;
                Kokkos::View<Complex64 **, MemSpace> mat = d_matrix_;

                Kokkos::parallel_for("ApplyGateOnDevice", Kokkos::RangePolicy<ExecSpace>(0, num_groups),
                    KOKKOS_LAMBDA(const int g) {
                    uint32_t base = 0, bit = 0;
                    for (uint32_t pos = 0; pos < nq; ++pos)
                    {
                        if (!(mmask & (1U << pos)))
                        {
                            if (g & (1U << bit))
                                base |= (1U << pos);
                            ++bit;
                        }
                    }
                    double vr[MAX_TARGETS], vi[MAX_TARGETS];
                    for (uint32_t c = 0; c < dm; ++c)
                    {
                        uint32_t idx = base;
                        for (uint32_t t = 0; t < nt; ++t)
                            if (c & (1U << (nt - 1 - t)))
                                idx |= (1U << targets[t]);
                        vr[c] = sv(idx).real();
                        vi[c] = sv(idx).imag();
                    }
                    for (uint32_t r = 0; r < dm; ++r)
                    {
                        double re = 0.0, im = 0.0;
                        for (uint32_t c = 0; c < dm; ++c)
                        {
                            const Complex64 m = mat(r, c);
                            re += m.real() * vr[c] - m.imag() * vi[c];
                            im += m.real() * vi[c] + m.imag() * vr[c];
                        }
                        uint32_t idx = base;
                        for (uint32_t t = 0; t < nt; ++t)
                            if (r & (1U << (nt - 1 - t)))
                                idx |= (1U << targets[t]);
                        sv(idx) = Complex64(re, im);
                    }
                });
            }
        };
    }
}