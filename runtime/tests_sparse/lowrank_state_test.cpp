#include "qhal/LowRankState.hpp"

#include <cmath>
#include <complex>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

using namespace rvdl::lowrank;

static int checks = 0, failures = 0;
static void check(bool ok, const char *what)
{
    ++checks;
    if (!ok) { ++failures; std::fprintf(stderr, "FAIL: %s\n", what); }
    else std::printf("  ok  %s\n", what);
}

static State random_state(size_t d, uint64_t seed)
{
    std::mt19937_64 rng(seed);
    std::normal_distribution<double> g(0.0, 1.0);
    State s(d);
    double norm = 0.0;
    for (auto &z : s) { z = Complex(g(rng), g(rng)); norm += std::norm(z); }
    const double inv = 1.0 / std::sqrt(norm);
    for (auto &z : s) z *= inv;
    return s;
}

static State dense_apply_ry(State s, size_t q, double a)
{
    const size_t m = size_t(1) << q;
    const double c = std::cos(a / 2), sn = std::sin(a / 2);
    for (size_t i = 0; i < s.size(); ++i)
    {
        if (i & m) continue;
        const Complex x = s[i], y = s[i | m];
        s[i] = c * x - sn * y;
        s[i | m] = sn * x + c * y;
    }
    return s;
}
static State dense_apply_rz(State s, size_t q, double a)
{
    const size_t m = size_t(1) << q;
    const Complex ph(std::cos(a), std::sin(a));
    for (size_t i = 0; i < s.size(); ++i) if (i & m) s[i] *= ph;
    return s;
}
static State dense_apply_cnot(State s, size_t ctrl, size_t targ)
{
    const size_t cm = size_t(1) << ctrl, tm = size_t(1) << targ;
    for (size_t i = 0; i < s.size(); ++i)
        if ((i & cm) && !(i & tm)) std::swap(s[i], s[i | tm]);
    return s;
}
static std::vector<double> dense_p1(const State &s, const std::vector<size_t> &bits)
{
    std::vector<double> out(bits.size(), 0.0);
    for (size_t i = 0; i < s.size(); ++i)
        for (size_t j = 0; j < bits.size(); ++j)
            if (i & (size_t(1) << bits[j])) out[j] += std::norm(s[i]);
    return out;
}
static double max_abs_diff(const State &a, const State &b)
{
    double e = 0.0;
    for (size_t i = 0; i < std::min(a.size(), b.size()); ++i)
        e = std::max(e, std::abs(a[i] - b[i]));
    return e;
}

