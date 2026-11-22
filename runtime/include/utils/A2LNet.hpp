#pragma once

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef _WINSOCK2API_
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mswsock.h>
#endif
#include <windows.h>
#else
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>
#endif

#include "utils/A2L.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if defined(__linux__)
#include <poll.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#if __has_include(<linux/io_uring.h>)
#include <linux/io_uring.h>
#define A2L_HAS_IO_URING_HEADER 1
#endif
#endif

namespace a2l
{
    namespace net
    {
#ifdef _WIN32
        using socket_t = SOCKET;
        static constexpr socket_t kInvalidSocket = INVALID_SOCKET;
        inline void close_socket(socket_t s) { if (s != INVALID_SOCKET) ::closesocket(s); }
        inline int last_error() { return ::WSAGetLastError(); }
#else
        using socket_t = int;
        static constexpr socket_t kInvalidSocket = -1;
        inline void close_socket(socket_t s) { if (s >= 0) ::close(s); }
        inline int last_error() { return errno; }
        using OVERLAPPED_ENTRY = void;
#endif

        enum class IoBackendKind
        {
            Poll,
            IOCP,
            IoUring,
            Epoll,
            None
        };

        inline const char *backend_name(IoBackendKind k)
        {
            switch (k)
            {
                case IoBackendKind::IOCP: return "iocp";
                case IoBackendKind::IoUring: return "io_uring";
                case IoBackendKind::Epoll: return "epoll";
                case IoBackendKind::Poll: return "poll";
                default: return "none";
            }
        }

        struct NetCapabilities
        {
            IoBackendKind backend = IoBackendKind::None;
            bool usable = false;
            std::string reason;
            int max_batch = 1;
        };

        class NetRuntime
        {
        public:
            static NetRuntime &instance()
            {
                static NetRuntime r;
                return r;
            }
            bool ready() const { return ready_; }
        private:
            NetRuntime()
            {
#ifdef _WIN32
                WSADATA wsa{};
                ready_ = (::WSAStartup(MAKEWORD(2, 2), &wsa) == 0);
#else
                ready_ = true;
#endif
            }
            ~NetRuntime()
            {
#ifdef _WIN32
                if (ready_) ::WSACleanup();
#endif
            }
            bool ready_ = false;
        };

        inline bool set_nonblocking(socket_t s)
        {
#ifdef _WIN32
            u_long mode = 1;
            return ::ioctlsocket(s, FIONBIO, &mode) == 0;
#else
            const int fl = ::fcntl(s, F_GETFL, 0);
            return fl >= 0 && ::fcntl(s, F_SETFL, fl | O_NONBLOCK) == 0;
#endif
        }

