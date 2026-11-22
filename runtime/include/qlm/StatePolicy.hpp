#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>

namespace qlm
{
    enum class StateBackend
    {
        Auto,
        Dense,
        Sparse,
        Ras
    };

    inline const char *state_backend_name(StateBackend b)
    {
        switch (b)
        {
            case StateBackend::Dense: return "dense";
            case StateBackend::Sparse: return "sparse";
            case StateBackend::Ras: return "ras";
            default: return "auto";
        }
    }

    inline StateBackend parse_state_backend(const std::string &s)
    {
        if (s == "dense") return StateBackend::Dense;
        if (s == "sparse" || s == "dq_sparse") return StateBackend::Sparse;
        if (s == "ras" || s == "lowrank") return StateBackend::Ras;
        return StateBackend::Auto;
    }

    struct StateResources
    {
        size_t qubits = 16;
        size_t batch_size = 1;
        size_t params = 0;
        uint64_t host_memory_bytes = 0;
        uint64_t device_memory_bytes = 0;
        size_t cpu_threads = 0;
        bool gpu_available = false;
        size_t moe_experts = 1;
        double requested_sparse_epsilon = 0.0;
        size_t sparse_tile_bits = 6;
    };






    struct AdjointRuntimeConfig
    {
        size_t tile_bits = 0;
        double tile_epsilon = 0.0;
        size_t sparse_tile_bits = 6;
        double sparse_epsilon = 0.0;
        size_t ras_rank = 0;
        double ras_epsilon = 0.0;
        uint64_t ras_seed = 42;
        size_t moe_experts = 1;
        size_t active_expert = 0;
        const size_t *gate_expert = nullptr;
        double *skipped_mass = nullptr;
        double *error_bound = nullptr;
        size_t *active_tiles = nullptr;
        size_t *total_tiles = nullptr;
    };

    struct StateDecision
    {
        StateBackend backend = StateBackend::Dense;
        double epsilon = 0.0;
        size_t tile_bits = 6;
        size_t suggested_batch = 0;
        std::string reason;
    };





    inline uint64_t dense_working_bytes(const StateResources &r)
    {
        const uint64_t dim = uint64_t(1) << std::min<size_t>(r.qubits, 30);
        const uint64_t B = std::max<size_t>(1, r.batch_size);
        return dim * 16ull * 4ull * B;
    }






    inline StateDecision select_state_backend(
        StateBackend requested,
        const StateResources &r,
        bool sparse_pod_available,
        bool dense_pod_available,
        double requested_epsilon = 0.0)
    {
        StateDecision d;
        d.tile_bits = r.sparse_tile_bits ? r.sparse_tile_bits : 6;
        d.suggested_batch = 0;
        (void)dense_pod_available;

        if (requested == StateBackend::Dense)
        {
            d.backend = StateBackend::Dense;
            d.epsilon = 0.0;
            d.reason = "manual dense (exact parity baseline)";
            return d;
        }

        if (requested == StateBackend::Sparse || requested == StateBackend::Ras)
        {
            if (!sparse_pod_available)
            {
                d.backend = StateBackend::Dense;
                d.epsilon = 0.0;
                d.reason = "manual compact state requested but backend unavailable -> dense fallback";
                return d;
            }
            d.backend = requested;
            d.epsilon = std::max(0.0, requested_epsilon);
            if (requested == StateBackend::Ras)
                d.reason = d.epsilon > 0.0
                    ? "manual RAS low-rank with factor-level adjoint"
                    : "manual RAS exact branch (bit-identical to dense)";
            else
                d.reason = d.epsilon > 0.0
                    ? "manual sparse with distributed quantisation"
                    : "manual sparse exact branch (bit-identical to dense)";
            return d;
        }

        if (!sparse_pod_available)
        {
            d.backend = StateBackend::Dense;
            d.reason = "auto: sparse adjoint backend not injected -> dense";
            return d;
        }
        if (r.moe_experts > 1)
        {
            d.backend = StateBackend::Dense;
            d.reason = "auto: MoE expert routing incompatible with shared-unitary sparse adjoint";
            return d;
        }

        const uint64_t dense_bytes = dense_working_bytes(r);
        uint64_t budget = 0;
        if (r.gpu_available && r.device_memory_bytes > 0)
            budget = r.device_memory_bytes / 2;
        else if (r.host_memory_bytes > 0)
            budget = r.host_memory_bytes / 4;
        else
            budget = uint64_t(256) * 1024ull * 1024ull;

        const double pressure = double(dense_bytes) / double(std::max<uint64_t>(1, budget));
        const bool small_n = r.qubits <= 16;
        const bool explicit_epsilon = requested_epsilon > 0.0;

        if (explicit_epsilon)
        {
            d.backend = StateBackend::Sparse;
            d.epsilon = requested_epsilon;
            d.reason = "auto: sparse epsilon explicitly requested -> distributed quantisation";
            return d;
        }

        if (small_n && pressure <= 0.6)
        {
            d.backend = StateBackend::Dense;
            d.reason = "auto: n<=16 and dense working set fits device budget -> dense (default parity)";
            return d;
        }

        if (pressure > 0.6 || (!r.gpu_available && r.qubits >= 18))
        {
            const double auto_eps = std::min(0.02, 0.002 * std::max(1.0, pressure));
            const bool prefer_ras = r.qubits >= 20;
            d.backend = prefer_ras ? StateBackend::Ras : StateBackend::Sparse;
            d.epsilon = auto_eps;
            d.reason = "auto: memory pressure " +
                       std::to_string(int(pressure * 100)) + "% of budget";
            if (!r.gpu_available && r.qubits >= 18)
                d.reason += " / no GPU and n>=18";
            d.reason += prefer_ras ? " / n>=20 -> RAS low-rank" : " -> sparse";
            if (!small_n && pressure > 2.0)
                d.suggested_batch = std::max<size_t>(1, r.batch_size / 2);
            return d;
        }

        d.backend = StateBackend::Dense;
        d.reason = "auto: dense fits budget";
        return d;
    }
}