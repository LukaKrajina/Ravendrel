#include "qlm/StatePolicy.hpp"
#include "qlm/SparseAdjoint.hpp"
#ifdef RVDL_REGRESSION_WITH_KOKKOS
#include "qlm/BatchAdjoint.hpp"
#endif

#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

using Complex = std::complex<double>;
using State = std::vector<Complex>;

static int g_checks = 0, g_failures = 0;
static void check(bool ok, const char *what)
{
    ++g_checks;
    if (!ok) { ++g_failures; std::fprintf(stderr, "FAIL: %s\n", what); }
}

[[maybe_unused]] static bool nearly(double a, double b, double eps = 1e-10)
{
    return std::fabs(a - b) <= eps * (1.0 + std::fabs(a) + std::fabs(b));
}

struct Gate
{
    int type;
    size_t q0, q1, param;
    double scale;
};

static void apply_fwd(State &s, const Gate &g, const std::vector<double> &theta)
{
    const double a = theta[g.param] * g.scale;
    if (g.type == 0)
    {
        const size_t m = size_t(1) << g.q0;
        const double c = std::cos(a / 2), sn = std::sin(a / 2);
        for (size_t i = 0; i < s.size(); ++i)
        {
            if (i & m) continue;
            const Complex x = s[i], y = s[i | m];
            s[i] = c * x - sn * y;
            s[i | m] = sn * x + c * y;
        }
    }
    else if (g.type == 1)
    {
        const size_t m = size_t(1) << g.q0;
        const Complex ph(std::cos(a), std::sin(a));
        for (size_t i = 0; i < s.size(); ++i) if (i & m) s[i] *= ph;
    }
    else
    {
        const size_t cm = size_t(1) << g.q0, tm = size_t(1) << g.q1;
        for (size_t i = 0; i < s.size(); ++i)
        {
            if (!(i & cm) || (i & tm)) continue;
            std::swap(s[i], s[i | tm]);
        }
    }
}

struct RefResult
{
    double loss = 0.0, acc = 0.0;
    std::vector<double> grad, pred;
};

static RefResult dense_reference(const std::vector<Gate> &gates, const State &ket,
                                 const std::vector<double> &theta,
                                 const std::vector<size_t> &bits, uint8_t target,
                                 size_t P)
{
    const size_t N = ket.size(), D = bits.size();
    State phi = ket, lambda(N, Complex(0.0, 0.0));
    for (const auto &g : gates) apply_fwd(phi, g, theta);

    RefResult r;
    r.pred.assign(D, 0.0);
    std::vector<double> gz(D, 0.0);
    r.grad.assign(P, 0.0);
    for (size_t j = 0; j < D; ++j)
    {
        double p1 = 0.0;
        for (size_t i = 0; i < N; ++i) if (i & (size_t(1) << bits[j])) p1 += std::norm(phi[i]);
        r.pred[j] = 1.0 - 2.0 * p1;
        const int bit = (target >> j) & 1;
        const double tgt = 1.0 - 2.0 * bit;
        const double d = r.pred[j] - tgt;
        r.loss += d * d;
        gz[j] = 2.0 * d / double(D);
        if (((r.pred[j] < 0.0) ? 1 : 0) == bit) r.acc += 1.0;
    }
    r.loss /= double(D);
    r.acc /= double(D);

    for (size_t i = 0; i < N; ++i)
    {
        double coef = 0.0;
        for (size_t j = 0; j < D; ++j)
            coef += gz[j] * (((i >> bits[j]) & 1) ? -1.0 : 1.0);
        lambda[i] = coef * phi[i];
    }

    for (size_t k = gates.size(); k > 0; --k)
    {
        const Gate &g = gates[k - 1];
        const size_t mask = size_t(1) << g.q0;
        double dot = 0.0;
        if (g.type == 1)
        {
            for (size_t s = 0; s < N; ++s)
                if (s & mask) dot += (std::conj(lambda[s]) * phi[s]).imag();
            r.grad[g.param] += -2.0 * g.scale * dot;
        }
        else if (g.type == 0)
        {
            for (size_t s = 0; s < N; ++s)
            {
                if (s & mask) continue;
                dot += (std::conj(lambda[s | mask]) * phi[s] -
                        std::conj(lambda[s]) * phi[s | mask]).real();
            }
            r.grad[g.param] += g.scale * dot;
        }
        Gate inv = g;
        inv.scale = -g.scale;
        apply_fwd(phi, inv, theta);
        apply_fwd(lambda, inv, theta);
    }
    return r;
}

static std::vector<Gate> make_circuit(size_t n, size_t layers, size_t &P)
{
    std::vector<Gate> gs;
    size_t param = 0;
    for (size_t l = 0; l < layers; ++l)
    {
        for (size_t q = 0; q < n; ++q)
            gs.push_back({0, q, 0, param++, 0.7 + 0.1 * double(l)});
        for (size_t q = 0; q < n; ++q)
            gs.push_back({1, q, 0, param++, 0.3 + 0.05 * double(l)});
        for (size_t q = 0; q + 1 < n; ++q)
            gs.push_back({2, q, q + 1, 0, 1.0});
    }
    P = param;
    return gs;
}

