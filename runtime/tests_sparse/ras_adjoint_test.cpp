#include "qlm/RasAdjoint.hpp"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <numeric>
#include <random>
#include <vector>

using Complex = std::complex<double>;
using State = std::vector<Complex>;

static int checks = 0, failures = 0;
static void check(bool ok, const char *what)
{
    ++checks;
    if (!ok) { ++failures; std::fprintf(stderr, "FAIL: %s\n", what); }
    else std::printf("  ok  %s\n", what);
}

static double dense_adjoint(const std::vector<unsigned char> &gt,
                            const std::vector<size_t> &q0,
                            const std::vector<size_t> &q1,
                            const std::vector<size_t> &prm,
                            const std::vector<double> &sc,
                            const State &ket, size_t N,
                            const std::vector<double> &theta,
                            const std::vector<size_t> &bits,
                            uint8_t target, size_t P,
                            std::vector<double> &grad, std::vector<double> &pred,
                            double &acc)
{
    auto fwd = [&](State &s, size_t g) {
        const int type = gt[g];
        const double a = theta[prm[g]] * sc[g];
        if (type == 0)
        {
            const size_t m = size_t(1) << q0[g];
            const double c = std::cos(a * 0.5), sn = std::sin(a * 0.5);
            for (size_t i = 0; i < N; ++i)
            {
                if (i & m) continue;
                const Complex x = s[i], y = s[i | m];
                s[i] = c * x - sn * y;
                s[i | m] = sn * x + c * y;
            }
        }
        else if (type == 1)
        {
            const size_t m = size_t(1) << q0[g];
            const Complex ph(std::cos(a), std::sin(a));
            for (size_t i = 0; i < N; ++i) if (i & m) s[i] *= ph;
        }
        else
        {
            const size_t cm = size_t(1) << q0[g], tm = size_t(1) << q1[g];
            for (size_t i = 0; i < N; ++i)
                if ((i & cm) && !(i & tm)) std::swap(s[i], s[i | tm]);
        }
    };
    const size_t D = bits.size();
    State ph = ket;
    for (size_t g = 0; g < gt.size(); ++g) fwd(ph, g);
    pred.assign(D, 0.0);
    std::vector<double> gz(D, 0.0);
    double loss = 0.0;
    acc = 0.0;
    for (size_t j = 0; j < D; ++j)
    {
        const size_t mask = size_t(1) << bits[j];
        double p1 = 0.0;
        for (size_t s = 0; s < N; ++s) if (s & mask) p1 += std::norm(ph[s]);
        pred[j] = 1.0 - 2.0 * p1;
        const int bit = (target >> j) & 1;
        const double tg = 1.0 - 2.0 * bit;
        const double d = pred[j] - tg;
        loss += d * d;
        gz[j] = 2.0 * d / double(D);
        if (((pred[j] < 0.0) ? 1 : 0) == bit) acc += 1.0;
    }
    loss /= double(D);
    acc /= double(D);
    State lm(N, Complex(0, 0));
    for (size_t s = 0; s < N; ++s)
    {
        double c = 0.0;
        for (size_t j = 0; j < D; ++j)
            c += gz[j] * (((s >> bits[j]) & 1) ? -1.0 : 1.0);
        lm[s] = c * ph[s];
    }
    grad.assign(P, 0.0);
    for (size_t kk = gt.size(); kk > 0; --kk)
    {
        const size_t g = kk - 1;
        const size_t mask = size_t(1) << q0[g];
        const double a = theta[prm[g]] * sc[g];
        double dot = 0.0;
        if (gt[g] == 1)
        {
            for (size_t s = 0; s < N; ++s)
                if (s & mask) dot += (std::conj(lm[s]) * ph[s]).imag();
            grad[prm[g]] += -2.0 * sc[g] * dot;
        }
        else if (gt[g] == 0)
        {
            for (size_t s = 0; s < N; ++s)
            {
                if (s & mask) continue;
                dot += (std::conj(lm[s | mask]) * ph[s] -
                        std::conj(lm[s]) * ph[s | mask]).real();
            }
            grad[prm[g]] += sc[g] * dot;
        }
        auto inv = [&](State &s) {
            const double na = -a;
            if (gt[g] == 0)
            {
                const size_t m = size_t(1) << q0[g];
                const double c = std::cos(na * 0.5), sn = std::sin(na * 0.5);
                for (size_t i = 0; i < N; ++i)
                {
                    if (i & m) continue;
                    const Complex x = s[i], y = s[i | m];
                    s[i] = c * x - sn * y;
                    s[i | m] = sn * x + c * y;
                }
            }
            else if (gt[g] == 1)
            {
                const size_t m = size_t(1) << q0[g];
                const Complex ph2(std::cos(na), std::sin(na));
                for (size_t i = 0; i < N; ++i) if (i & m) s[i] *= ph2;
            }
            else
            {
                const size_t cm = size_t(1) << q0[g], tm = size_t(1) << q1[g];
                for (size_t i = 0; i < N; ++i)
                    if ((i & cm) && !(i & tm)) std::swap(s[i], s[i | tm]);
            }
        };
        inv(ph); inv(lm);
    }
    return loss;
}





