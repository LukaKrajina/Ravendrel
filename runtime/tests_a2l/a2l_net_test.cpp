#include "utils/A2LNet.hpp"

#include <atomic>
#include <chrono>
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

int main()
{
    const auto caps = NativeReactor::probe();
    std::printf("[caps ] backend=%s usable=%d batch=%d reason=%s\n",
                backend_name(caps.backend), caps.usable ? 1 : 0,
                caps.max_batch, caps.reason.c_str());
    check(caps.usable, "本机探测到可用的 IO 多路复用后端");
    check(caps.backend != IoBackendKind::None, "后端类型明确");
    check(!caps.reason.empty(), "探测给出可核对理由");
#ifdef _WIN32
    check(caps.backend == IoBackendKind::IOCP || caps.backend == IoBackendKind::Poll,
          "Windows 只报 IOCP 或 poll 降级");
#endif

    {
        NativeReactor r;
        check(r.open(), "reactor 打开成功");
        check(r.capabilities().usable, "reactor 能力可用");
        check(r.backend() != IoBackendKind::None, "reactor 后端已确定");
        uint16_t port = 0;
        socket_t s = make_listener(0, port);
        check(s != kInvalidSocket && port != 0, "回环监听 socket 建立");
        check(r.add(s, 1), "socket 注册到 reactor");
        check(r.registered() >= 1, "注册计数");
        r.wakeup();
        size_t n = r.poll_once(1, [](const NetEvent &) {});
        check(n <= 64, "单次 poll 不超过批量上限");
        r.remove(s);
        check(r.registered() == 0, "注销生效");
        close_socket(s);
        r.close();
    }

    {
        const std::string payload = "hello\na2l\n分布式帧";
        const std::string frame = encode_frame(payload);
        check(frame.size() == payload.size() + 4, "帧长 = payload + 4");
        check(frame_length(frame.data()) == payload.size(), "帧头长度正确");
        const std::string empty;
        check(frame_length(encode_frame(empty).data()) == 0, "空帧长度 0");
    }

    {
        DistributedNode a("node-a", 8), b("node-b", 8);
        std::atomic<int> a_received{0}, b_received{0};
        std::string a_last, b_last;
        std::mutex a_mu, b_mu;
        a.on_message([&](const NetMessage &m) {
            std::lock_guard<std::mutex> g(a_mu);
            a_last = m.topic + ":" + m.body;
            ++a_received;
        });
        b.on_message([&](const NetMessage &m) {
            std::lock_guard<std::mutex> g(b_mu);
            b_last = m.topic + ":" + m.body;
            ++b_received;
        });
        check(a.start(0), "节点 A 启动监听");
        check(b.start(0), "节点 B 启动监听");
        check(a.port() != 0 && b.port() != 0, "系统分配了真实端口");
        check(a.backend() != IoBackendKind::None, "节点 A 使用真实 reactor 后端");

        check(a.connect_peer("127.0.0.1", b.port(), "node-b"), "A 真实 connect 到 B");
        check(b.connect_peer("127.0.0.1", a.port(), "node-a"), "B 真实 connect 到 A");

        const size_t sent_a = a.broadcast("PING", "from-a");
        const size_t sent_b = b.broadcast("PONG", "from-b");
        check(sent_a >= 1, "A 广播至少送达 1 个对端");
        check(sent_b >= 1, "B 广播至少送达 1 个对端");

        for (int i = 0; i < 100 && (a_received.load() == 0 || b_received.load() == 0); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        {
            std::lock_guard<std::mutex> g(b_mu);
            check(b_received.load() >= 1, "B 收到 A 的帧");
            check(b_last.find("PING") != std::string::npos, "B 收到正确 topic");
        }
        {
            std::lock_guard<std::mutex> g(a_mu);
            check(a_received.load() >= 1, "A 收到 B 的帧");
            check(a_last.find("PONG") != std::string::npos, "A 收到正确 topic");
        }

        check(a.lease(0, 1), "分片租约 epoch=1 获得");
        check(!a.lease(0, 1), "重复 epoch 被拒");
        check(a.lease(0, 2), "更高 epoch 抢占成功");
        const size_t s1 = a.checkpoint("ck-1");
        const size_t s2 = a.checkpoint("ck-2");
        check(s1 == 1 && s2 == 2 && a.checkpoint_seq() == 2, "checkpoint 序号单调");
        check(a.broadcast("CHECKPOINT", "3:manual") >= 1, "checkpoint 广播走真实链路");
        check(a.alive_peers() >= 1 || a.peer_count() >= 1, "对端表已建立");
        (void)a.heartbeat(0);
        check(a.shards() == 8, "分片数");

        a.stop();
        b.stop();
    }

    std::printf("\n=== %d checks, %d failures ===\n", checks, failures);
    return failures ? 1 : 0;
}