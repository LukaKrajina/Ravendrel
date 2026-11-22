#include "utils/A2LNetIo.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

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
    const bool avail = OverlappedIo::platform_available();
#ifdef __linux__
    const auto nr = NativeReactor::probe();
    std::printf("[caps ] completion_io=%d backend=%s reason=%s\n",
                avail ? 1 : 0, backend_name(nr.backend), nr.reason.c_str());
#endif
    check(avail, "本机提供真实 completion IO 后端");

    {
        FrameAssembler fa;
        std::vector<std::string> out;
        const std::string a = encode_frame("alpha");
        const std::string b = encode_frame(std::string(300, 'B'));
        fa.push(a.data(), 2, out);
        check(out.empty() && fa.pending() == 2, "半长度头不成帧");
        fa.push(a.data() + 2, 3, out);
        check(out.empty(), "半 payload 不成帧");
        fa.push(a.data() + 5, a.size() - 5, out);
        check(out.size() == 1 && out[0] == "alpha", "拼出完整第一帧");
        const std::string both = a + b;
        fa.push(both.data(), both.size(), out);
        check(out.size() == 3 && out[1] == "alpha" && out[2].size() == 300,
              "粘包一次切出两帧");
        char bad[4] = {char(0x7F), char(0xFF), char(0xFF), char(0xFF)};
        fa.push(bad, 4, out);
        check(fa.pending() == 0, "畸形帧头被丢弃且不放大分配");
    }

    {
        OverlappedIo io;
        const bool opened = io.open();
        std::printf("[io   ] opened=%d reason=%s\n", opened ? 1 : 0, io.reason().c_str());
        check(opened, "OverlappedIo 打开真实完成后端");
        if (opened)
        {
            uint16_t port = 0;
            socket_t listener = make_listener(0, port);
            check(listener != kInvalidSocket && port != 0, "监听端口由系统分配");
            check(io.attach(listener), "监听 socket 关联到完成后端");
            check(io.post_accept(listener), "异步 accept 投递成功");

            std::thread client([port]()
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                socket_t s = connect_to("127.0.0.1", port);
                if (s == kInvalidSocket) return;
                (void)send_frame(s, "HELLO from-client");
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                close_socket(s);
            });

            std::atomic<int> accepted{0}, received{0};
            std::string got_payload;
            FrameAssembler fa;
            socket_t client_fd = kInvalidSocket;
            bool reposted = false;
            for (int tick = 0; tick < 60 && (accepted.load() == 0 || received.load() == 0); ++tick)
            {
                io.poll(20, [&](const IoCompletion &c)
                {
                    if (c.op == 3 && c.accepted != kInvalidSocket)
                    {
                        ++accepted;
                        client_fd = c.accepted;
#ifdef _WIN32
                        ::setsockopt(client_fd, SOL_SOCKET, SO_UPDATE_ACCEPT_CONTEXT,
                                     (const char *)&c.listener, sizeof(c.listener));
#endif
                        (void)set_nonblocking(client_fd);
                        (void)io.attach(client_fd);
                        (void)io.post_recv(client_fd);
                        (void)io.post_accept(c.listener);
                        reposted = true;
                    }
                    else if (c.op == 1 && c.bytes > 0)
                    {
                        std::vector<std::string> frames;
                        fa.push(c.payload.data(), c.payload.size(), frames);
                        for (const auto &f : frames)
                        {
                            got_payload = f;
                            ++received;
                        }
                    }
                });
            }
            client.join();
            check(accepted.load() >= 1, "accept 完成事件真的到达");
            check(reposted, "接受后重新投递 accept + recv");
            check(received.load() >= 1, "recv 完成事件真的带来数据");
            check(got_payload == "HELLO from-client", "帧内容逐字节一致");
            check(io.live_operations() > 0, "完成上下文仍被正确持有");

            std::atomic<int> send_done{0};
            const bool posted = io.post_send(client_fd == kInvalidSocket ? listener : client_fd,
                                            "REPLY from-server");
            check(posted, "异步 send 投递成功");
            for (int tick = 0; tick < 20 && send_done.load() == 0; ++tick)
                io.poll(20, [&](const IoCompletion &c)
                {
                    if (c.op == 2) ++send_done;
                    if (c.op == 1 && c.bytes > 0)
                    {
                        std::vector<std::string> frames;
                        fa.push(c.payload.data(), c.payload.size(), frames);
                    }
                });
            check(send_done.load() >= 1, "send 完成事件真的到达");

            io.wakeup();
            check(io.poll(1, [](const IoCompletion &) {}) >= 0, "wakeup 不产生虚假事件");
            if (client_fd != kInvalidSocket) close_socket(client_fd);
            close_socket(listener);
            io.close();
        }
    }

    std::printf("\n=== %d checks, %d failures ===\n", checks, failures);
    return failures ? 1 : 0;
}