static State product_ket(size_t N, uint64_t seed)
{
    const size_t n = size_t(std::log2(double(N)));
    std::mt19937_64 rng(seed);
    std::uniform_real_distribution<double> u(-0.6, 0.6);
    State s(N, Complex(1.0, 0.0));
    for (size_t q = 0; q < n; ++q)
    {
        const double a = u(rng);
        const size_t m = size_t(1) << q;
        const double c = std::cos(a * 0.5), sn = std::sin(a * 0.5);
        for (size_t i = 0; i < N; ++i)
        {
            if (i & m) continue;
            const Complex x = s[i], y = s[i | m];
            s[i] = c * x - sn * y;
            s[i | m] = sn * x + c * y;
        }
    }
    return s;
}

static State random_ket(size_t N, uint64_t seed)
{
    std::mt19937_64 rng(seed);
    std::normal_distribution<double> g(0.0, 1.0);
    State s(N);
    double n = 0.0;
    for (auto &z : s) { z = Complex(g(rng), g(rng)); n += std::norm(z); }
    const double inv = 1.0 / std::sqrt(n);
    for (auto &z : s) z *= inv;
    return s;
}

struct Circuit
{
    std::vector<unsigned char> gt;
    std::vector<size_t> q0, q1, prm;
    std::vector<double> sc;
    size_t params = 0;
};

static Circuit make(size_t n, size_t layers, bool cross_domain, size_t t)
{
    Circuit c;
    size_t p = 0;
    for (size_t l = 0; l < layers; ++l)
    {
        for (size_t q = 0; q < n; ++q)
        { c.gt.push_back(0); c.q0.push_back(q); c.q1.push_back(0); c.prm.push_back(p++); c.sc.push_back(0.8); }
        for (size_t q = 0; q < n; ++q)
        { c.gt.push_back(1); c.q0.push_back(q); c.q1.push_back(0); c.prm.push_back(p++); c.sc.push_back(0.5); }
        for (size_t q = 0; q + 1 < n; ++q)
        {
            if (q + 1 == t) continue;
            c.gt.push_back(2); c.q0.push_back(q); c.q1.push_back(q + 1);
            c.prm.push_back(0); c.sc.push_back(1.0);
            if (cross_domain && q == 0)
            {
                c.gt.push_back(2); c.q0.push_back(1); c.q1.push_back(t + 1);
                c.prm.push_back(0); c.sc.push_back(1.0);
            }
        }
    }
    c.params = p;
    return c;
}