        inline socket_t make_listener(uint16_t port, uint16_t &actual_port)
        {
            if (!NetRuntime::instance().ready()) return kInvalidSocket;
            socket_t s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (s == kInvalidSocket) return kInvalidSocket;
            int one = 1;
#ifdef _WIN32
            ::setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char *)&one, sizeof(one));
#else
            ::setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
#endif
            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
            addr.sin_port = ::htons(port);
            if (::bind(s, (sockaddr *)&addr, sizeof(addr)) != 0 ||
                ::listen(s, 64) != 0)
            {
                close_socket(s);
                return kInvalidSocket;
            }
            if (port == 0)
            {
                sockaddr_in got{};
#ifdef _WIN32
                int len = sizeof(got);
#else
                socklen_t len = sizeof(got);
#endif
                if (::getsockname(s, (sockaddr *)&got, &len) == 0)
                    actual_port = ::ntohs(got.sin_port);
            }
            else actual_port = port;
            (void)set_nonblocking(s);
            return s;
        }

        inline socket_t connect_to(const std::string &host, uint16_t port)
        {
            if (!NetRuntime::instance().ready()) return kInvalidSocket;
            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_port = ::htons(port);
            if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1)
                addr.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
            socket_t s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (s == kInvalidSocket) return kInvalidSocket;
            if (::connect(s, (sockaddr *)&addr, sizeof(addr)) != 0)
            {
                close_socket(s);
                return kInvalidSocket;
            }
            int one = 1;
            ::setsockopt(s, IPPROTO_TCP, TCP_NODELAY,
#ifdef _WIN32
                         (const char *)&one,
#else
                         &one,
#endif
                         sizeof(one));
            return s;
        }

        inline std::string encode_frame(const std::string &payload)
        {
            std::string out(4, '\0');
            const uint32_t n = static_cast<uint32_t>(payload.size());
            out[0] = char((n >> 24) & 0xFF); out[1] = char((n >> 16) & 0xFF);
            out[2] = char((n >> 8) & 0xFF);  out[3] = char(n & 0xFF);
            out += payload;
            return out;
        }
        inline uint32_t frame_length(const char *p)
        {
            return (uint32_t(uint8_t(p[0])) << 24) | (uint32_t(uint8_t(p[1])) << 16) |
                   (uint32_t(uint8_t(p[2])) << 8) | uint32_t(uint8_t(p[3]));
        }

        inline bool send_frame(socket_t s, const std::string &payload)
        {
            const std::string f = encode_frame(payload);
            size_t sent = 0;
            while (sent < f.size())
            {
#ifdef _WIN32
                const int n = ::send(s, f.data() + sent, int(f.size() - sent), 0);
#else
                const ssize_t n = ::send(s, f.data() + sent, f.size() - sent, MSG_NOSIGNAL);
#endif
                if (n <= 0) return false;
                sent += size_t(n);
            }
            return true;
        }










        inline bool recv_frame(socket_t s, std::string &out, int timeout_ms = 0)
        {
            const bool nonblocking = timeout_ms < 0;
            char head[4];
            const auto deadline = std::chrono::steady_clock::now() +
                                  std::chrono::milliseconds(timeout_ms > 0 ? timeout_ms : 0);
            auto time_left = [&]() -> int
            {
                const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    deadline - std::chrono::steady_clock::now()).count();
                return ms > 0 ? int(ms) : 0;
            };
            auto read_exact = [&](char *dst, size_t n) -> bool
            {
                size_t off = 0;
                while (off < n)
                {
                    fd_set rd; FD_ZERO(&rd); FD_SET(s, &rd);
                    timeval tv{};
                    tv.tv_sec = 0;
                    tv.tv_usec = nonblocking ? 0 : 5000;
#ifdef _WIN32
                    const int r = ::select(0, &rd, nullptr, nullptr, &tv);
#else
                    const int r = ::select(int(s) + 1, &rd, nullptr, nullptr, &tv);
#endif
                    if (r == 0)
                    {
                        if (nonblocking) return false;
                        if (timeout_ms > 0 && time_left() == 0) return false;
                        continue;
                    }
                    if (r < 0) return false;
#ifdef _WIN32
                    const int k = ::recv(s, dst + off, int(n - off), 0);
#else
                    const ssize_t k = ::recv(s, dst + off, n - off, 0);
#endif
                    if (k <= 0) return false;
                    off += size_t(k);
                }
                return true;
            };
            if (!read_exact(head, 4)) return false;
            const uint32_t n = frame_length(head);
            if (n == 0 || n > (1u << 24)) return false;
            out.assign(size_t(n), '\0');
            return read_exact(&out[0], size_t(n));
        }







        struct NetEvent
        {
            socket_t fd = kInvalidSocket;
            uint32_t events = 0;
            size_t bytes = 0;
            void *context = nullptr;
        };

        class NativeReactor
        {
        public:
            using OnEvent = std::function<void(const NetEvent &)>;

            NativeReactor() = default;
            ~NativeReactor() { close(); }
            NativeReactor(const NativeReactor &) = delete;
            NativeReactor &operator=(const NativeReactor &) = delete;

            bool open()
            {
                if (opened_) return true;
                caps_ = probe();
                if (!caps_.usable) return false;
                backend_ = caps_.backend;
#ifdef _WIN32
                if (backend_ == IoBackendKind::IOCP)
                {
                    port_ = ::CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 0);
                    if (!port_)
                    {
                        caps_.usable = false;
                        caps_.reason = "CreateIoCompletionPort failed";
                        return false;
                    }
                }
#elif defined(__linux__)
                if (backend_ == IoBackendKind::IoUring)
                {
                    if (!uring_.setup(64))
                    {
                        backend_ = IoBackendKind::Epoll;
                        caps_.backend = IoBackendKind::Epoll;
                        caps_.reason = "io_uring_setup failed -> epoll fallback";
                        epfd_ = ::epoll_create1(0);
                        if (epfd_ < 0) return open_poll();
                        return (opened_ = true);
                    }
                }
                else if (backend_ == IoBackendKind::Epoll)
                {
                    epfd_ = ::epoll_create1(0);
                    if (epfd_ < 0) return open_poll();
                    return (opened_ = true);
                }
                else return open_poll();
#endif
                opened_ = true;
                return true;
            }

            bool open_poll()
            {
                backend_ = IoBackendKind::Poll;
                caps_.backend = IoBackendKind::Poll;
                caps_.usable = true;
                caps_.reason = "portable select fallback";
                opened_ = true;
                return true;
            }

            void close()
            {
                if (!opened_) return;
#ifdef _WIN32
                if (port_) { ::CloseHandle(port_); port_ = nullptr; }
#elif defined(__linux__)
                uring_.close_ring();
                if (epfd_ >= 0) { ::close(epfd_); epfd_ = -1; }
#endif
                opened_ = false;
            }

            bool add(socket_t fd, uint32_t events = 1)
            {
                if (!opened_) return false;
#ifdef _WIN32
                if (backend_ == IoBackendKind::IOCP)
                {
                    if (!::CreateIoCompletionPort((HANDLE)fd, port_,
                                                  (ULONG_PTR)fd, 0))
                        return false;
                }
#elif defined(__linux__)
                if (backend_ == IoBackendKind::Epoll)
                {
                    epoll_event ev{};
                    ev.events = (events & 1 ? EPOLLIN : 0) | (events & 2 ? EPOLLOUT : 0);
                    ev.data.fd = int(fd);
                    if (::epoll_ctl(epfd_, EPOLL_CTL_ADD, int(fd), &ev) != 0) return false;
                }
                else if (backend_ == IoBackendKind::IoUring)
                {
                    if (!uring_.poll_add(int(fd), events)) return false;
                }
#endif
                {
                    std::lock_guard<std::mutex> g(mu_);
                    fds_.push_back(fd);
                }
                return true;
            }

            void remove(socket_t fd)
            {
                std::lock_guard<std::mutex> g(mu_);
                fds_.erase(std::remove(fds_.begin(), fds_.end(), fd), fds_.end());
            }

            size_t poll_once(int timeout_ms, const OnEvent &on_event)
            {
                if (!opened_) return 0;
                size_t delivered = 0;
#ifdef _WIN32
                if (backend_ == IoBackendKind::IOCP)
                {
                    OVERLAPPED_ENTRY entries[64];
                    ULONG removed = 0;
                    if (::GetQueuedCompletionStatusEx(port_, entries, 64, &removed, 0, FALSE))
                        for (ULONG i = 0; i < removed; ++i)
                        {
                            NetEvent e;
                            e.fd = (socket_t)entries[i].lpCompletionKey;
                            e.events = 4;
                            e.bytes = entries[i].dwNumberOfBytesTransferred;
                            e.context = entries[i].lpOverlapped;
                            on_event(e);
                            ++delivered;
                        }
                }
#elif defined(__linux__)
                if (backend_ == IoBackendKind::IoUring)
                {
                    const auto ready = uring_.wait(timeout_ms);
                    for (int fd : ready)
                    {
                        NetEvent e; e.fd = fd; e.events = 1;
                        on_event(e); ++delivered;
                    }
                    return delivered;
                }
                if (backend_ == IoBackendKind::Epoll)
                {
                    epoll_event evs[64];
                    const int n = ::epoll_wait(epfd_, evs, 64, std::max(0, timeout_ms));
                    if (n <= 0) return 0;
                    for (int i = 0; i < n; ++i)
                    {
                        NetEvent e;
                        e.fd = evs[i].data.fd;
                        e.events = ((evs[i].events & EPOLLIN) ? 1u : 0u) |
                                   ((evs[i].events & EPOLLOUT) ? 2u : 0u);
                        on_event(e); ++delivered;
                    }
                    return delivered;
                }
#endif
                std::vector<socket_t> snapshot;
                {
                    std::lock_guard<std::mutex> g(mu_);
                    snapshot = fds_;
                }
                if (snapshot.empty())
                {
                    if (timeout_ms > 0)
                        std::this_thread::sleep_for(std::chrono::milliseconds(
                            std::min<int>(timeout_ms, 5)));
                    return 0;
                }
                fd_set rd; FD_ZERO(&rd);
                socket_t maxfd = 0;
                for (socket_t fd : snapshot) { FD_SET(fd, &rd); if (fd > maxfd) maxfd = fd; }
                timeval tv{};
                tv.tv_sec = 0;
                tv.tv_usec = std::min<int>(timeout_ms <= 0 ? 5000 : timeout_ms, 100) * 1000;
#ifdef _WIN32
                const int r = ::select(0, &rd, nullptr, nullptr, &tv);
#else
                const int r = ::select(int(maxfd) + 1, &rd, nullptr, nullptr, &tv);
#endif
                if (r <= 0) return 0;
                for (socket_t fd : snapshot)
                    if (FD_ISSET(fd, &rd))
                    {
                        NetEvent e; e.fd = fd; e.events = 1;
                        on_event(e); ++delivered;
                    }
                return delivered;
            }

            void wakeup()
            {
#ifdef _WIN32
                if (opened_ && port_ && backend_ == IoBackendKind::IOCP)
                    ::PostQueuedCompletionStatus(port_, 0, 0, nullptr);
#endif
                (void)0;
            }

            IoBackendKind backend() const { return backend_; }
            const NetCapabilities &capabilities() const { return caps_; }
            size_t registered() const
            {
                std::lock_guard<std::mutex> g(mu_);
                return fds_.size();
            }

            static NetCapabilities probe()
            {
                NetCapabilities c;
                if (!NetRuntime::instance().ready())
                {
                    c.reason = "network runtime unavailable";
                    return c;
                }
#ifdef _WIN32
                HANDLE p = ::CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 0);
                if (p)
                {
                    ::CloseHandle(p);
                    c.backend = IoBackendKind::IOCP;
                    c.usable = true;
                    c.max_batch = 64;
                    c.reason = "Windows I/O completion port available";
                }
                else
                {
                    c.backend = IoBackendKind::Poll;
                    c.usable = true;
                    c.reason = "IOCP unavailable -> select fallback";
                }
