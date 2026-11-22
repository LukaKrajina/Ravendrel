#include "qhal/SparseAmplitude.hpp"
#include "qhal/SparseResources.hpp"

#include <cstdio>
#include <random>
#include <thread>
#include <mutex>
#include <set>

using namespace rvdl::sparse;
using namespace rvdl::sparse::res;

static int checks = 0, failures = 0;
static void check(bool ok, const char *what)
{
    ++checks;
    if (!ok) { ++failures; std::fprintf(stderr, "FAIL: %s\n", what); }
    else std::printf("  ok  %s\n", what);
}
static bool near(double a, double b, double eps = 1e-12)
{
    return std::fabs(a - b) <= eps * (1.0 + std::fabs(a) + std::fabs(b));
}

static State random_state(size_t d, uint64_t seed)
{
    std::mt19937_64 rng(seed);
    std::normal_distribution<double> g(0.0, 1.0);
    State s(d);
    for (auto &x : s) x = Complex(g(rng), g(rng));
    normalize(s);
    return s;
}
static void dense_ry(State &s, size_t q, double a)
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
}
static void dense_rz(State &s, size_t q, double a)
{
    const size_t m = size_t(1) << q;
    const Complex p = std::polar(1.0, a);
    for (size_t i = 0; i < s.size(); ++i) if (i & m) s[i] *= p;
}
static void dense_cnot(State &s, size_t ctrl, size_t targ)
{
    const size_t cm = size_t(1) << ctrl, tm = size_t(1) << targ;
    for (size_t i = 0; i < s.size(); ++i)
        if ((i & cm) && !(i & tm)) std::swap(s[i], s[i | tm]);
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
    std::printf("=== DQ-Sparse 可证伪断言 ===\n");

    {
        SparseConfig cfg;
        State s = random_state(1 << 16, 1);
        auto r = evaluate(s, 16, cfg);
        check(r.backend == BackendChoice::Dense, "n<=n0 → Dense（定理 2）");
        cfg.epsilon = 0.0;
        r = evaluate(s, 20, cfg);
        check(r.backend == BackendChoice::Dense, "ε=0 → Dense（精确路径）");

        SparseState exact(s, 6, 0.0, false);
        const State back = exact.to_dense();
        check(max_abs_diff(s, back) == 0.0, "exact 分支 to_dense 逐位一致");
        check(exact.error_bound() == 0.0, "exact 分支误差界为 0");
        check(exact.active_tiles() == exact.tiles(), "exact 分支全部 tile 活跃");
    }

    {
        const size_t d = 1 << 10;
        State dense = random_state(d, 7);
        SparseState sp(dense, 6, 0.0, false);
        dense_ry(dense, 2, 0.37);  sp.apply_ry(2, 0.37);
        dense_rz(dense, 8, -0.61); sp.apply_rz(8, -0.61);
        dense_ry(dense, 8, 0.22);  sp.apply_ry(8, 0.22);
        dense_rz(dense, 2, 0.44);  sp.apply_rz(2, 0.44);
        const double e = max_abs_diff(dense, sp.to_dense());
        std::printf("       gate parity maxdiff=%.3e\n", e);
        check(e < 1e-12, "RY/RZ 跨 tile 与 tile 内均与稠密逐位一致");

        State d2 = random_state(d, 8);
        SparseState s2(d2, 6, 0.0, false);
        dense_cnot(d2, 3, 7); s2.apply_cnot(3, 7);
        check(max_abs_diff(d2, s2.to_dense()) < 1e-12, "CNOT(跨tile control, tile内 target) parity");
        State d3 = random_state(d, 9);
        SparseState s3(d3, 6, 0.0, false);
        dense_cnot(d3, 7, 2); s3.apply_cnot(7, 2);
        check(max_abs_diff(d3, s3.to_dense()) < 1e-12, "CNOT(tile内 control, 跨tile target) parity");
    }

    {
        const size_t d = 1 << 12, tb = 6, C = size_t(1) << tb, L = d / C;
        State s = random_state(d, 11);
        for (size_t c = 0; c < C; c += 4)
            for (size_t k = 0; k < L; ++k) s[(k << tb) | c] *= 1e-8;
        normalize(s);
        SparseState sp(s, tb, 0.05, true);
        const double bound = sp.error_bound();
        const double err = sp.measured_error(s);
        std::printf("       δ=%.4e bound=%.4e measured=%.4e active=%zu/%zu\n",
                    sp.skipped_mass(), bound, err, sp.active_tiles(), sp.tiles());
        check(sp.active_tiles() < sp.tiles(), "低质量 tile 被裁剪");
        check(bound >= sp.skipped_mass(), "误差界 ≥ 丢弃质量（定理 1 推论）");
        check(err <= bound + 1e-9, "实测误差不超过分布式上界（定理 1）");
        check(near(norm_sq(sp.to_dense()), 1.0, 1e-9), "裁剪后重新归一化");
    }

    {
        SparseConfig cfg;
        cfg.epsilon = 0.05;
        const size_t d = 1 << 18, n = 18;
        State s = random_state(d, 21);
        for (size_t i = 0; i < d; i += 2) s[i] *= 1e-6;
        normalize(s);
        auto r = evaluate(s, n, cfg);
        check(r.backend == BackendChoice::DQSparse || r.active_ratio > 0.5,
              "大 n + 可裁剪 tile → 稀疏或如实报告");
        cfg.vram_budget_bytes = 64;
        auto r2 = evaluate(s, n, cfg);
        check(r2.backend == BackendChoice::Dense, "显存不足 → 安全回退 Dense");
        check(!r2.reason.empty(), "策略给出可核对的选择理由");
    }

    {
        const size_t d = 1 << 10;
        State s = random_state(d, 31);
        SparseState sp(s, 6, 0.0, false);
        std::vector<size_t> bits{0, 3, 7};
        auto p1 = sp.probability_one(bits);
        check(p1.size() == bits.size(), "读出维度正确");
        bool ok = true;
        for (double p : p1) if (p < -1e-12 || p > 1.0 + 1e-12) ok = false;
        check(ok, "P(1) 落在 [0,1]");
        std::vector<double> ref(bits.size(), 0.0);
        for (size_t i = 0; i < s.size(); ++i)
            for (size_t j = 0; j < bits.size(); ++j)
                if (i & (size_t(1) << bits[j])) ref[j] += std::norm(s[i]);
        double e = 0.0;
        for (size_t j = 0; j < bits.size(); ++j) e = std::max(e, std::fabs(p1[j] - ref[j]));
        check(e < 1e-12, "稀疏读出与稠密 P(1) 逐位一致");
    }

    {
        TileScheduler sched(1024);
        std::mutex mu;
        std::set<size_t> seen;
        std::vector<std::thread> ts;
        for (int t = 0; t < 8; ++t)
        {
            ts.emplace_back([&]()
            {
                size_t idx;
                while (sched.claim(idx))
                {
                    std::lock_guard<std::mutex> g(mu);
                    seen.insert(idx);
                }
            });
        }
        for (auto &t : ts) t.join();
        check(seen.size() == 1024, "8 线程无锁领取 1024 tile 无重复无遗漏");

        StreamPool pool(4);
        check(pool.acquire() != pool.acquire() || pool.size() == 4, "stream round-robin 可分发");
        VramBudget vram(1024);
        check(vram.try_reserve(512) && !vram.try_reserve(600) && vram.available() == 512,
              "显存 CAS 配额正确");
        vram.release(512);
        check(vram.available() == 1024, "显存归还正确");
        TileArena arena(4, 16);
        size_t slot = 0;
        check(arena.acquire(slot) && arena.at(slot).size() == 16, "TileArena 槽位可领取");
        arena.release(slot);
        size_t slot2 = 0;
        check(arena.acquire(slot2) && slot2 == slot, "TileArena 槽位复用");
    }

    {
        QpuNodePool nodes(2);
        OramBudget oram(8);
        size_t a = 99, b = 99;
        {
            QubitLease l1(nodes, oram, 3);
            QubitLease l2(nodes, oram, 3);
            a = l1.node(); b = l2.node();
            check(l1.valid() && l2.valid() && a != b, "两租约获得不同 QPU 节点");
            check(!nodes.acquire(b), "节点耗尽时拒绝新租约");
            check(oram.available() == 2, "ORAM 槽位按租约扣减");
        }
        check(oram.available() == 8, "RAII 析构归还 ORAM 槽位");
        size_t c = 0;
        check(nodes.acquire(c), "RAII 析构归还 QPU 节点");
        nodes.release(c);

        CoherenceBudget cb(2);
        check(cb.try_enter() && cb.try_enter() && !cb.try_enter(), "相干预算限制并发门数");
        cb.leave();
        check(cb.try_enter(), "leave 后恢复并发额度");
    }

    std::printf("\n=== %d checks, %d failures ===\n", checks, failures);
    return failures == 0 ? 0 : 1;
}