static void run_case(size_t n, size_t layers, size_t t, bool cross,
                     double eps, size_t max_rank, uint64_t seed, bool expect_exact,
                     bool product = false)
{
    const size_t N = size_t(1) << n;
    const size_t D = 8;
    std::vector<size_t> bits;
    for (size_t j = 0; j < D; ++j) bits.push_back(n - D + j);
    Circuit c = make(n, layers, cross, t);
    const State ket = product ? product_ket(N, seed) : random_ket(N, seed);
    std::vector<double> theta(c.params);
    std::mt19937_64 rng(seed * 7 + 1);
    std::uniform_real_distribution<double> u(-0.4, 0.4);
    for (auto &x : theta) x = u(rng);
    const uint8_t target = 0x5A;

    std::vector<double> g_ref, p_ref;
    double a_ref = 0.0;
    const double l_ref = dense_adjoint(c.gt, c.q0, c.q1, c.prm, c.sc, ket, N,
                                       theta, bits, target, c.params,
                                       g_ref, p_ref, a_ref);

    qlm::AdjointRuntimeConfig cfg;
    cfg.sparse_tile_bits = t;
    cfg.sparse_epsilon = eps;
    cfg.ras_epsilon = eps;
    cfg.ras_rank = max_rank;
    cfg.ras_seed = seed;
    std::vector<double> g_ras, p_ras, kd(N * 2);
    for (size_t i = 0; i < N; ++i)
    { kd[2 * i] = ket[i].real(); kd[2 * i + 1] = ket[i].imag(); }
    double a_ras = 0.0;
    rvdl::ras_adjoint::Result rep;
    const double l_ras = rvdl::ras_adjoint::sample_adjoint_ras(
        c.gt.data(), c.q0.data(), c.q1.data(), c.prm.data(), c.sc.data(), c.gt.size(),
        reinterpret_cast<const rvdl::lowrank::Complex *>(kd.data()), N,
        theta.data(), bits.data(), D, target, c.params, cfg,
        g_ras, p_ras, a_ras, &rep);

    double eg = 0.0;
    for (size_t i = 0; i < c.params; ++i) eg = std::max(eg, std::fabs(g_ras[i] - g_ref[i]));
    double el = std::fabs(l_ras - l_ref);
    double ea = std::fabs(a_ras - a_ref);

    char label[192];
    std::snprintf(label, sizeof(label),
                  "n=%zu L=%zu t=%zu cross=%d eps=%.0e r=%zu seed=%llu",
                  n, layers, t, cross ? 1 : 0, eps, max_rank, (unsigned long long)seed);
    if (expect_exact)
    {
        if (eg > 1e-12 || el > 1e-12 || ea > 1e-12)
            std::fprintf(stderr, "EXACT FAIL %s eg=%.3e el=%.3e ea=%.3e\n", label, eg, el, ea);
        check(eg <= 1e-12 && el <= 1e-12 && ea <= 1e-12, "A1 exact 与稠密 host 逐位一致");
        check(rep.rank == 0, "A1 exact 时 rank=0（走稠密分支）");
    }
    else
    {
        double dot = 0.0, n1 = 0.0, n2 = 0.0;
        for (size_t i = 0; i < c.params; ++i)
        { dot += g_ras[i] * g_ref[i]; n1 += g_ras[i] * g_ras[i]; n2 += g_ref[i] * g_ref[i]; }
        const double corr = (n1 > 0 && n2 > 0) ? dot / std::sqrt(n1 * n2) : 0.0;
        std::printf("[ras ] %s rank=%zu lam=%zu refac=%zu el=%.3e corr=%.4f bound=%.3e\n",
                    label, rep.rank, rep.lambda_rank, rep.refactors, el, corr, rep.error_bound);
        check(std::isfinite(l_ras) && std::isfinite(a_ras), "A2 loss/acc 有限");
        if (product)
            check(corr > 0.99, "A5 可压态梯度与稠密强正相关（>0.99）");
        else
            check(std::isfinite(corr), "A5 高秩态梯度相关性有限（不宣称可压）");
        check(rep.rank <= max_rank * 2 + 2, "A4 秩不超过 2r 上界");
    }
}

int main()
{
    for (size_t n : {size_t(8), size_t(10)})
    for (size_t t : {size_t(4), size_t(6)})
    for (bool cross : {false, true})
    for (uint64_t seed : {uint64_t(11), uint64_t(29)})
    {
        if (t >= n) continue;
        run_case(n, 2, t, cross, 0.0, 0, seed, true);
    }
    std::printf("[exact] 全部 exact 用例通过\n");

    for (size_t n : {size_t(8), size_t(10)})
    for (bool cross : {false, true})
    for (double eps : {1e-3, 1e-2})
    for (uint64_t seed : {uint64_t(5), uint64_t(13)})
        run_case(n, 2, 6, cross, eps, 6, seed, false, true);

    for (uint64_t seed : {uint64_t(3), uint64_t(17)})
        run_case(8, 2, 6, false, 1e-2, 4, seed, false, false);

    {
        const size_t n = 10, N = size_t(1) << n, t = 6;
        Circuit c = make(n, 2, false, t);
        std::vector<double> theta(c.params, 0.21);
        std::vector<size_t> bits;
        for (size_t j = 0; j < 8; ++j) bits.push_back(n - 8 + j);
        qlm::AdjointRuntimeConfig cfg;
        cfg.sparse_tile_bits = t;
        cfg.sparse_epsilon = 1e-3;
        cfg.ras_epsilon = 1e-3;
        cfg.ras_rank = 6;
        std::vector<double> g, p, kd(N * 2);
        const State ket = random_ket(N, 99);
        for (size_t i = 0; i < N; ++i)
        { kd[2 * i] = ket[i].real(); kd[2 * i + 1] = ket[i].imag(); }
        double a = 0.0;
        rvdl::ras_adjoint::Result rep;
        (void)rvdl::ras_adjoint::sample_adjoint_ras(
            c.gt.data(), c.q0.data(), c.q1.data(), c.prm.data(), c.sc.data(), c.gt.size(),
            reinterpret_cast<const rvdl::lowrank::Complex *>(kd.data()), N,
            theta.data(), bits.data(), 8, 0x33, c.params, cfg, g, p, a, &rep);
        check(rep.refactors == 0, "A3 纯同位域电路不做重分解");
        check(rep.rank > 0, "A3 低秩模式 rank > 0");
        check(rep.lambda_rank > 0 && rep.lambda_rank <= 2 * rep.rank,
              "A3 λ 的秩 ≤ 2·rank（定理 A）");
    }

    std::printf("\n=== %d checks, %d failures ===\n", checks, failures);
    return failures ? 1 : 0;
}