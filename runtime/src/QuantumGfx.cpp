#ifndef RVDL_RT_BUILD
#define RVDL_RT_BUILD
#endif

#include "qhal/QuantumGfx.hpp"
#include "qhal/IQuantumBackend.hpp"

#include <algorithm>
#include <cmath>
#include <complex>
#include <vector>

namespace
{
    double phase_to_hue(double phase)
    {
        constexpr double two_pi = 6.28318530717958647692;
        double h = std::atan2(std::sin(phase), std::cos(phase));
        if (h < 0.0)
            h += two_pi;
        return h * (360.0 / two_pi);
    }
}

extern "C"
{
    RVDL_HOST_EXPORT uint64_t rvdl_qgfx_frame_new(void)
    {
        return qhal::qgfx_alloc_frame();
    }

    RVDL_HOST_EXPORT int32_t rvdl_qgfx_frame_capture(uint64_t handle, void *backend,
                                                    int32_t num_qubits)
    {
        qhal::QuantumFrame *frame = qhal::qgfx_resolve(handle);
        if (!frame)
            return 0;
        if (num_qubits <= 0)
        {
            frame->set_amplitudes({}, 1);
            return 0;
        }
        auto *be = static_cast<qhal::IQuantumBackend *>(backend);
        if (!be)
        {
            frame->set_amplitudes({}, static_cast<uint32_t>(num_qubits));
            return 0;
        }
        const std::vector<qhal::qcomplex> amps = be->get_state_vector();
        frame->set_amplitudes(amps, static_cast<uint32_t>(num_qubits));
        return static_cast<int32_t>(amps.size());
    }

    RVDL_HOST_EXPORT void rvdl_qgfx_frame_set_amplitudes(uint64_t handle, const double *re_im,
                                                        int32_t count, int32_t num_qubits)
    {
        qhal::QuantumFrame *frame = qhal::qgfx_resolve(handle);
        if (!frame || !re_im || count <= 0)
            return;
        const size_t n = std::min<size_t>(size_t(count) / 2,
                                          size_t(1) << std::min(num_qubits, 24));
        std::vector<qhal::qcomplex> amps;
        amps.reserve(n);
        for (size_t i = 0; i < n; ++i)
            amps.emplace_back(re_im[2 * i], re_im[2 * i + 1]);
        frame->set_amplitudes(std::move(amps), static_cast<uint32_t>(num_qubits));
    }

    RVDL_HOST_EXPORT void rvdl_qgfx_frame_release(uint64_t handle)
    {
        qhal::qgfx_release_frame(handle);
    }

    RVDL_HOST_EXPORT double rvdl_qgfx_frame_residual(uint64_t handle)
    {
        const qhal::QuantumFrame *f = qhal::qgfx_resolve(handle);
        return f ? f->normalization_residual() : 1.0;
    }

    RVDL_HOST_EXPORT int32_t rvdl_qgfx_frame_num_qubits(uint64_t handle)
    {
        const qhal::QuantumFrame *f = qhal::qgfx_resolve(handle);
        return f ? static_cast<int32_t>(f->num_qubits()) : 0;
    }

    RVDL_HOST_EXPORT int32_t rvdl_qgfx_frame_basis_size(uint64_t handle)
    {
        const qhal::QuantumFrame *f = qhal::qgfx_resolve(handle);
        return f ? static_cast<int32_t>(f->hilbert_dim()) : 0;
    }

    RVDL_HOST_EXPORT const char *rvdl_qgfx_projection_name(int32_t projection)
    {
        switch (projection)
        {
        case 0: return "local_bloch";
        case 1: return "quaternionic";
        case 2: return "entanglement_sphere";
        case 3: return "nested";
        case 4: return "hypersphere";
        case 5: return "wigner";
        case 6: return "density";
        default: return "unknown";
        }
    }

    RVDL_HOST_EXPORT double rvdl_qgfx_projection_source_dims(int32_t num_qubits)
    {
        return qhal::qgf_state_real_dims(static_cast<uint32_t>(num_qubits < 1 ? 1 : num_qubits));
    }

    RVDL_HOST_EXPORT double rvdl_qgfx_projection_target_dims(int32_t projection,
                                                            int32_t num_qubits)
    {
        return qhal::qgf_projection_target_dims(
            static_cast<qhal::QgfProjection>(projection),
            static_cast<uint32_t>(num_qubits < 1 ? 1 : num_qubits));
    }

    RVDL_HOST_EXPORT double rvdl_qgfx_concurrence(uint64_t handle)
    {
        const qhal::QuantumFrame *f = qhal::qgfx_resolve(handle);
        return f ? f->concurrence() : 0.0;
    }

    RVDL_HOST_EXPORT int32_t rvdl_qgfx_quaternionic(uint64_t handle, double *out4)
    {
        const qhal::QuantumFrame *f = qhal::qgfx_resolve(handle);
        if (!f)
            return 0;
        const qhal::QuaternionicCoords q = f->quaternionic_coords();
        if (!q.valid)
            return 0;
        if (out4)
        {
            out4[0] = q.theta;
            out4[1] = q.phi;
            out4[2] = q.quaternionic;
            out4[3] = q.global_phase;
        }
        return 1;
    }

    RVDL_HOST_EXPORT double rvdl_qgfx_entropy(uint64_t handle, int32_t first_qubit,
                                             int32_t count, int32_t whole)
    {
        const qhal::QuantumFrame *f = qhal::qgfx_resolve(handle);
        if (!f)
            return 0.0;
        if (whole)
            return f->von_neumann_entropy(0, f->num_qubits());
        if (first_qubit < 0 || count <= 0)
            return 0.0;
        return f->von_neumann_entropy(static_cast<uint32_t>(first_qubit),
                                      static_cast<uint32_t>(count));
    }

    RVDL_HOST_EXPORT int32_t rvdl_qgfx_barcode(uint64_t handle, int32_t first_qubit,
                                              int32_t count, double *out_bars, int32_t max_bars)
    {
        const qhal::QuantumFrame *f = qhal::qgfx_resolve(handle);
        if (!f || first_qubit < 0 || count <= 0)
            return 0;
        std::vector<qhal::QgfBar> bars;
        qhal::EntanglementBarcode::of_frame(
            *f, static_cast<uint32_t>(first_qubit), static_cast<uint32_t>(count), bars);
        if (out_bars && max_bars > 0)
        {
            const int32_t n = std::min<int32_t>(max_bars, static_cast<int32_t>(bars.size()));
            for (int32_t i = 0; i < n; ++i)
            {
                out_bars[2 * i] = bars[i].birth;
                out_bars[2 * i + 1] = bars[i].death;
            }
        }
        return static_cast<int32_t>(bars.size());
    }

    RVDL_HOST_EXPORT int32_t rvdl_qgfx_same_class(uint64_t a, uint64_t b, int32_t first_qubit,
                                                 int32_t count, double tol)
    {
        const qhal::QuantumFrame *fa = qhal::qgfx_resolve(a);
        const qhal::QuantumFrame *fb = qhal::qgfx_resolve(b);
        if (!fa || !fb || first_qubit < 0 || count <= 0)
            return 0;
        std::vector<qhal::QgfBar> ba, bb;
        qhal::EntanglementBarcode::of_frame(
            *fa, static_cast<uint32_t>(first_qubit), static_cast<uint32_t>(count), ba);
        qhal::EntanglementBarcode::of_frame(
            *fb, static_cast<uint32_t>(first_qubit), static_cast<uint32_t>(count), bb);
        return qhal::EntanglementBarcode::same_class(ba, bb, tol) ? 1 : 0;
    }

    RVDL_HOST_EXPORT double rvdl_qgfx_amplitude(uint64_t handle, int32_t basis, int32_t peaks)
    {
        const qhal::QuantumFrame *f = qhal::qgfx_resolve(handle);
        if (!f || basis < 0 || size_t(basis) >= f->hilbert_dim())
            return 0.0;
        return qhal::QuantumFrame::dirichlet(qhal::qgf_detail::kPi * basis, peaks);
    }

    RVDL_HOST_EXPORT double rvdl_qgfx_amplitude_phase(uint64_t handle, int32_t basis)
    {
        const qhal::QuantumFrame *f = qhal::qgfx_resolve(handle);
        if (!f || basis < 0)
            return 0.0;
        return f->phase(size_t(basis));
    }

    RVDL_HOST_EXPORT double rvdl_qgfx_interference(uint64_t handle, int32_t i, int32_t j)
    {
        const qhal::QuantumFrame *f = qhal::qgfx_resolve(handle);
        if (!f || i < 0 || j < 0)
            return 0.0;
        return (f->amplitude(size_t(i)) * std::conj(f->amplitude(size_t(j)))).real();
    }

    RVDL_HOST_EXPORT double rvdl_qgfx_phase_hue(double phase_radians)
    {
        const double h = phase_to_hue(phase_radians);
        return (h >= 360.0) ? 0.0 : h;
    }

    RVDL_HOST_EXPORT int32_t rvdl_qgfx_nested_shells(int32_t num_qubits)
    {
        return static_cast<int32_t>(
            qhal::QuantumFrame::nested_shell_count(static_cast<uint32_t>(num_qubits < 1 ? 1 : num_qubits)));
    }

    RVDL_HOST_EXPORT double rvdl_qgfx_nested_mass(uint64_t handle, int32_t shell,
                                                 int32_t total_shells)
    {
        const qhal::QuantumFrame *f = qhal::qgfx_resolve(handle);
        if (!f || shell < 0 || total_shells <= 0 || shell >= total_shells)
            return 0.0;
        const size_t dim = f->hilbert_dim();
        const size_t per = dim / size_t(total_shells);
        if (per == 0)
            return 0.0;
        const size_t lo = size_t(shell) * per;
        const size_t hi = (size_t(shell) + 1 == size_t(total_shells)) ? dim : lo + per;
        double acc = 0.0;
        for (size_t i = lo; i < hi && i < dim; ++i)
            acc += f->probability(i);
        return acc;
    }

    RVDL_HOST_EXPORT void rvdl_qgfx_bloch(uint64_t handle, int32_t qubit, double *out2)
    {
        const qhal::QuantumFrame *f = qhal::qgfx_resolve(handle);
        if (!f || !out2)
            return;
        f->local_bloch_polar(static_cast<uint32_t>(qubit < 0 ? 0 : qubit), out2[0], out2[1]);
    }

    RVDL_HOST_EXPORT double rvdl_qgfx_probability(uint64_t handle, int32_t basis)
    {
        const qhal::QuantumFrame *f = qhal::qgfx_resolve(handle);
        if (!f || basis < 0 || size_t(basis) >= f->hilbert_dim())
            return 0.0;
        return f->probability(size_t(basis));
    }
}