#elif defined(__linux__)
                {
                    const int e = ::epoll_create1(0);
                    if (e >= 0)
                    {
                        ::close(e);
                        c.backend = IoBackendKind::Epoll;
                        c.usable = true;
                        c.max_batch = 64;
                        c.reason = "epoll available (level-triggered readiness)";
                    }
                    else
                    {
                        c.backend = IoBackendKind::Poll;
                        c.usable = true;
                        c.reason = "no epoll -> select fallback";
                    }
                }
#else
                c.backend = IoBackendKind::Poll;
                c.usable = true;
                c.reason = "portable select fallback";
#endif
                return c;
            }

        private:
#if defined(__linux__)
            struct Uring
            {
                int fd = -1;
                void *sq_ring = nullptr, *cq_ring = nullptr, *sqes = nullptr;
                size_t sq_size = 0, cq_size = 0, sqe_size = 0;
                unsigned *sq_head = nullptr, *sq_tail = nullptr, *sq_mask = nullptr;
                unsigned *sq_array = nullptr;
                unsigned *cq_head = nullptr, *cq_tail = nullptr, *cq_mask = nullptr;
                struct io_uring_cqe *cqes = nullptr;
                unsigned sq_ring_sz = 0, cq_ring_sz = 0;

                bool setup(unsigned entries)
                {
#ifdef A2L_HAS_IO_URING_HEADER
                    io_uring_params p{};
                    fd = int(::syscall(__NR_io_uring_setup, entries, &p));
                    if (fd < 0) return false;
                    sq_ring_sz = p.sq_off.array + p.sq_entries * sizeof(unsigned);
                    cq_ring_sz = p.cq_off.cqes + p.cq_entries * sizeof(io_uring_cqe);
                    sq_size = sq_ring_sz; cq_size = cq_ring_sz;
                    sq_ring = ::mmap(nullptr, sq_size, PROT_READ | PROT_WRITE,
                                     MAP_SHARED | MAP_POPULATE, fd, 0);
                    cq_ring = ::mmap(nullptr, cq_size, PROT_READ | PROT_WRITE,
                                     MAP_SHARED | MAP_POPULATE, fd, IORING_OFF_CQ_RING);
                    if (sq_ring == MAP_FAILED || cq_ring == MAP_FAILED) { close_ring(); return false; }
                    const size_t sqe_bytes = p.sq_entries * sizeof(io_uring_sqe);
                    sqes = ::mmap(nullptr, sqe_bytes, PROT_READ | PROT_WRITE,
                                  MAP_SHARED | MAP_POPULATE, fd, IORING_OFF_SQES);
                    if (sqes == MAP_FAILED) { sqes = nullptr; close_ring(); return false; }
                    sqe_size = sqe_bytes;
                    sq_head = (unsigned *)((char *)sq_ring + p.sq_off.head);
                    sq_tail = (unsigned *)((char *)sq_ring + p.sq_off.tail);
                    sq_mask = (unsigned *)((char *)sq_ring + p.sq_off.ring_mask);
                    sq_array = (unsigned *)((char *)sq_ring + p.sq_off.array);
                    cq_head = (unsigned *)((char *)cq_ring + p.cq_off.head);
                    cq_tail = (unsigned *)((char *)cq_ring + p.cq_off.tail);
                    cq_mask = (unsigned *)((char *)cq_ring + p.cq_off.ring_mask);
                    cqes = (io_uring_cqe *)((char *)cq_ring + p.cq_off.cqes);
                    return true;
#else
                    (void)entries;
                    return false;
#endif
                }
                void close_ring()
                {
#ifdef A2L_HAS_IO_URING_HEADER
                    if (sqes && sqe_size) ::munmap(sqes, sqe_size);
                    if (sq_ring && sq_size && sq_ring != MAP_FAILED) ::munmap(sq_ring, sq_size);
                    if (cq_ring && cq_size && cq_ring != MAP_FAILED) ::munmap(cq_ring, cq_size);
                    if (fd >= 0) ::close(fd);
#endif
                    fd = -1; sq_ring = cq_ring = sqes = nullptr;
                }
                bool poll_add(int sock, uint32_t )
                {
#ifdef A2L_HAS_IO_URING_HEADER
                    if (fd < 0 || !sqes) return false;
                    const unsigned head = __atomic_load_n(sq_head, __ATOMIC_ACQUIRE);
                    const unsigned tail = *sq_tail;
                    if (tail - head >= 64) return false;
                    auto *sqe = &((io_uring_sqe *)sqes)[tail & *sq_mask];
                    std::memset(sqe, 0, sizeof(*sqe));
                    sqe->opcode = IORING_OP_POLL_ADD;
                    sqe->fd = sock;
                    sqe->poll_events = POLLIN;
                    sqe->user_data = uint64_t(uint32_t(sock));
                    sq_array[tail & *sq_mask] = tail & *sq_mask;
                    __atomic_store_n(sq_tail, tail + 1, __ATOMIC_RELEASE);
                    ::syscall(__NR_io_uring_enter, fd, 1u, 0u, 0u, nullptr, 0);
                    return true;
#else
                    (void)sock;
                    return false;
#endif
                }
                std::vector<int> wait(int timeout_ms)
                {
                    std::vector<int> out;
#ifdef A2L_HAS_IO_URING_HEADER
                    if (fd < 0) return out;
                    auto drain = [&]()
                    {
                        const unsigned head = __atomic_load_n(cq_head, __ATOMIC_ACQUIRE);
                        const unsigned tail = *cq_tail;
                        for (unsigned i = head; i < tail; ++i)
                        {
                            const auto &cqe = cqes[i & *cq_mask];
                            if (cqe.res >= 0) out.push_back(int(uint32_t(cqe.user_data)));
                        }
                        if (tail != head) __atomic_store_n(cq_head, tail, __ATOMIC_RELEASE);
                    };
                    drain();
                    if (!out.empty()) return out;
                    pollfd pfd{};
                    pfd.fd = fd;
                    pfd.events = POLLIN;
                    if (::poll(&pfd, 1, timeout_ms > 0 ? timeout_ms : 0) > 0) drain();
#endif
                    return out;
                }
            } uring_;
            int epfd_ = -1;
