#include "test_framework.hpp"
#include "qhal/QuantumState.hpp"
#include "qhal/QuantumGfx.hpp"
#include "qhal/QuantumUi.hpp"

#include <cmath>

using namespace qhal;

static const double kSqrtHalf = std::sqrt(0.5);

static void set_bell(uint64_t h)
{
    const double amp[8] = {kSqrtHalf, 0.0, 0.0, 0.0, 0.0, 0.0, kSqrtHalf, 0.0};
    rvdl_qgfx_frame_set_amplitudes(h, amp, 8, 2);
}

static double collision_of(uint64_t h)
{
    const QuantumFrame *f = qgfx_resolve_const(h);
    return f ? f->collision_probability() : -1.0;
}

static double linear_entropy_of(uint64_t h)
{
    const QuantumFrame *f = qgfx_resolve_const(h);
    return f ? f->linear_entropy() : -1.0;
}

static bool entangled(uint64_t h, uint32_t qa, uint32_t qb)
{
    const QuantumFrame *f = qgfx_resolve_const(h);
    return f ? f->entanglement_exists(qa, qb) : false;
}

static double pair_strength(uint64_t h, uint32_t qa, uint32_t qb)
{
    const QuantumFrame *f = qgfx_resolve_const(h);
    return f ? f->pair_entanglement(qa, qb) : -1.0;
}


QTEST(qgfx_frame_lifecycle) {
    uint64_t h = rvdl_qgfx_frame_new();
    QCHECK(h != 0);
    QCHECK(rvdl_qgfx_frame_num_qubits(h) == 1);
    QCHECK(rvdl_qgfx_frame_basis_size(h) == 2);
    QCHECK_NEAR(rvdl_qgfx_frame_residual(h), 1.0, 1e-12);
    QCHECK_NEAR(rvdl_qgfx_probability(h, 0), 0.0, 1e-12);
    rvdl_qgfx_frame_release(h);
}

QTEST(qgfx_stale_handle_is_safe) {
    uint64_t h = rvdl_qgfx_frame_new();
    rvdl_qgfx_frame_release(h);
    QCHECK(qgfx_resolve_const(h) == nullptr);
    QCHECK(rvdl_qgfx_frame_num_qubits(h) == 0);
    QCHECK(rvdl_qgfx_frame_basis_size(h) == 0);
    QCHECK_NEAR(rvdl_qgfx_frame_residual(h), 1.0, 1e-12);
    QCHECK_NEAR(rvdl_qgfx_concurrence(h), 0.0, 1e-12);
    QCHECK(rvdl_qgfx_quaternionic(h, nullptr) == 0);
    rvdl_qgfx_frame_release(h);
    QCHECK(rvdl_qgfx_frame_num_qubits(0xDEADBEEF) == 0);
    QCHECK(rvdl_qgfx_frame_num_qubits(0) == 0);
}

QTEST(qgfx_slot_reuse_after_release) {
    uint64_t a = rvdl_qgfx_frame_new();
    rvdl_qgfx_frame_release(a);
    uint64_t b = rvdl_qgfx_frame_new();
    QCHECK(b != 0);
    QCHECK(qgfx_resolve_const(b) != nullptr);
    rvdl_qgfx_frame_release(b);
}

QTEST(qgfx_capture_without_backend) {
    uint64_t h = rvdl_qgfx_frame_new();
    QCHECK(rvdl_qgfx_frame_capture(h, nullptr, 3) == 0);
    QCHECK(rvdl_qgfx_frame_num_qubits(h) == 3);
    QCHECK(rvdl_qgfx_frame_basis_size(h) == 8);
    QCHECK_NEAR(rvdl_qgfx_frame_residual(h), 1.0, 1e-12);
    QCHECK(rvdl_qgfx_frame_capture(h, nullptr, 0) == 0);
    QCHECK(rvdl_qgfx_frame_num_qubits(h) == 1);
    rvdl_qgfx_frame_release(h);
}