static State make_ket(size_t N, uint64_t seed)
{
    std::mt19937_64 rng(seed);
    std::normal_distribution<double> g(0.0, 1.0);
    State s(N);
    double norm = 0.0;
    for (auto &z : s) { z = Complex(g(rng), g(rng)); norm += std::norm(z); }
    const double inv = 1.0 / std::sqrt(norm);
    for (auto &z : s) z *= inv;
    return s;
}

static void test_policy()
{
    using namespace qlm;
    StateResources r;
    r.qubits = 16; r.batch_size = 32; r.host_memory_bytes = uint64_t(64) << 30;
    r.gpu_available = false; r.moe_experts = 1;

    check(select_state_backend(StateBackend::Dense, r, true, true).backend == StateBackend::Dense,
          "policy: manual Dense forced");
    check(select_state_backend(StateBackend::Sparse, r, true, true).backend == StateBackend::Sparse,
          "policy: manual Sparse forced");
    check(select_state_backend(StateBackend::Sparse, r, false, true).backend == StateBackend::Dense,
          "policy: Sparse without backend -> Dense fallback");
    check(select_state_backend(StateBackend::Auto, r, false, true).backend == StateBackend::Dense,
          "policy: Auto without backend -> Dense");

    const auto small = select_state_backend(StateBackend::Auto, r, true, true);
    check(small.backend == StateBackend::Dense && small.epsilon == 0.0,
          "policy: small n + ample RAM -> Dense exact");
    check(!small.reason.empty(), "policy: reason recorded");

    r.host_memory_bytes = uint64_t(1) << 26;
    const auto tight = select_state_backend(StateBackend::Auto, r, true, true);
    check(tight.backend == StateBackend::Sparse && tight.epsilon > 0.0 && tight.epsilon <= 0.02,
          "policy: memory pressure -> bounded Sparse epsilon");

    r.host_memory_bytes = uint64_t(64) << 30;
    r.qubits = 18;
    check(select_state_backend(StateBackend::Auto, r, true, true).backend == StateBackend::Sparse,
          "policy: n=18 / no GPU -> DQ-Sparse");
    r.qubits = 20;
    const auto big = select_state_backend(StateBackend::Auto, r, true, true);
    check(big.backend == StateBackend::Ras && big.epsilon > 0.0,
          "policy: n=20 / no GPU -> RAS low-rank");
    check(!big.reason.empty() && big.reason.find("RAS") != std::string::npos,
          "policy: RAS 决策给出可核对理由");

    check(select_state_backend(StateBackend::Ras, r, true, true).backend == StateBackend::Ras,
          "policy: manual Ras forced");
    check(state_backend_name(StateBackend::Ras) == std::string("ras") &&
          parse_state_backend("ras") == StateBackend::Ras &&
          parse_state_backend("lowrank") == StateBackend::Ras,
          "policy: ras/lowrank 名称往返");

    r.qubits = 16;
    check(select_state_backend(StateBackend::Auto, r, true, true, 0.005).backend == StateBackend::Sparse,
          "policy: explicit epsilon overrides");

    r.qubits = 16; r.moe_experts = 32;
    check(select_state_backend(StateBackend::Auto, r, true, true).backend == StateBackend::Dense,
          "policy: MoE routing incompatible with shared-unitary sparse");

    check(parse_state_backend("dense") == StateBackend::Dense &&
          parse_state_backend("sparse") == StateBackend::Sparse &&
          parse_state_backend("auto") == StateBackend::Auto,
          "policy: parse/name roundtrip");
    check(std::string(state_backend_name(StateBackend::Sparse)) == "sparse",
          "policy: backend name");
    check(dense_working_bytes(r) > 0, "policy: working-set estimate positive");
}

