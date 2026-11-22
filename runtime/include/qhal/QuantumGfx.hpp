#pragma once
#include "RuntimeApi.h"
#include "qhal/QuantumState.hpp"

#include <cstdint>

#ifndef RVDL_HOST_EXPORT
#define RVDL_HOST_EXPORT extern "C" RVDL_RT_API
#endif

namespace qhal
{
    struct QuantumGfxState
    {
        int32_t initialized = 0;
        std::vector<QuantumFrame> frames;
        std::vector<uint32_t> generations;
    };

    inline QuantumGfxState &qgfx_state()
    {
        static QuantumGfxState s;
        return s;
    }

    inline uint64_t qgfx_alloc_frame()
    {
        auto &st = qgfx_state();
        for (size_t i = 0; i < st.frames.size(); ++i)
        {
            if (st.generations[i] == 0)
            {
                st.generations[i] = 1;
                return (uint64_t(1) << 48) | uint64_t(i);
            }
        }
        st.frames.emplace_back();
        st.generations.push_back(1);
        return (uint64_t(1) << 48) | uint64_t(st.frames.size() - 1);
    }

    inline QuantumFrame *qgfx_resolve(uint64_t handle)
    {
        if (handle == 0)
            return nullptr;
        if ((handle >> 48) != 1)
            return nullptr;
        auto &st = qgfx_state();
        const size_t idx = size_t(handle & 0xFFFFFFFFFFFFull);
        if (idx >= st.frames.size() || st.generations[idx] == 0)
            return nullptr;
        return &st.frames[idx];
    }

    inline const QuantumFrame *qgfx_resolve_const(uint64_t handle)
    {
        return qgfx_resolve(handle);
    }

    inline void qgfx_release_frame(uint64_t handle)
    {
        if (!qgfx_resolve(handle))
            return;
        auto &st = qgfx_state();
        const size_t idx = size_t(handle & 0xFFFFFFFFFFFFull);
        st.generations[idx] = 0;
    }
}

extern "C"
{

    RVDL_HOST_EXPORT uint64_t rvdl_qgfx_frame_new(void);

    RVDL_HOST_EXPORT int32_t rvdl_qgfx_frame_capture(uint64_t handle, void *backend,
                                                    int32_t num_qubits);

    RVDL_HOST_EXPORT void rvdl_qgfx_frame_set_amplitudes(uint64_t handle, const double *re_im,
                                                        int32_t count, int32_t num_qubits);

    RVDL_HOST_EXPORT void rvdl_qgfx_frame_release(uint64_t handle);


    RVDL_HOST_EXPORT double rvdl_qgfx_frame_residual(uint64_t handle);

    RVDL_HOST_EXPORT int32_t rvdl_qgfx_frame_num_qubits(uint64_t handle);
    RVDL_HOST_EXPORT int32_t rvdl_qgfx_frame_basis_size(uint64_t handle);

    RVDL_HOST_EXPORT const char *rvdl_qgfx_projection_name(int32_t projection);
    RVDL_HOST_EXPORT double rvdl_qgfx_projection_source_dims(int32_t num_qubits);
    RVDL_HOST_EXPORT double rvdl_qgfx_projection_target_dims(int32_t projection,
                                                            int32_t num_qubits);


    RVDL_HOST_EXPORT double rvdl_qgfx_concurrence(uint64_t handle);

    RVDL_HOST_EXPORT int32_t rvdl_qgfx_quaternionic(uint64_t handle, double *out4);

    RVDL_HOST_EXPORT double rvdl_qgfx_entropy(uint64_t handle, int32_t first_qubit,
                                             int32_t count, int32_t whole);

    RVDL_HOST_EXPORT int32_t rvdl_qgfx_barcode(uint64_t handle, int32_t first_qubit,
                                              int32_t count, double *out_bars, int32_t max_bars);

    RVDL_HOST_EXPORT int32_t rvdl_qgfx_same_class(uint64_t a, uint64_t b, int32_t first_qubit,
                                                 int32_t count, double tol);

    RVDL_HOST_EXPORT double rvdl_qgfx_amplitude(uint64_t handle, int32_t basis, int32_t peaks);

    RVDL_HOST_EXPORT double rvdl_qgfx_amplitude_phase(uint64_t handle, int32_t basis);

    RVDL_HOST_EXPORT double rvdl_qgfx_interference(uint64_t handle, int32_t i, int32_t j);

    RVDL_HOST_EXPORT double rvdl_qgfx_phase_hue(double phase_radians);


    RVDL_HOST_EXPORT int32_t rvdl_qgfx_nested_shells(int32_t num_qubits);

    RVDL_HOST_EXPORT double rvdl_qgfx_nested_mass(uint64_t handle, int32_t shell,
                                                 int32_t total_shells);

    RVDL_HOST_EXPORT void rvdl_qgfx_bloch(uint64_t handle, int32_t qubit, double *out2);

    RVDL_HOST_EXPORT double rvdl_qgfx_probability(uint64_t handle, int32_t basis);
}