int main()
{
    const size_t N = 1 << 10;
    const std::vector<size_t> bits = {2, 3, 4, 5, 6, 7, 8, 9};

    {
        State s = random_state(N, 42);
        RasConfig cfg;
        cfg.tile_bits = 6; cfg.max_rank = 0; cfg.epsilon = 0.0;
        RasReport rep;
        LowRankState lr(s, cfg, &rep);
        check(lr.exact() && rep.exact, "ε=0 走 exact 稠密分支");
        check(rep.error_bound == 0.0 && rep.delta == 0.0, "exact 分支误差界为 0");
        check(max_abs_diff(s, lr.to_dense()) == 0.0, "exact to_dense 逐位一致");

        lr.apply_ry(7, 0.31);
        lr.apply_rz(3, -0.42);
        lr.apply_cnot(5, 8);
        lr.apply_cnot(1, 2);
        State ref = dense_apply_cnot(
            dense_apply_cnot(dense_apply_rz(dense_apply_ry(s, 7, 0.31), 3, -0.42), 5, 8), 1, 2);
        check(max_abs_diff(ref, lr.to_dense()) == 0.0, "exact 门序列逐位 parity");
        const auto p = lr.probability_one(bits), q = dense_p1(ref, bits);
        double e = 0.0;
        for (size_t j = 0; j < bits.size(); ++j) e = std::max(e, std::fabs(p[j] - q[j]));
        check(e == 0.0, "exact 读出逐位 parity");
        check(lr.measured_error(ref) == 0.0, "exact measured_error = 0");
    }

    {
        State s = random_state(N, 77);
        RasConfig cfg;
        cfg.tile_bits = 6; cfg.max_rank = 6; cfg.epsilon = 0.01;
        cfg.power_iters = 6; cfg.seed = 123;
        RasReport rep;
        LowRankState lr(s, cfg, &rep);
        std::printf("[ras  ] rows=%zu cols=%zu rank=%zu/%zu compress=%.2fx "
                    "delta=%.4e bound=%.4e measured=%.4e\n",
                    rep.rows, rep.cols, rep.rank, rep.full_rank, rep.compression,
                    rep.delta, rep.error_bound, lr.measured_error(s));
        check(!lr.exact() && lr.rank() > 0, "ε>0 走低秩因子分支");
        check(rep.compression > 1.0, "稳态内存压缩比 > 1（O(r(L+C)) < O(LC)）");
        check(rep.error_bound > 0.0, "报告可计算误差上界");
        check(lr.measured_error(s) <= rep.error_bound + 1e-12,
              "L4：实测 trace distance ≤ 报告上界");
        check(rep.sigma.size() == rep.rank, "奇异值数量 = 秩");
        bool decreasing = true;
        for (size_t j = 1; j < rep.sigma.size(); ++j)
            if (rep.sigma[j] > rep.sigma[j - 1] * (1.0 + 1e-6)) decreasing = false;
        check(decreasing, "奇异值按降序（幂迭代 deflation）");
        check(rep.kept_energy <= 1.0 + 1e-6, "保留能量不超过总能量");
    }

    {
        State s = random_state(N, 99);
        RasConfig cfg;
        cfg.tile_bits = 6; cfg.max_rank = 64; cfg.epsilon = 1e-3;
        cfg.power_iters = 8; cfg.seed = 7;
        cfg.max_rank = 8;
        LowRankState lr(s, cfg);
        const size_t r0 = lr.rank();
        lr.apply_ry(8, 0.2);
        lr.apply_rz(7, -0.1);
        lr.apply_ry(2, 0.35);
        lr.apply_rz(1, 0.4);
        lr.apply_cnot(7, 9);
        lr.apply_cnot(0, 3);
        check(lr.rank() == r0, "L2：同位域门秩严格不变");
        check(lr.refactor_count() == 0, "L2：同位域门无需重分解");

        const State materialised = lr.to_dense();
        const auto p_factor = lr.probability_one(bits);
        const auto p_dense = dense_p1(materialised, bits);
        double e = 0.0;
        for (size_t j = 0; j < bits.size(); ++j)
            e = std::max(e, std::fabs(p_factor[j] - p_dense[j]));
        std::printf("[read ] factor-vs-dense maxerr=%.3e\n", e);
        check(e < 1e-9, "因子级读出 = 物化后稠密读出（不物化全态）");
    }

    {
        State s = random_state(N, 2024);
        RasConfig cfg;
        cfg.tile_bits = 6; cfg.max_rank = 6; cfg.epsilon = 0.005;
        cfg.power_iters = 6; cfg.seed = 55;
        LowRankState lr(s, cfg);
        const double before = lr.delta();
        lr.apply_cnot(2, 8);
        check(lr.refactor_count() == 1, "L3：跨位域 CNOT 触发一次重分解");
        check(lr.delta() >= before, "L3：重分解误差可加");
        check(lr.rank() > 0, "重分解后仍有有效秩");

        State base = LowRankState(s, cfg).to_dense();
        const State expected = dense_apply_cnot(base, 2, 8);
        const State got = lr.to_dense();
        const double diff_from_expected = max_abs_diff(got, expected);
        const double diff_from_unchanged = max_abs_diff(got, base);
        std::printf("[cnot ] to_expected=%.4f to_unchanged=%.4f delta=%.4e bound=%.4e\n",
                    diff_from_expected, diff_from_unchanged, lr.delta(), lr.error_bound());
        check(diff_from_unchanged > 1e-6, "跨位域 CNOT 确实改变了态（不是丢门）");
        check(diff_from_expected < diff_from_unchanged + 1e-9,
              "重分解后仍逼近正确门结果");
        check(lr.error_bound() > 0.0, "累计误差界可计算");
    }

    {
        size_t cases = 0;
        for (size_t t : {size_t(4), size_t(6)})
        for (size_t r : {size_t(2), size_t(4), size_t(8)})
        for (double eps : {1e-3, 1e-2, 5e-2})
        for (uint64_t seed : {uint64_t(1), uint64_t(2)})
        {
            State s = random_state(N, seed * 31 + t);
            RasConfig cfg;
            cfg.tile_bits = t; cfg.max_rank = r; cfg.epsilon = eps;
            cfg.power_iters = 5; cfg.seed = seed;
            RasReport rep;
            LowRankState lr(s, cfg, &rep);
            const double measured = lr.measured_error(s);
            if (measured > rep.error_bound + 1e-10)
            {
                std::fprintf(stderr,
                    "BOUND FAIL t=%zu r=%zu eps=%.3g seed=%llu measured=%.3e bound=%.3e\n",
                    t, r, eps, (unsigned long long)seed, measured, rep.error_bound);
            }
            check(measured <= rep.error_bound + 1e-10, "矩阵：误差 ≤ 报告上界");
            check(rep.rank <= r && rep.rank > 0, "矩阵：秩不超过上限");
            ++cases;
        }
        std::printf("[matrix] cases=%zu\n", cases);
        check(cases == 36, "矩阵覆盖 36 组配置");
    }

    std::printf("\n=== %d checks, %d failures ===\n", checks, failures);
    return failures ? 1 : 0;
}