static void run_matrix(bool use_device)
{
    const char *tag = use_device ? "device" : "host";
    const size_t qubits[] = {8, 10};
    const size_t layers[] = {1, 2};
    const size_t batches[] = {1, 3};
    const size_t tilebits[] = {0, 4};
    const double epsilons[] = {0.0, 0.02};
    const uint64_t seeds[] = {42, 777};

    for (size_t n : qubits)
    for (size_t L : layers)
    for (size_t B : batches)
    for (size_t tb : tilebits)
    for (double eps : epsilons)
    for (uint64_t seed : seeds)
    {
        const size_t N = size_t(1) << n;
        size_t P = 0;
        const auto gates = make_circuit(n, L, P);
        std::vector<double> theta(P);
        std::mt19937_64 rng(seed + P);
        std::uniform_real_distribution<double> u(-0.4, 0.4);
        for (auto &x : theta) x = u(rng);

        const std::vector<size_t> bits = {n - 8u, n - 7u, n - 6u, n - 5u,
                                         n - 4u, n - 3u, n - 2u, n - 1u};
        const size_t D = bits.size();
        State kets(B * N);
        std::vector<uint8_t> targets(B);
        for (size_t b = 0; b < B; ++b)
        {
            const State one = make_ket(N, seed + b * 131 + n);
            for (size_t i = 0; i < N; ++i) kets[b * N + i] = one[i];
            targets[b] = static_cast<uint8_t>(0x37 + 0x2B * b);
        }

        RefResult ref;
        ref.grad.assign(P, 0.0);
        for (size_t b = 0; b < B; ++b)
        {
            State one(kets.begin() + b * N, kets.begin() + (b + 1) * N);
            const auto r = dense_reference(gates, one, theta, bits, targets[b], P);
            ref.loss += r.loss / double(B);
            ref.acc += r.acc / double(B);
            for (size_t i = 0; i < P; ++i) ref.grad[i] += r.grad[i] / double(B);
        }

        std::vector<unsigned char> gt(gates.size());
        std::vector<size_t> q0(gates.size()), q1(gates.size()), prm(gates.size());
        std::vector<double> sc(gates.size());
        for (size_t g = 0; g < gates.size(); ++g)
        {
            gt[g] = static_cast<unsigned char>(gates[g].type);
            q0[g] = gates[g].q0; q1[g] = gates[g].q1;
            prm[g] = gates[g].param; sc[g] = gates[g].scale;
        }

        qlm::AdjointRuntimeConfig cfg;
        cfg.tile_bits = tb;
        cfg.sparse_tile_bits = tb ? tb : 6;
        cfg.sparse_epsilon = eps;
        cfg.moe_experts = 1;
        double skipped = 0.0, bound = 0.0;
        size_t atiles = 0, ttiles = 0;
        cfg.skipped_mass = &skipped; cfg.error_bound = &bound;
        cfg.active_tiles = &atiles; cfg.total_tiles = &ttiles;

        std::vector<double> grad(P, 0.0), loss(B, 0.0), acc(B, 0.0);
        std::vector<double> ket_pod(B * N * 2);
        for (size_t i = 0; i < B * N; ++i)
        {
            ket_pod[2 * i] = kets[i].real();
            ket_pod[2 * i + 1] = kets[i].imag();
        }

#ifdef RVDL_REGRESSION_WITH_KOKKOS
        if (use_device)
            rvdl::accel::device_adjoint_sparse_batch_pod(
                gt.data(), q0.data(), q1.data(), prm.data(), sc.data(), gates.size(),
                ket_pod.data(), B, N, theta.data(), bits.data(), D, targets.data(),
                grad.data(), P, loss.data(), acc.data(), &cfg);
        else
#endif
            rvdl::sparse_adjoint::sparse_adjoint_batch_pod(
                gt.data(), q0.data(), q1.data(), prm.data(), sc.data(), gates.size(),
                ket_pod.data(), B, N, theta.data(), bits.data(), D, targets.data(),
                grad.data(), P, loss.data(), acc.data(), &cfg);

        double eg = 0.0, el = 0.0, ea = 0.0;
        for (size_t i = 0; i < P; ++i) eg = std::max(eg, std::fabs(grad[i] - ref.grad[i]));
        for (size_t b = 0; b < B; ++b)
        {
            State one(kets.begin() + b * N, kets.begin() + (b + 1) * N);
            const auto r = dense_reference(gates, one, theta, bits, targets[b], P);
            el = std::max(el, std::fabs(loss[b] - r.loss));
            ea = std::max(ea, std::fabs(acc[b] - r.acc));
        }

        char label[256];
        std::snprintf(label, sizeof(label),
                      "%s n=%zu L=%zu B=%zu tb=%zu eps=%.3g seed=%llu",
                      tag, n, L, B, tb, eps, (unsigned long long)seed);
        if (eps == 0.0)
        {
            if (eg > 1e-9 || el > 1e-9 || ea > 1e-9)
            {
                std::fprintf(stderr,
                             "PARITY FAIL %s eg=%.3e el=%.3e ea=%.3e "
                             "skipped=%.3e bound=%.3e tiles=%zu/%zu\n",
                             label, eg, el, ea, skipped, bound, atiles, ttiles);
                ++g_failures;
            }
            ++g_checks;
            check(bound == 0.0, "exact branch reports zero error bound");
            check(atiles == ttiles, "exact branch keeps all tiles active");
        }
        else
        {
            check(eg <= bound + 1e-6, "approximate grad error within reported bound");
            check(bound > 0.0 && skipped >= 0.0, "approximate branch reports computable bound");
            check(atiles <= ttiles, "active tiles never exceed total");
            if (eg > bound + 1e-6)
                std::fprintf(stderr, "BOUND FAIL %s eg=%.3e bound=%.3e\n", label, eg, bound);
        }
        (void)el; (void)ea;
    }
}

int main()
{
    std::printf("=== A. StatePolicy ===\n");
    test_policy();
    std::printf("=== B. host sparse adjoint long regression ===\n");
    run_matrix(false);
#ifdef RVDL_REGRESSION_WITH_KOKKOS
    std::printf("=== C. Kokkos sparse adjoint long regression ===\n");
    Kokkos::initialize();
    { run_matrix(true); }
    Kokkos::finalize();
#else
    std::printf("=== C. Kokkos matrix skipped (define RVDL_REGRESSION_WITH_KOKKOS) ===\n");
#endif
    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}