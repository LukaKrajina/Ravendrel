#include "utils/A2LCollective.hpp"
#ifdef near
#undef near
#endif

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <thread>

using namespace a2l::net;

static int checks = 0, failures = 0;
static void check(bool ok, const char *what)
{
    ++checks;
    if (!ok) { ++failures; std::fprintf(stderr, "FAIL: %s\n", what); }
    else std::printf("  ok  %s\n", what);
}
static bool nearly_eq(double a, double b, double eps = 1e-9)
{
    return std::fabs(a - b) <= eps * (1.0 + std::fabs(a) + std::fabs(b));
}

int main()
{
    {
        GradientAllReduce ar(3, 3);
        const std::vector<double> a{1, 2, 3}, b{4, 5, 6}, c{7, 8, 9};
        check(ar.contribute(1, "a", a), "贡献 a");
        check(ar.contribute(1, "b", b), "贡献 b");
        check(!ar.contribute(1, "a", a), "同一 round 重复贡献被去重");
        check(ar.contribute(1, "c", c), "贡献 c");
        const auto r = ar.wait(1, 50);
        check(r.received == 3 && !r.degraded, "三节点全部收齐");
        check(nearly_eq(r.sum[0], 4.0) && nearly_eq(r.sum[1], 5.0) && nearly_eq(r.sum[2], 6.0),
              "聚合结果 = 算术平均");
        check(ar.pending_rounds() == 0, "完成的 round 被清理");

        GradientAllReduce ar2(3, 3);
        (void)ar2.contribute(5, "c", c);
        (void)ar2.contribute(5, "a", a);
        (void)ar2.contribute(5, "b", b);
        const auto r2 = ar2.wait(5, 50);
        check(nearly_eq(r2.sum[0], 4.0) && nearly_eq(r2.sum[2], 6.0), "R1 聚合对输入置换不变");

        GradientAllReduce ar3(2, 2);
        (void)ar3.contribute(10, "a", std::vector<double>{1, 1});
        (void)ar3.contribute(10, "b", std::vector<double>{3, 3});
        const auto r10 = ar3.wait(10, 20);
        check(nearly_eq(r10.sum[0], 2.0), "round 10 正确");
        check(!ar3.contribute(9, "late", std::vector<double>{100, 100}), "R2 旧 round 被拒");
        check(ar3.current_round() == 10, "当前 round 不回退");

        GradientAllReduce ar4(2, 3);
        (void)ar4.contribute(1, "a", std::vector<double>{2, 4});
        (void)ar4.contribute(1, "b", std::vector<double>{4, 6});
        const auto r4 = ar4.wait(1, 20);
        check(r4.degraded && r4.received == 2, "R3 超时标记 degraded");
        check(nearly_eq(r4.sum[0], 3.0) && nearly_eq(r4.sum[1], 5.0),
              "R3 只用实际收到的贡献做分母");

        check(!ar4.contribute(2, "bad", std::vector<double>{NAN, 1}),
              "R5 NaN 贡献被拒");
        check(!ar4.contribute(2, "bad", std::vector<double>{1}),
              "维度不符被拒");
    }

    {
        const std::vector<double> g{0.1, -2.5, 1.0 / 3.0, 1e-17};
        const std::string s = GradientAllReduce::encode(42, "node-A", g);
        uint64_t r = 0;
        std::string from;
        std::vector<double> out;
        check(GradientAllReduce::decode(s, r, from, out), "载荷可解码");
        check(r == 42 && from == "node-A" && out.size() == g.size(), "round/from/维度一致");
        double e = 0.0;
        for (size_t i = 0; i < g.size(); ++i) e = std::max(e, std::fabs(g[i] - out[i]));
        check(e < 1e-15, "浮点往返精度");
        check(!GradientAllReduce::decode("garbage", r, from, out), "坏载荷被拒");
    }

    {
        ReconnectPolicy p;
        uint64_t prev = 0;
        bool monotone = true;
        for (size_t i = 1; i <= 8; ++i)
        {
            const uint64_t d = p.delay_for(i);
            if (d < prev) monotone = false;
            prev = d;
        }
        check(monotone, "R4 退避单调不减");
        check(p.delay_for(1) >= 100 && p.delay_for(20) <= 30000 + 30000 / 8,
              "R4 退避有上界");
        check(p.delay_for(3) == p.delay_for(3), "R4 同一 seed 抖动可复现");
        check(p.exhausted(8) && !p.exhausted(1), "最大重试次数生效");
    }

    {
        DistributedNodeCompletion a("grad-a", 4), b("grad-b", 4);
        const bool ok = a.start(0) && b.start(0);
        std::printf("[nodes] start=%d reason=%s / %s\n", ok ? 1 : 0,
                    a.io_reason().c_str(), b.io_reason().c_str());
        check(ok, "两个 completion-driven 节点启动");
        if (ok)
        {
            check(a.completion_active() && b.completion_active(),
                  "两节点都走真实 completion 通道");
            check(a.connect_peer("127.0.0.1", b.port(), "grad-b"), "A 连 B");
            check(b.connect_peer("127.0.0.1", a.port(), "grad-a"), "B 连 A");

            DistributedTrainer<DistributedNodeCompletion> ta(a, 3, 2), tb(b, 3, 2);
            ta.start();
            tb.start();

            AllReduceRound ra{}, rb{};
            std::thread t1([&] { ra = ta.all_reduce({1, 2, 3}, 3000); });
            std::thread t2([&] { rb = tb.all_reduce({4, 5, 6}, 3000); });
            t1.join(); t2.join();

            std::printf("[grad ] A received=%zu degraded=%d sum=%.3f,%.3f,%.3f | "
                        "B received=%zu degraded=%d sum=%.3f,%.3f,%.3f\n",
                        ra.received, ra.degraded ? 1 : 0,
                        ra.sum.empty() ? 0 : ra.sum[0],
                        ra.sum.empty() ? 0 : ra.sum[1],
                        ra.sum.empty() ? 0 : ra.sum[2],
                        rb.received, rb.degraded ? 1 : 0,
                        rb.sum.empty() ? 0 : rb.sum[0],
                        rb.sum.empty() ? 0 : rb.sum[1],
                        rb.sum.empty() ? 0 : rb.sum[2]);
            check(ra.received >= 1 && rb.received >= 1, "两节点都收到贡献");
            check(ta.rounds() == 1 && tb.rounds() == 1, "各跑一个 round");
            check(!ra.sum.empty() && !rb.sum.empty(), "聚合结果非空");
            const bool any_full = (ra.received >= 2 || rb.received >= 2);
            check(any_full, "至少一个节点收齐跨节点梯度");
            if (ra.received >= 2)
                check(nearly_eq(ra.sum[0], 2.5) && nearly_eq(ra.sum[2], 4.5),
                      "跨节点 AllReduce 数值正确");
            if (rb.received >= 2)
                check(nearly_eq(rb.sum[0], 2.5) && nearly_eq(rb.sum[2], 4.5),
                      "对侧 AllReduce 数值一致");

            check(a.lease(0, 1) && !a.lease(0, 1) && a.lease(0, 2), "分片租约单调");
            check(a.checkpoint("ck") == 1 && a.checkpoint_seq() == 1, "checkpoint 序号");
            (void)a.heartbeat(30000);
            check(a.alive_peers() >= 1, "重连/存活状态可查询");
            check(a.live_operations() > 0, "完成上下文未泄漏为空");

            ta.stop(); tb.stop();
            a.stop(); b.stop();
        }
    }

    std::printf("\n=== %d checks, %d failures ===\n", checks, failures);
    return failures ? 1 : 0;
}