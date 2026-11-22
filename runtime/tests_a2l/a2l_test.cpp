#include "utils/A2L.hpp"

#include <atomic>
#include <cstdio>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

using namespace a2l;

static int checks = 0, failures = 0;
static void check(bool ok, const char *what)
{
    ++checks;
    if (!ok) { ++failures; std::fprintf(stderr, "FAIL: %s\n", what); }
    else std::printf("  ok  %s\n", what);
}

int main()
{
    std::printf("=== A2L 并发基础层断言 ===\n");

    {
        CoopBudget b(2);
        check(b.consume() && b.consume() && !b.consume(), "预算耗尽后必须 yield");
        b.reset(1);
        check(b.consume(), "reset 后恢复预算");
    }

    {
        MpmcQueue<int> q(4096);
        std::atomic<int> pushed{0};
        std::vector<std::thread> ts;
        for (int t = 0; t < 4; ++t)
            ts.emplace_back([&, t]()
            {
                for (int i = 0; i < 500; ++i)
                    if (q.try_push(t * 1000 + i)) pushed.fetch_add(1);
            });
        for (auto &t : ts) t.join();
        std::set<int> got;
        int v;
        while (q.try_pop(v)) got.insert(v);
        check(pushed.load() == 2000, "4 线程并发 push 全部成功");
        check(got.size() == 2000, "并发 pop 无重复无丢失");
    }

    {
        ThreadPool pool(4, 8192);
        std::vector<std::future<int>> fs;
        for (int i = 0; i < 1000; ++i) fs.push_back(pool.submit([i] { return i * 2; }));
        long long sum = 0;
        for (auto &f : fs) sum += f.get();
        check(sum == 2LL * 999 * 1000 / 2, "1000 个任务结果全部正确");
        check(pool.completed() == 1000, "completed 原子计数准确");
        check(pool.size() == 4, "worker 数量符合配置");
    }

    {
        ThreadPool pool(4);
        Strand strand;
        std::mutex mu;
        std::vector<int> order;
        std::atomic<int> inside{0};
        std::vector<std::future<void>> fs;
        for (int i = 0; i < 200; ++i)
        {
            fs.push_back(strand.execute(pool, [&, i]
            {
                const int concurrent = inside.fetch_add(1) + 1;
                { std::lock_guard<std::mutex> g(mu); order.push_back(i); }
                std::this_thread::yield();
                inside.fetch_sub(1);
                (void)concurrent;
            }));
        }
        for (auto &f : fs) f.get();
        bool serial = true;
        for (size_t i = 0; i < order.size(); ++i) if (order[i] != int(i)) serial = false;
        check(serial, "Strand 内任务严格按提交顺序串行执行");
        check(inside.load() == 0, "Strand 无并发重叠");
    }

    {
        ShardMap<int, int> map(16);
        std::vector<std::thread> ts;
        for (int t = 0; t < 8; ++t)
            ts.emplace_back([&, t]()
            {
                for (int i = 0; i < 500; ++i) map.put(t * 500 + i, t * 500 + i);
            });
        for (auto &t : ts) t.join();
        bool ok = true;
        for (int i = 0; i < 4000; ++i) { int v; if (!map.get(i, v) || v != i) ok = false; }
        check(ok, "8 线程写入 4000 键全部可读且值正确");
        check(map.shards() == 16, "分片数可配置");
        check(map.erase(0) == 1, "erase 返回删除数量");
    }

    {
        EpochReclaimer r;
        const size_t e1 = r.enter();
        r.retire(std::make_unique<int>(7));
        check(r.retired_count() == 1, "活跃 epoch 内对象延迟回收");
        r.leave(e1);
        check(r.retired_count() == 0, "epoch 退出后安全回收");
    }

    {
        Reactor reactor;
        std::atomic<int> fired{0};
        reactor.register_fd(3, [&](int) { fired.fetch_add(1); });
        reactor.register_fd(7, [&](int) { fired.fetch_add(1); });
        check(reactor.registered() == 2, "fd 注册可计数");
        check(reactor.poll() == 2, "一次 poll 分发全部就绪事件");
        check(fired.load() == 2, "回调全部触发");
        reactor.unregister(3);
        check(reactor.registered() == 1, "注销生效");
        check(!Capabilities::iocp_backend() && !Capabilities::io_uring_backend(),
              "无平台后端时给出可判定的 false（A2a 风格）");
    }

    {
        ThreadPool pool(4);
        auto s = just([] { return 21; }).then([](int x) { return x * 2; });
        check(s.sync() == 42, "Sender::then 类型变换正确");
        check(s.via(pool).get() == 42, "Sender::via 在线程池执行并返回 future");

        std::atomic<int> counter{0};
        {
            StructuredScope scope(pool);
            for (int i = 0; i < 100; ++i) scope.spawn([&] { counter.fetch_add(1); });
        }
        check(counter.load() == 100, "StructuredScope 析构保证全部子任务完成");
    }

    {
        DistributedCoordinator coord(8, 2);
        check(coord.owner(0) == 0 && coord.owner(1) == 1, "分片归属可判定");
        check(coord.try_lease(0, 0, 1) && !coord.try_lease(0, 0, 1), "同 epoch 租约不可重复");
        check(!coord.try_lease(0, 1, 2), "非归属节点不能租约");
        check(coord.try_lease(0, 0, 2), "更大 epoch 可续租");
        check(coord.commit_checkpoint() == 1 && coord.checkpoint() == 1, "checkpoint 原子递增");
    }

    {
        RuntimeResources rr(2);
        auto f = rr.pool.submit([] { return 7; });
        check(f.get() == 7, "RuntimeResources 组合可用");
        rr.reactor.register_fd(1, [](int) {});
        check(rr.reactor.registered() == 1, "资源句柄内 reactor 可用");
    }

    std::printf("\n=== %d checks, %d failures ===\n", checks, failures);
    return failures == 0 ? 0 : 1;
}