#endif
#ifdef _WIN32
            HANDLE port_ = nullptr;
#endif
            IoBackendKind backend_ = IoBackendKind::None;
            NetCapabilities caps_;
            bool opened_ = false;
            mutable std::mutex mu_;
            std::vector<socket_t> fds_;
        };

        struct PeerInfo
        {
            std::string id;
            std::string host;
            uint16_t port = 0;
            socket_t fd = kInvalidSocket;
            bool alive = false;
            uint64_t last_heartbeat = 0;
        };

        struct NetMessage
        {
            std::string topic;
            std::string body;
            std::string from;
        };

        class DistributedNode
        {
        public:
            using OnMessage = std::function<void(const NetMessage &)>;

            explicit DistributedNode(std::string id, size_t shards = 16)
                : id_(std::move(id)), coordinator_(shards, 1), pool_(2),
                  running_(false) {}
            ~DistributedNode() { stop(); }

            const std::string &id() const { return id_; }

            bool start(uint16_t port = 0)
            {
                if (running_) return true;
                reactor_caps_ = NativeReactor::probe();
                if (!reactor_.open())
                {
                    last_error_ = "reactor open failed: " + reactor_caps_.reason;
                    return false;
                }
                uint16_t actual = port;
                listener_ = make_listener(port, actual);
                if (listener_ == kInvalidSocket)
                {
                    last_error_ = "listen failed";
                    reactor_.close();
                    return false;
                }
                port_ = actual;
                reactor_.add(listener_, 1);
                running_ = true;
                io_thread_ = std::thread([this] { io_loop(); });
                return true;
            }

            void stop()
            {
                if (!running_.exchange(false)) return;
                reactor_.wakeup();
                close_socket(listener_);
                listener_ = kInvalidSocket;
                if (io_thread_.joinable()) io_thread_.join();
                std::lock_guard<std::mutex> g(mu_);
                for (auto &p : peers_) close_socket(p.fd);
                peers_.clear();
                reactor_.close();
            }

            uint16_t port() const { return port_; }
            IoBackendKind backend() const { return reactor_.backend(); }
            const std::string &error() const { return last_error_; }

            bool connect_peer(const std::string &host, uint16_t port,
                              const std::string &peer_id)
            {
                socket_t s = connect_to(host, port);
                if (s == kInvalidSocket)
                {
                    last_error_ = "connect failed";
                    return false;
                }
                (void)send_frame(s, "HELLO\n" + id_ + "\n");
                {
                    std::lock_guard<std::mutex> g(mu_);
                    PeerInfo p;
                    p.id = peer_id; p.host = host; p.port = port;
                    p.fd = s; p.alive = true;
                    p.last_heartbeat = now_ms();
                    peers_.push_back(p);
                }
                reactor_.add(s, 1);
                return true;
            }

            size_t broadcast(const std::string &topic, const std::string &body)
            {
                const std::string frame = topic + "\n" + id_ + "\n" + body;
                size_t sent = 0;
                std::lock_guard<std::mutex> g(mu_);
                for (auto &p : peers_)
                {
                    if (!p.alive || p.fd == kInvalidSocket) continue;
                    if (send_frame(p.fd, frame)) { ++sent; p.last_heartbeat = now_ms(); }
                    else p.alive = false;
                }
                return sent;
            }

            void on_message(OnMessage cb)
            {
                std::lock_guard<std::mutex> g(mu_);
                callback_ = std::move(cb);
            }

            size_t peer_count() const
            {
                std::lock_guard<std::mutex> g(mu_);
                return peers_.size();
            }
            size_t alive_peers() const
            {
                std::lock_guard<std::mutex> g(mu_);
                size_t n = 0;
                for (const auto &p : peers_) if (p.alive) ++n;
                return n;
            }
            std::vector<PeerInfo> peers() const
            {
                std::lock_guard<std::mutex> g(mu_);
                return peers_;
            }

            bool lease(size_t shard, uint64_t epoch) { return coordinator_.try_lease(shard, 0, epoch); }
            size_t checkpoint(const std::string &payload)
            {
                const size_t seq = coordinator_.commit_checkpoint();
                strand_.execute(pool_, [this, seq, payload] {
                    broadcast("CHECKPOINT", std::to_string(seq) + ":" + payload);
                }).wait();
                return seq;
            }
            size_t checkpoint_seq() const { return coordinator_.checkpoint(); }
            size_t shards() const { return coordinator_.shards(); }

            size_t heartbeat(uint64_t timeout_ms = 5000)
            {
                const uint64_t now = now_ms();
                size_t dead = 0;
                std::lock_guard<std::mutex> g(mu_);
                for (auto &p : peers_)
                {
                    if (p.alive && now - p.last_heartbeat > timeout_ms)
                    { p.alive = false; ++dead; }
                }
                return dead;
            }

        private:
            static uint64_t now_ms()
            {
                return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count());
            }

            void io_loop()
            {
                while (running_)
                {
                    const size_t n = reactor_.poll_once(20, [this](const NetEvent &e) {
                        this->on_event(e);
                    });
                    if (n == 0) continue;
                }
            }

            void on_event(const NetEvent &e)
            {
                if (e.fd == listener_)
                {
                    sockaddr_in remote{};
#ifdef _WIN32
                    int len = sizeof(remote);
#else
                    socklen_t len = sizeof(remote);
#endif
                    const socket_t c = ::accept(listener_, (sockaddr *)&remote, &len);
                    if (c == kInvalidSocket) return;
                    (void)set_nonblocking(c);
                    {
                        std::lock_guard<std::mutex> g(mu_);
                        PeerInfo p;
                        char buf[INET_ADDRSTRLEN] = {0};
                        ::inet_ntop(AF_INET, &remote.sin_addr, buf, sizeof(buf));
                        p.host = buf;
                        p.port = ::ntohs(remote.sin_port);
                        p.fd = c; p.alive = true;
                        p.last_heartbeat = now_ms();
                        peers_.push_back(p);
                    }
                    reactor_.add(c, 1);
                    return;
                }
                for (int guard = 0; guard < 64; ++guard)
                {
                    std::string payload;
                    if (!recv_frame(e.fd, payload, 0)) break;
                    const auto first = payload.find('\n');
                    const auto second = first == std::string::npos
                        ? std::string::npos : payload.find('\n', first + 1);
                    NetMessage m;
                    if (first != std::string::npos && second != std::string::npos)
                    {
                        m.topic = payload.substr(0, first);
                        m.from = payload.substr(first + 1, second - first - 1);
                        m.body = payload.substr(second + 1);
                    }
                    else { m.topic = "RAW"; m.body = payload; }
                    if (m.topic == "HELLO")
                    {
                        std::lock_guard<std::mutex> g(mu_);
                        for (auto &p : peers_) if (p.fd == e.fd) p.id = m.body;
                    }
                    else
                    {
                        OnMessage cb;
                        {
                            std::lock_guard<std::mutex> g(mu_);
                            cb = callback_;
                        }
                        if (cb) strand_.execute(pool_, [cb, m] { cb(m); });
                    }
                    fd_set rd; FD_ZERO(&rd); FD_SET(e.fd, &rd);
                    timeval tv{};
                    tv.tv_sec = 0; tv.tv_usec = 0;
#ifdef _WIN32
                    const int more = ::select(0, &rd, nullptr, nullptr, &tv);
#else
                    const int more = ::select(int(e.fd) + 1, &rd, nullptr, nullptr, &tv);
#endif
                    if (more <= 0 || !FD_ISSET(e.fd, &rd)) break;
                }
            }

            std::string id_;
            DistributedCoordinator coordinator_;
            ThreadPool pool_;
            Strand strand_;
            NativeReactor reactor_;
            NetCapabilities reactor_caps_;
            std::atomic<bool> running_;
            std::thread io_thread_;
            socket_t listener_ = kInvalidSocket;
            uint16_t port_ = 0;
            mutable std::mutex mu_;
            std::vector<PeerInfo> peers_;
            OnMessage callback_;
            std::string last_error_;
        };

    }
}