QTEST(qgfx_amplitude_clamping) {
    uint64_t h = rvdl_qgfx_frame_new();
    const double amp[4] = {2.0, 0.0, 0.0, 0.0};
    rvdl_qgfx_frame_set_amplitudes(h, amp, 2, 2);
    QCHECK(rvdl_qgfx_frame_residual(h) > 0.5);
    const double many[32] = {1.0};
    rvdl_qgfx_frame_set_amplitudes(h, many, 32, 2);
    QCHECK(rvdl_qgfx_frame_basis_size(h) == 4);
    QCHECK(rvdl_qgfx_probability(h, 999) == 0.0);
    QCHECK(rvdl_qgfx_amplitude(h, 999, 4) == 0.0);
    rvdl_qgfx_frame_set_amplitudes(h, nullptr, 8, 2);
    rvdl_qgfx_bloch(h, 0, nullptr);
    rvdl_qgfx_frame_release(h);
}


QTEST(qgfx_projection_introspection) {
    QCHECK_NEAR(rvdl_qgfx_projection_source_dims(1), 2.0, 1e-12);
    QCHECK_NEAR(rvdl_qgfx_projection_source_dims(2), 6.0, 1e-12);
    QCHECK_NEAR(rvdl_qgfx_projection_source_dims(3), 14.0, 1e-12);

    const double src2 = rvdl_qgfx_projection_source_dims(2);
    QCHECK_NEAR(src2, 6.0, 1e-12);
    for (int p = 0; p <= 6; ++p) {
        const double dst = rvdl_qgfx_projection_target_dims(p, 2);
        QCHECK(dst > 0.0);
        QCHECK(dst <= src2);
        QCHECK(std::string(rvdl_qgfx_projection_name(p)) != "unknown");
    }

    QCHECK_NEAR(rvdl_qgfx_projection_target_dims(0, 2), 6.0, 1e-12);
    QCHECK_NEAR(rvdl_qgfx_projection_target_dims(2, 2), 6.0, 1e-12);

    QCHECK_NEAR(rvdl_qgfx_projection_target_dims(5, 2), 2.0, 1e-12);
    QCHECK_NEAR(rvdl_qgfx_projection_target_dims(3, 2), 3.0, 1e-12);
    QCHECK_NEAR(rvdl_qgfx_projection_target_dims(4, 2), 3.0, 1e-12);

    QCHECK_NEAR(rvdl_qgfx_projection_source_dims(3), 14.0, 1e-12);
    QCHECK_NEAR(rvdl_qgfx_projection_target_dims(0, 3), 9.0, 1e-12);
    QCHECK(rvdl_qgfx_projection_target_dims(0, 3) < 14.0);
    QCHECK_NEAR(rvdl_qgfx_projection_target_dims(0, 2), 6.0, 1e-12);

    for (int p = 0; p <= 6; ++p)
        QCHECK(std::string(rvdl_qgfx_projection_name(p)) != "unknown");
    QCHECK(std::string(rvdl_qgfx_projection_name(99)) == "unknown");

    QCHECK(rvdl_qgfx_projection_source_dims(0) > 0.0);
}


QTEST(qgfx_predicates_on_bell) {
    uint64_t h = rvdl_qgfx_frame_new();
    set_bell(h);

    QCHECK_NEAR(collision_of(h), 0.5, 1e-9);
    QCHECK_NEAR(linear_entropy_of(h), 0.5, 1e-9);

    QCHECK_NEAR(rvdl_qgfx_concurrence(h), 1.0, 1e-9);
    QCHECK(entangled(h, 0, 1) == 1);

    double q[4] = {0, 0, 0, 0};
    QCHECK(rvdl_qgfx_quaternionic(h, q) == 1);
    QCHECK_NEAR(q[2], 0.5, 1e-9);
    QCHECK_NEAR(q[0], M_PI / 4.0, 1e-9);

    QCHECK_NEAR(rvdl_qgfx_entropy(h, 0, 1, 0), 1.0, 1e-9);
    QCHECK_NEAR(rvdl_qgfx_entropy(h, 1, 1, 0), 1.0, 1e-9);
    QCHECK_NEAR(rvdl_qgfx_entropy(h, 0, 2, 1), 0.0, 1e-9);

    double bp[2] = {0, 0};
    rvdl_qgfx_bloch(h, 0, bp);
    QCHECK_NEAR(bp[0], M_PI / 2.0, 1e-6);

    double bars[8] = {0};
    const int32_t nb = rvdl_qgfx_barcode(h, 0, 1, bars, 4);
    QCHECK(nb >= 1);
    QCHECK_NEAR(bars[0], 1.0, 1e-9);
    QCHECK_NEAR(bars[1], kSqrtHalf, 1e-9);

    QCHECK_NEAR(rvdl_qgfx_interference(h, 0, 3), 0.5, 1e-9);
    QCHECK_NEAR(rvdl_qgfx_interference(h, 0, 1), 0.0, 1e-12);

    QCHECK(rvdl_qgfx_barcode(h, 0, 1, nullptr, 0) == nb);

    rvdl_qgfx_frame_release(h);
}

QTEST(qgfx_separable_predicate) {
    uint64_t h = rvdl_qgfx_frame_new();
    const double amp[8] = {1.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
    rvdl_qgfx_frame_set_amplitudes(h, amp, 8, 2);
    double q[4] = {9, 9, 9, 9};
    QCHECK(rvdl_qgfx_quaternionic(h, q) == 1);
    QCHECK_NEAR(q[2], 0.0, 1e-12);
    QCHECK_NEAR(rvdl_qgfx_concurrence(h), 0.0, 1e-12);
    QCHECK_NEAR(rvdl_qgfx_entropy(h, 0, 1, 0), 0.0, 1e-12);
    double bp[2] = {9, 9};
    rvdl_qgfx_bloch(h, 0, bp);
    QCHECK_NEAR(bp[0], 0.0, 1e-9);
    rvdl_qgfx_frame_release(h);
}

QTEST(qgfx_quaternionic_invalid_for_wrong_size) {
    uint64_t h = rvdl_qgfx_frame_new();
    const double amp[4] = {kSqrtHalf, 0.0, kSqrtHalf, 0.0};
    rvdl_qgfx_frame_set_amplitudes(h, amp, 4, 2);
    const double amp3[16] = {1.0};
    rvdl_qgfx_frame_set_amplitudes(h, amp3, 2, 3);
    double q[4] = {0, 0, 0, 0};
    QCHECK(rvdl_qgfx_quaternionic(h, q) == 0);
    QCHECK_NEAR(rvdl_qgfx_concurrence(h), 0.0, 1e-12);
    rvdl_qgfx_frame_release(h);
}

QTEST(qgfx_same_class_equivalence) {
    uint64_t a = rvdl_qgfx_frame_new();
    uint64_t b = rvdl_qgfx_frame_new();
    const double amp_a[8] = {0.6, 0.0, 0.0, 0.0, 0.0, 0.0, 0.8, 0.0};
    const double amp_b[8] = {0.8, 0.0, 0.0, 0.0, 0.0, 0.0, 0.6, 0.0};
    rvdl_qgfx_frame_set_amplitudes(a, amp_a, 8, 2);
    rvdl_qgfx_frame_set_amplitudes(b, amp_b, 8, 2);
    QCHECK_NEAR(rvdl_qgfx_concurrence(a), rvdl_qgfx_concurrence(b), 1e-9);
    QCHECK(rvdl_qgfx_same_class(a, b, 0, 1, 1e-6) == 1);

    const double prod[8] = {1.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
    rvdl_qgfx_frame_set_amplitudes(b, prod, 8, 2);
    QCHECK(rvdl_qgfx_same_class(a, b, 0, 1, 1e-6) == 0);

    rvdl_qgfx_frame_release(b);
    QCHECK(rvdl_qgfx_same_class(a, b, 0, 1, 1e-6) == 0);
    rvdl_qgfx_frame_release(a);
}


QTEST(qgfx_dirichlet_kernel) {
    QCHECK_NEAR(QuantumFrame::dirichlet(0.0, 4), 9.0, 1e-6);
    QCHECK_NEAR(QuantumFrame::dirichlet(0.0, 0), 1.0, 1e-12);
    QCHECK_NEAR(QuantumFrame::dirichlet(0.0, 1), 3.0, 1e-6);
    QCHECK_NEAR(QuantumFrame::dirichlet(0.7, 4), QuantumFrame::dirichlet(-0.7, 4), 1e-12);
    QCHECK_NEAR(QuantumFrame::dirichlet(2.0 * M_PI / 7.0, 3), 0.0, 1e-9);
    const double lobe = std::fabs(QuantumFrame::dirichlet(0.0, 8));
    const double side = std::fabs(QuantumFrame::dirichlet(2.0 * M_PI / 17.0, 8));
    QCHECK(lobe > side * 5.0);
}

QTEST(qgfx_phase_is_carried) {
    const double real_only[8] = {kSqrtHalf, 0.0, kSqrtHalf, 0.0,
                                  0.0, 0.0, 0.0, 0.0};
    const double with_phase[8] = {kSqrtHalf, 0.0, 0.0, kSqrtHalf,
                                  0.0, 0.0, 0.0, 0.0};
    uint64_t a = rvdl_qgfx_frame_new();
    uint64_t b = rvdl_qgfx_frame_new();
    rvdl_qgfx_frame_set_amplitudes(a, real_only, 8, 2);
    rvdl_qgfx_frame_set_amplitudes(b, with_phase, 8, 2);

    QCHECK_NEAR(rvdl_qgfx_probability(a, 0), rvdl_qgfx_probability(b, 0), 1e-12);
    QCHECK_NEAR(rvdl_qgfx_probability(a, 1), rvdl_qgfx_probability(b, 1), 1e-12);
    QCHECK_NEAR(rvdl_qgfx_amplitude_phase(a, 1), 0.0, 1e-12);
    QCHECK_NEAR(rvdl_qgfx_amplitude_phase(b, 1), M_PI / 2.0, 1e-12);
    double pa[2] = {0, 0}, pb[2] = {0, 0};
    rvdl_qgfx_bloch(a, 0, pa);
    rvdl_qgfx_bloch(b, 0, pb);
    QCHECK_NEAR(pa[0], M_PI / 2.0, 1e-9);
    QCHECK_NEAR(pb[0], M_PI / 2.0, 1e-9);
    QCHECK_NEAR(pa[1], 0.0, 1e-9);
    QCHECK_NEAR(pb[1], M_PI / 2.0, 1e-9);
    QCHECK(std::fabs(pa[1] - pb[1]) > 1e-6);
    rvdl_qgfx_frame_release(a);
    rvdl_qgfx_frame_release(b);
}

QTEST(qgfx_phase_hue_is_perceptual) {
    QCHECK(rvdl_qgfx_phase_hue(0.0) >= 0.0 && rvdl_qgfx_phase_hue(0.0) < 360.0);
    QCHECK(rvdl_qgfx_phase_hue(M_PI) >= 0.0 && rvdl_qgfx_phase_hue(M_PI) < 360.0);
    QCHECK_NEAR(rvdl_qgfx_phase_hue(M_PI), rvdl_qgfx_phase_hue(-M_PI), 1e-9);
    {
        const double a = rvdl_qgfx_phase_hue(0.0);
        const double b = rvdl_qgfx_phase_hue(2.0 * M_PI);
        const double circ = std::fabs(a - b);
        QCHECK_NEAR(std::min(circ, 360.0 - circ), 0.0, 1e-6);
    }
    QCHECK(rvdl_qgfx_phase_hue(1e-15) < 360.0);
    QCHECK(rvdl_qgfx_phase_hue(-1e-15) < 360.0);
    {
        const double h0 = rvdl_qgfx_phase_hue(0.0);
        const double h1 = rvdl_qgfx_phase_hue(2.0 * M_PI / 3.0);
        const double h2 = rvdl_qgfx_phase_hue(4.0 * M_PI / 3.0);
        auto arc = [](double from, double to) {
            const double d = std::fabs(to - from);
            return std::min(d, 360.0 - d);
        };
        QCHECK_NEAR(arc(h0, h1), 120.0, 1e-6);
        QCHECK_NEAR(arc(h1, h2), 120.0, 1e-6);
        QCHECK_NEAR(arc(h2, h0), 120.0, 1e-6);
    }
}


QTEST(qgfx_nested_hierarchy) {
    QCHECK(rvdl_qgfx_nested_shells(1) == 1);
    QCHECK(rvdl_qgfx_nested_shells(2) == 2);
    QCHECK(rvdl_qgfx_nested_shells(3) == 4);
    QCHECK(rvdl_qgfx_nested_shells(4) == 8);
    QCHECK(rvdl_qgfx_nested_shells(20) == 524288);

    uint64_t h = rvdl_qgfx_frame_new();
    set_bell(h);
    double total = 0.0;
    for (int32_t s = 0; s < 2; ++s)
        total += rvdl_qgfx_nested_mass(h, s, 2);
    QCHECK_NEAR(total, 1.0, 1e-9);
    QCHECK(rvdl_qgfx_nested_mass(h, 5, 2) == 0.0);
    QCHECK(rvdl_qgfx_nested_mass(h, -1, 2) == 0.0);
    rvdl_qgfx_frame_release(h);
}


QTEST(qqui_headless_no_crash) {
    uint64_t h = rvdl_qgfx_frame_new();
    set_bell(h);

    rvdl_qqui_qubit_glyph(0, 0, 24, h, 0, 0xFFFFFF, 255);
    rvdl_qqui_qubit_row(0, 0, 24, h, 0, 4, 0xFFFFFF, 255);
    const double link = rvdl_qqui_entangle_link(0, 0, 100, 0, h, 0, 1, 0xFF0000, 255);
    QCHECK_NEAR(link, 1.0, 1e-9);

    rvdl_qqui_uncertainty_bin(0, 100, 20, 0.5, 0.1, 0x00FF00, 255);
    rvdl_qqui_wigner_slice(0, 0, 64, 64, h, 0, 16);
    rvdl_qqui_nested_bar(0, 0, 8, h, 0x0000FF, 255);
    QCHECK(rvdl_qqui_amplitude_strip(0, 0, 100, 50, h, 0xFFFFFF, 255) == -1);
    rvdl_qqui_state_card(0, 0, 200, h, 1, 0xFFFFFF, 255);

    rvdl_qqui_qubit_glyph(0, 0, 0, h, 0, 0xFFFFFF, 255);
    rvdl_qqui_qubit_glyph(0, 0, -5, h, -1, 0xFFFFFF, 255);
    rvdl_qqui_uncertainty_bin(0, 0, 0, 0.0, 0.0, 0, 0);
    rvdl_qqui_wigner_slice(0, 0, 0, 0, h, 0, 16);
    rvdl_qqui_wigner_slice(0, 0, 64, 64, h, 0, 0);
    rvdl_qqui_wigner_slice(0, 0, 64, 64, h, 0, 9999);
    rvdl_qqui_nested_bar(0, 0, 0, h, 0, 0);
    rvdl_qqui_state_card(0, 0, 0, h, 0, 0, 0);

    rvdl_qgfx_frame_release(h);
    rvdl_qqui_qubit_glyph(0, 0, 24, h, 0, 0xFFFFFF, 255);
    QCHECK(rvdl_qqui_entangle_link(0, 0, 100, 0, h, 0, 1, 0xFF0000, 255) == 0.0);
    QCHECK(rvdl_qqui_amplitude_strip(0, 0, 100, 50, h, 0, 0) == -1);
    rvdl_qqui_nested_bar(0, 0, 8, h, 0, 0);
    rvdl_qqui_state_card(0, 0, 200, h, 1, 0, 0);
}

QTEST(qqui_link_edge_cases) {
    uint64_t h = rvdl_qgfx_frame_new();
    set_bell(h);
    QCHECK(rvdl_qqui_entangle_link(0, 0, 100, 0, h, 1, 1, 0xFF0000, 255) == 0.0);
    const double prod[8] = {1.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
    rvdl_qgfx_frame_set_amplitudes(h, prod, 8, 2);
    QCHECK(rvdl_qqui_entangle_link(0, 0, 100, 0, h, 0, 1, 0xFF0000, 255) == 0.0);
    rvdl_qgfx_frame_release(h);
}


QTEST(qgfx_three_qubit_ghz) {
    uint64_t h = rvdl_qgfx_frame_new();
    const double amp[16] = {kSqrtHalf, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0,
                             0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, kSqrtHalf};
    rvdl_qgfx_frame_set_amplitudes(h, amp, 16, 3);
    QCHECK(rvdl_qgfx_frame_basis_size(h) == 8);
    QCHECK_NEAR(rvdl_qgfx_frame_residual(h), 0.0, 1e-12);
    QCHECK_NEAR(rvdl_qgfx_entropy(h, 0, 3, 1), 0.0, 1e-9);
    for (int32_t q = 0; q < 3; ++q)
        QCHECK_NEAR(rvdl_qgfx_entropy(h, q, 1, 0), 1.0, 1e-9);
    double bars[8] = {0};
    const int32_t nb = rvdl_qgfx_barcode(h, 0, 1, bars, 4);
    QCHECK(nb >= 1);
    QCHECK_NEAR(bars[1], kSqrtHalf, 1e-9);
    QCHECK_NEAR(rvdl_qgfx_concurrence(h), 0.0, 1e-12);
    rvdl_qgfx_frame_release(h);
}

    QTEST(qgfx_n_qubit_pair_conventions) {
    uint64_t h = rvdl_qgfx_frame_new();
    const double amp[16] = {kSqrtHalf, 0.0,
                             0.0, 0.0,
                             0.0, 0.0,
                             0.0, 0.0,
                             0.0, 0.0,
                             kSqrtHalf, 0.0,
                             0.0, 0.0,
                             0.0, 0.0};
    rvdl_qgfx_frame_set_amplitudes(h, amp, 16, 3);
    QCHECK(rvdl_qgfx_frame_num_qubits(h) == 3);
    QCHECK_NEAR(rvdl_qgfx_frame_residual(h), 0.0, 1e-12);

    QCHECK_NEAR(rvdl_qgfx_concurrence(h), 0.0, 1e-12);
    QCHECK_NEAR(pair_strength(h, 0, 1), 0.0, 1e-12);
    QCHECK(entangled(h, 0, 1) == true);
    QCHECK(entangled(h, 0, 2) == false);

    const double prod[16] = {1.0};
    rvdl_qgfx_frame_set_amplitudes(h, prod, 16, 3);
    QCHECK(entangled(h, 0, 1) == false);
    QCHECK(entangled(h, 0, 2) == false);
    QCHECK_NEAR(pair_strength(h, 0, 1), 0.0, 1e-12);

    rvdl_qgfx_frame_release(h);
}

QTEST(qgfx_two_qubit_pair_strength) {
    uint64_t h = rvdl_qgfx_frame_new();
    set_bell(h);
    QCHECK_NEAR(pair_strength(h, 0, 1), 1.0, 1e-9);
    QCHECK(entangled(h, 0, 1) == true);

    const double prod[8] = {1.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
    rvdl_qgfx_frame_set_amplitudes(h, prod, 8, 2);
    QCHECK_NEAR(pair_strength(h, 0, 1), 0.0, 1e-12);
    QCHECK(entangled(h, 0, 1) == false);
    QCHECK_NEAR(rvdl_qqui_entangle_link(0, 0, 100, 0, h, 0, 1, 0xFF0000, 255), 0.0, 1e-12);
    QCHECK_NEAR(rvdl_qqui_entangle_link(0, 0, 100, 0, h, 1, 1, 0xFF0000, 255), 0.0, 1e-12);
    rvdl_qgfx_frame_release(h);
}

QTEST(qgfx_ghz_vs_w_spectrum) {
    uint64_t ghz = rvdl_qgfx_frame_new();
    uint64_t w = rvdl_qgfx_frame_new();
const double g_amp[16] = {kSqrtHalf, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0,
                               0.0, 0.0, 0.0, 0.0, 0.0, 0.0, kSqrtHalf, 0.0};
    const double wt = 1.0 / std::sqrt(3.0);
    const double w_amp[16] = {0.0, 0.0,
                               wt, 0.0,
                               wt, 0.0,
                               0.0, 0.0,
                               wt, 0.0,
                               0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
    rvdl_qgfx_frame_set_amplitudes(ghz, g_amp, 16, 3);
    rvdl_qgfx_frame_set_amplitudes(w, w_amp, 16, 3);
    QCHECK_NEAR(rvdl_qgfx_frame_residual(ghz), 0.0, 1e-12);
    QCHECK_NEAR(rvdl_qgfx_frame_residual(w), 0.0, 1e-12);

    for (int32_t q = 0; q < 3; ++q) {
        QCHECK_NEAR(rvdl_qgfx_entropy(ghz, q, 1, 0), 1.0, 1e-9);
        QCHECK_NEAR(rvdl_qgfx_entropy(w, q, 1, 0), 0.9182958342, 1e-8);
    }

    double gb[16], wb[16];
    const int32_t ng = rvdl_qgfx_barcode(ghz, 0, 1, gb, 8);
    const int32_t nw = rvdl_qgfx_barcode(w, 0, 1, wb, 8);
    QCHECK(ng == 2);
    QCHECK(nw == 2);
    QCHECK_NEAR(gb[1], kSqrtHalf, 1e-9);
    QCHECK_NEAR(wb[1], std::sqrt(2.0 / 3.0), 1e-9);
    QCHECK(rvdl_qgfx_same_class(ghz, w, 0, 1, 1e-6) == 0);


    uint64_t sep = rvdl_qgfx_frame_new();
    const double s_amp[16] = {kSqrtHalf, 0.0,
                               0.0, 0.0,
                               kSqrtHalf, 0.0,
                               0.0, 0.0,
                               0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
    rvdl_qgfx_frame_set_amplitudes(sep, s_amp, 16, 3);
    QCHECK_NEAR(rvdl_qgfx_frame_residual(sep), 0.0, 1e-12);

    for (int32_t q = 0; q < 3; ++q)
        QCHECK_NEAR(rvdl_qgfx_entropy(sep, q, 1, 0), 0.0, 1e-9);
    for (int32_t q = 0; q < 3; ++q)
        QCHECK_NEAR(rvdl_qgfx_entropy(ghz, q, 1, 0), 1.0, 1e-9);
    double sb[16];
    const int32_t ns = rvdl_qgfx_barcode(sep, 0, 1, sb, 8);
    QCHECK(ns == 1);
    QCHECK_NEAR(sb[1], 1.0, 1e-9);
    QCHECK(entangled(sep, 0, 1) == false);
    QCHECK(entangled(ghz, 0, 1) == true);
    QCHECK(rvdl_qgfx_same_class(sep, ghz, 0, 1, 1e-6) == 0);
    rvdl_qgfx_frame_release(sep);

    rvdl_qgfx_frame_release(ghz);
    rvdl_qgfx_frame_release(w);
}


QTEST(qgfx_frame_isolation) {
    uint64_t a = rvdl_qgfx_frame_new();
    uint64_t b = rvdl_qgfx_frame_new();
    set_bell(a);
    const double prod[8] = {1.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
    rvdl_qgfx_frame_set_amplitudes(b, prod, 8, 2);
    QCHECK_NEAR(rvdl_qgfx_concurrence(a), 1.0, 1e-9);
    QCHECK_NEAR(rvdl_qgfx_concurrence(b), 0.0, 1e-12);
    const double r1 = rvdl_qgfx_entropy(a, 0, 1, 0);
    const double r2 = rvdl_qgfx_entropy(a, 0, 1, 0);
    QCHECK_NEAR(r1, r2, 1e-15);
    rvdl_qgfx_frame_release(a);
    QCHECK_NEAR(rvdl_qgfx_concurrence(b), 0.0, 1e-12);
    rvdl_qgfx_frame_release(b);
}