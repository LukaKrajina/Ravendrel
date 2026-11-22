#pragma once
#include "utils/A2LNet.hpp"
#ifdef _WIN32
#include <mswsock.h>
#endif

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace a2l
{
    namespace net
    {
        struct IoCompletion
        {
            socket_t fd = kInvalidSocket;
            socket_t accepted = kInvalidSocket;
            socket_t listener = kInvalidSocket;
            int op = 0;
            size_t bytes = 0;
            std::string payload;
            int error = 0;
        };

        class FrameAssembler
        {
        public:
            static constexpr uint32_t kMaxFrame = 1u << 24;

            void push(const char *p, size_t n, std::vector<std::string> &out)
            {
                buf_.append(p, n);
                for (;;)
                {
                    if (buf_.size() < 4) return;
                    const uint32_t len = frame_length(buf_.data());
                    if (len == 0 || len > kMaxFrame)
                    {
                        buf_.clear();
                        return;
                    }
                    if (buf_.size() < 4 + size_t(len)) return;
                    out.emplace_back(buf_.data() + 4, size_t(len));
                    buf_.erase(0, 4 + size_t(len));
                }
            }
            size_t pending() const { return buf_.size(); }
            void reset() { buf_.clear(); }
        private:
            std::string buf_;
        };

        class OverlappedIo
        {
        public:
            using OnCompletion = std::function<void(const IoCompletion &)>;

            OverlappedIo() = default;
            ~OverlappedIo() { close(); }
            OverlappedIo(const OverlappedIo &) = delete;
            OverlappedIo &operator=(const OverlappedIo &) = delete;

            static bool platform_available()
            {
#ifdef _WIN32
                HANDLE p = ::CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 0);
                if (!p) return false;
                ::CloseHandle(p);
                return true;
#elif defined(__linux__) && defined(A2L_HAS_IO_URING_HEADER)
                io_uring_params params{};
                const long fd = ::syscall(__NR_io_uring_setup, 8, &params);
                if (fd < 0) return false;
                ::close(int(fd));
                return true;
#else
                return false;
#endif
            }

            bool open()
            {
                if (opened_) return true;
                if (!NetRuntime::instance().ready() || !platform_available())
                {
                    reason_ = "completion IO unavailable on this platform/kernel";
                    return false;
                }
#ifdef _WIN32
                port_ = ::CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 0);
                if (!port_) { reason_ = "CreateIoCompletionPort failed"; return false; }
                SOCKET probe = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
                GUID guid_accept = WSAID_ACCEPTEX;
                DWORD bytes = 0;
                if (probe != INVALID_SOCKET)
                {
                    ::WSAIoctl(probe, SIO_GET_EXTENSION_FUNCTION_POINTER,
                               &guid_accept, sizeof(guid_accept),
                               &accept_ex_, sizeof(accept_ex_), &bytes, nullptr, nullptr);
                    ::closesocket(probe);
                }
                if (!accept_ex_)
                {
                    reason_ = "AcceptEx extension unavailable";
                    ::CloseHandle(port_); port_ = nullptr;
                    return false;
                }
                reason_ = "IOCP overlapped accept/recv/send active";
#elif defined(__linux__) && defined(A2L_HAS_IO_URING_HEADER)
                if (!uring_.setup(128))
                {
                    reason_ = "io_uring_setup failed";
                    return false;
                }
                reason_ = "io_uring IORING_OP_{ACCEPT,RECV,SEND} active";
#else
                reason_ = "no completion backend";
                return false;
#endif
                opened_ = true;
                return true;
            }

            void close()
            {
                if (!opened_) return;
#ifdef _WIN32
                if (port_) { ::CloseHandle(port_); port_ = nullptr; }
#elif defined(__linux__) && defined(A2L_HAS_IO_URING_HEADER)
                uring_.close_ring();
#endif
#ifdef _WIN32
                {
                    std::lock_guard<std::mutex> g(mu_);
                    live_.clear();
                }
#endif
                opened_ = false;
            }

            bool is_open() const { return opened_; }
            const std::string &reason() const { return reason_; }

            bool attach(socket_t fd)
            {
                if (!opened_) return false;
#ifdef _WIN32
                return ::CreateIoCompletionPort((HANDLE)fd, port_, (ULONG_PTR)fd, 0) != nullptr;
#else
                (void)fd;
                return true;
#endif
            }

            bool post_recv(socket_t fd)
            {
                if (!opened_) return false;
#ifdef _WIN32
                auto ctx = make_ctx(1, fd);
                if (!ctx) return false;
                ctx->buf.resize(8192);
                ctx->wbuf.len = ULONG(ctx->buf.size());
                ctx->wbuf.buf = ctx->buf.data();
                DWORD n = 0, flags = 0;
                const int r = ::WSARecv(fd, &ctx->wbuf, 1, &n, &flags, &ctx->ov, nullptr);
                if (r == 0) return true;
                const int e = ::WSAGetLastError();
                if (e == WSA_IO_PENDING) return true;
                release_ctx(ctx->ov_ptr());
                return false;
#else
                return uring_.submit_recv(int(fd), 8192);
#endif
            }

            bool post_send(socket_t fd, const std::string &payload)
            {
                if (!opened_) return false;
#ifdef _WIN32
                auto ctx = make_ctx(2, fd);
                if (!ctx) return false;
                const std::string frame = encode_frame(payload);
                ctx->buf.assign(frame.begin(), frame.end());
                ctx->wbuf.len = ULONG(ctx->buf.size());
                ctx->wbuf.buf = ctx->buf.data();
                DWORD n = 0;
                const int r = ::WSASend(fd, &ctx->wbuf, 1, &n, 0, &ctx->ov, nullptr);
                if (r == 0) return true;
                const int e = ::WSAGetLastError();
                if (e == WSA_IO_PENDING) return true;
                release_ctx(ctx->ov_ptr());
                return false;
#else
                return uring_.submit_send(int(fd), encode_frame(payload));
#endif
            }

            bool post_accept(socket_t listener)
            {
                if (!opened_) return false;
#ifdef _WIN32
                socket_t s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
                if (s == INVALID_SOCKET) return false;
                auto ctx = make_ctx(3, listener);
                if (!ctx) { close_socket(s); return false; }
                ctx->accepted = s;
                ctx->listener = listener;
                ctx->buf.resize(2 * (sizeof(sockaddr_in) + 16));
                DWORD got = 0;
                const BOOL ok = accept_ex_(listener, s, ctx->buf.data(), 0,
                                           DWORD(sizeof(sockaddr_in) + 16),
                                           DWORD(sizeof(sockaddr_in) + 16),
                                           &got, &ctx->ov);
                if (ok) return true;
                const int e = ::WSAGetLastError();
                if (e == WSA_IO_PENDING) return true;
                close_socket(s);
                release_ctx(ctx->ov_ptr());
                return false;
#else
                return uring_.submit_accept(int(listener));
#endif
            }




            size_t poll(int timeout_ms, const OnCompletion &cb)
            {
                if (!opened_) return 0;
                size_t delivered = 0;
#ifdef _WIN32
                OVERLAPPED_ENTRY entries[64];
                ULONG removed = 0;
                if (!::GetQueuedCompletionStatusEx(port_, entries, 64, &removed,
                                                   DWORD(std::max(0, timeout_ms)), FALSE))
                    return 0;
                for (ULONG i = 0; i < removed; ++i)
                {
                    auto *ctx = reinterpret_cast<Ctx *>(entries[i].lpOverlapped);
                    if (!ctx)
                    {
                        IoCompletion wake; wake.op = 4;
                        cb(wake); ++delivered;
                        continue;
                    }
                    IoCompletion c;
                    c.fd = ctx->fd;
                    c.op = ctx->op;
                    c.bytes = entries[i].dwNumberOfBytesTransferred;
                    c.accepted = ctx->accepted;
                    c.listener = ctx->listener;
                    if (ctx->op == 1 && c.bytes > 0 && c.bytes <= ctx->buf.size())
                        c.payload.assign(ctx->buf.data(), c.bytes);
                    const socket_t recv_fd = (ctx->op == 1) ? ctx->fd : kInvalidSocket;
                    const bool closed = (c.bytes == 0 && ctx->op == 1);
                    cb(c);
                    ++delivered;
                    release_ctx(reinterpret_cast<OVERLAPPED *>(ctx));
                    if (recv_fd != kInvalidSocket && !closed) post_recv(recv_fd);
                }
#elif defined(__linux__) && defined(A2L_HAS_IO_URING_HEADER)
                const auto completions = uring_.wait(timeout_ms);
                for (const auto &c : completions)
                {
                    const int recv_fd = (c.op == 1 && c.bytes > 0) ? int(c.fd) : -1;
                    cb(c);
                    ++delivered;
                    if (recv_fd >= 0) post_recv(socket_t(recv_fd));
                }
#endif
                return delivered;
            }

            void wakeup()
            {
#ifdef _WIN32
                if (opened_ && port_)
                    ::PostQueuedCompletionStatus(port_, 0, 0, nullptr);
#endif
            }

            size_t live_operations() const
            {
#ifdef _WIN32
                std::lock_guard<std::mutex> g(mu_);
                return live_.size();
#elif defined(__linux__) && defined(A2L_HAS_IO_URING_HEADER)
                return uring_.pending.size();
#else
                return 0;
#endif
            }

        private:
#ifdef _WIN32
            struct Ctx
            {
                WSAOVERLAPPED ov{};
                WSABUF wbuf{};
                std::vector<char> buf;
                int op = 0;
                socket_t fd = INVALID_SOCKET;
                socket_t accepted = INVALID_SOCKET;
                socket_t listener = INVALID_SOCKET;
                OVERLAPPED *ov_ptr() { return &ov; }
            };
            static_assert(offsetof(Ctx, ov) == 0, "WSAOVERLAPPED must be first");

            Ctx *make_ctx(int op, socket_t fd)
            {
                auto c = std::make_unique<Ctx>();
                c->op = op; c->fd = fd;
                std::lock_guard<std::mutex> g(mu_);
                live_.push_back(std::move(c));
                return live_.back().get();
            }
            void release_ctx(OVERLAPPED *ov)
            {
                if (!ov) return;
                std::lock_guard<std::mutex> g(mu_);
                for (auto it = live_.begin(); it != live_.end(); ++it)
                    if (reinterpret_cast<OVERLAPPED *>(it->get()) == ov)
                    { live_.erase(it); return; }
            }
            LPFN_ACCEPTEX accept_ex_ = nullptr;
            HANDLE port_ = nullptr;
            std::deque<std::unique_ptr<Ctx>> live_;
#elif defined(__linux__) && defined(A2L_HAS_IO_URING_HEADER)
            struct UringIo
            {
                int fd = -1;
                void *sq_ring = nullptr, *cq_ring = nullptr, *sqes = nullptr;
                size_t sq_size = 0, cq_size = 0, sqe_size = 0;
                unsigned *sq_head = nullptr, *sq_tail = nullptr, *sq_mask = nullptr;
                unsigned *sq_array = nullptr;
                unsigned *cq_head = nullptr, *cq_tail = nullptr, *cq_mask = nullptr;
                io_uring_cqe *cqes = nullptr;
                struct Pending
                {
                    int op = 0, sock = -1;
                    std::vector<char> buf;
                };
                std::unordered_map<uint64_t, Pending> pending;
                uint64_t next_id = 1;
                unsigned submitted = 0;

                bool setup(unsigned entries)
                {
                    io_uring_params p{};
                    fd = int(::syscall(__NR_io_uring_setup, entries, &p));
                    if (fd < 0) return false;
                    const size_t sqsz = p.sq_off.array + p.sq_entries * sizeof(unsigned);
                    const size_t cqsz = p.cq_off.cqes + p.cq_entries * sizeof(io_uring_cqe);
                    sq_ring = ::mmap(nullptr, sqsz, PROT_READ | PROT_WRITE,
                                     MAP_SHARED | MAP_POPULATE, fd, 0);
                    cq_ring = ::mmap(nullptr, cqsz, PROT_READ | PROT_WRITE,
                                     MAP_SHARED | MAP_POPULATE, fd, IORING_OFF_CQ_RING);
                    if (sq_ring == MAP_FAILED || cq_ring == MAP_FAILED) { close_ring(); return false; }
                    sq_size = sqsz; cq_size = cqsz;
                    sqe_size = p.sq_entries * sizeof(io_uring_sqe);
                    sqes = ::mmap(nullptr, sqe_size, PROT_READ | PROT_WRITE,
                                  MAP_SHARED | MAP_POPULATE, fd, IORING_OFF_SQES);
                    if (sqes == MAP_FAILED) { sqes = nullptr; close_ring(); return false; }
                    sq_head = (unsigned *)((char *)sq_ring + p.sq_off.head);
                    sq_tail = (unsigned *)((char *)sq_ring + p.sq_off.tail);
                    sq_mask = (unsigned *)((char *)sq_ring + p.sq_off.ring_mask);
                    sq_array = (unsigned *)((char *)sq_ring + p.sq_off.array);
                    cq_head = (unsigned *)((char *)cq_ring + p.cq_off.head);
                    cq_tail = (unsigned *)((char *)cq_ring + p.cq_off.tail);
                    cq_mask = (unsigned *)((char *)cq_ring + p.cq_off.ring_mask);
                    cqes = (io_uring_cqe *)((char *)cq_ring + p.cq_off.cqes);
                    return true;
                }
                void close_ring()
                {
                    if (sqes && sqe_size) ::munmap(sqes, sqe_size);
                    if (sq_ring && sq_size && sq_ring != MAP_FAILED) ::munmap(sq_ring, sq_size);
                    if (cq_ring && cq_size && cq_ring != MAP_FAILED) ::munmap(cq_ring, cq_size);
                    if (fd >= 0) ::close(fd);
                    fd = -1; sq_ring = cq_ring = sqes = nullptr;
                }
                io_uring_sqe *next(int op, int sock, uint64_t id)
                {
                    if (fd < 0 || !sqes) return nullptr;
                    const unsigned head = __atomic_load_n(sq_head, __ATOMIC_ACQUIRE);
                    const unsigned tail = *sq_tail + submitted;
                    if (tail - head >= 128) return nullptr;
                    auto *sqe = &((io_uring_sqe *)sqes)[tail & *sq_mask];
                    std::memset(sqe, 0, sizeof(*sqe));
                    sqe->fd = sock;
                    sqe->user_data = id | (uint64_t(op) << 56);
                    sq_array[tail & *sq_mask] = tail & *sq_mask;
                    ++submitted;
                    return sqe;
                }
                void kick()
                {
                    if (submitted == 0) return;
                    __atomic_store_n(sq_tail, *sq_tail + submitted, __ATOMIC_RELEASE);
                    const unsigned n = submitted;
                    submitted = 0;
                    ::syscall(__NR_io_uring_enter, fd, n, 0u, 0u, nullptr, 0);
                }
                bool submit_recv(int sock, size_t len)
                {
                    const uint64_t id = next_id++;
                    auto *sqe = next(1, sock, id);
                    if (!sqe) return false;
                    auto &p = pending[id];
                    p.op = 1; p.sock = sock;
                    p.buf.resize(len);
                    sqe->opcode = IORING_OP_RECV;
                    sqe->addr = (uint64_t)p.buf.data();
                    sqe->len = (uint32_t)len;
                    kick();
                    return true;
                }
                bool submit_send(int sock, const std::string &frame)
                {
                    const uint64_t id = next_id++;
                    auto *sqe = next(2, sock, id);
                    if (!sqe) return false;
                    auto &p = pending[id];
                    p.op = 2; p.sock = sock;
                    p.buf.assign(frame.begin(), frame.end());
                    sqe->opcode = IORING_OP_SEND;
                    sqe->addr = (uint64_t)p.buf.data();
                    sqe->len = (uint32_t)p.buf.size();
                    kick();
                    return true;
                }
                bool submit_accept(int listener)
                {
                    const uint64_t id = next_id++;
                    auto *sqe = next(3, listener, id);
                    if (!sqe) return false;
                    auto &p = pending[id];
                    p.op = 3; p.sock = listener;
                    sqe->opcode = IORING_OP_ACCEPT;
                    sqe->addr = 0;
                    sqe->addr2 = 0;
                    kick();
                    return true;
                }





                std::vector<IoCompletion> wait(int timeout_ms)
                {
                    std::vector<IoCompletion> out;
                    if (fd < 0) return out;
                    auto drain = [&]()
                    {
                        const unsigned head = __atomic_load_n(cq_head, __ATOMIC_ACQUIRE);
                        const unsigned tail = *cq_tail;
                        for (unsigned i = head; i < tail; ++i)
                        {
                            const auto &cqe = cqes[i & *cq_mask];
                            const uint64_t id = cqe.user_data & 0x00FFFFFFFFFFFFFFULL;
                            const int op = int(cqe.user_data >> 56);
                            auto it = pending.find(id);
                            if (it == pending.end()) continue;
                            IoCompletion c;
                            c.op = op;
                            c.fd = it->second.sock;
                            c.bytes = cqe.res >= 0 ? size_t(cqe.res) : 0;
                            c.error = cqe.res < 0 ? int(-cqe.res) : 0;
                            if (op == 1 && c.bytes > 0 && c.bytes <= it->second.buf.size())
                                c.payload.assign(it->second.buf.data(), c.bytes);
                            if (op == 3 && cqe.res >= 0) c.accepted = cqe.res;
                            pending.erase(it);
                            out.push_back(std::move(c));
                        }
                        if (tail != head) __atomic_store_n(cq_head, tail, __ATOMIC_RELEASE);
                    };
                    drain();
                    if (!out.empty()) return out;
                    pollfd pfd{};
                    pfd.fd = fd;
                    pfd.events = POLLIN;
                    const int r = ::poll(&pfd, 1, timeout_ms > 0 ? timeout_ms : 0);
                    if (r <= 0) return out;
                    drain();
                    return out;
                }
            } uring_;
#endif
            bool opened_ = false;
            std::string reason_;
            mutable std::mutex mu_;
        };






        class DistributedNodeCompletion
        {
        public:
            using OnMessage = std::function<void(const NetMessage &)>;

            explicit DistributedNodeCompletion(std::string id, size_t shards = 16)
                : id_(std::move(id)), coordinator_(shards, 1), pool_(2), running_(false) {}
            ~DistributedNodeCompletion() { stop(); }

            const std::string &id() const { return id_; }

            bool start(uint16_t port = 0)
            {
                if (running_) return true;
                if (!io_.open())
                {
                    last_error_ = io_.reason();
                    return false;
                }
                uint16_t actual = port;
                listener_ = make_listener(port, actual);
                if (listener_ == kInvalidSocket)
                {
                    last_error_ = "listen failed";
                    io_.close();
                    return false;
                }
                port_ = actual;
                if (!io_.attach(listener_) || !io_.post_accept(listener_))
                {
                    last_error_ = "AcceptEx post failed";
                    close_socket(listener_);
                    listener_ = kInvalidSocket;
                    io_.close();
                    return false;
                }
                running_ = true;
                io_thread_ = std::thread([this] { io_loop(); });
                return true;
            }

            void stop()
            {
                if (!running_.exchange(false)) return;
                io_.wakeup();
                close_socket(listener_);
                listener_ = kInvalidSocket;
                if (io_thread_.joinable()) io_thread_.join();
                std::lock_guard<std::mutex> g(mu_);
                for (auto &p : peers_) close_socket(p.fd);
                peers_.clear();
                assemblers_.clear();
                io_.close();
            }

            bool completion_active() const { return running_ && io_.is_open(); }
            const std::string &io_reason() const { return io_.reason(); }
            uint16_t port() const { return port_; }
            const std::string &error() const { return last_error_; }

            bool connect_peer(const std::string &host, uint16_t port,
                              const std::string &peer_id)
            {
                socket_t s = connect_to(host, port);
                if (s == kInvalidSocket) { last_error_ = "connect failed"; return false; }
                (void)io_.attach(s);
                {
                    std::lock_guard<std::mutex> g(mu_);
                    PeerInfo p;
                    p.id = peer_id; p.host = host; p.port = port;
                    p.fd = s; p.alive = true; p.last_heartbeat = now_ms();
                    peers_.push_back(p);
                    assemblers_[s] = FrameAssembler();
                }
                (void)io_.post_recv(s);
                (void)io_.post_send(s, "HELLO\n" + id_ + "\n");
                return true;
            }

            size_t broadcast(const std::string &topic, const std::string &body)
            {
                const std::string payload = topic + "\n" + id_ + "\n" + body;
                size_t posted = 0;
                std::lock_guard<std::mutex> g(mu_);
                for (auto &p : peers_)
                {
                    if (!p.alive || p.fd == kInvalidSocket) continue;
                    if (io_.post_send(p.fd, payload))
                    { ++posted; p.last_heartbeat = now_ms(); }
                    else p.alive = false;
                }
                return posted;
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
                strand_.execute(pool_, [this, seq, payload]() {
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
                    if (p.alive && now - p.last_heartbeat > timeout_ms)
                    { p.alive = false; ++dead; }
                return dead;
            }
            size_t live_operations() const { return io_.live_operations(); }

        private:
            static uint64_t now_ms()
            {
                return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count());
            }
            void io_loop()
            {
                while (running_)
                    io_.poll(20, [this](const IoCompletion &c) { on_completion(c); });
            }
            void on_completion(const IoCompletion &c)
            {
                if (c.op == 4) return;
                if (c.op == 2) return;
                if (c.op == 3)
                {
                    if (c.accepted == kInvalidSocket) return;
#ifdef _WIN32
                    ::setsockopt(c.accepted, SOL_SOCKET, SO_UPDATE_ACCEPT_CONTEXT,
                                 (const char *)&c.listener, sizeof(c.listener));
#endif
                    (void)set_nonblocking(c.accepted);
                    (void)io_.attach(c.accepted);
                    {
                        std::lock_guard<std::mutex> g(mu_);
                        PeerInfo p;
                        p.host = "accepted"; p.port = 0; p.fd = c.accepted;
                        p.alive = true; p.last_heartbeat = now_ms();
                        peers_.push_back(p);
                        assemblers_[c.accepted] = FrameAssembler();
                    }
                    (void)io_.post_recv(c.accepted);
                    if (c.listener != kInvalidSocket) (void)io_.post_accept(c.listener);
                    return;
                }
                if (c.op == 1)
                {
                    if (c.bytes == 0)
                    {
                        std::lock_guard<std::mutex> g(mu_);
                        for (auto &p : peers_) if (p.fd == c.fd) p.alive = false;
                        return;
                    }
                    std::vector<std::string> frames;
                    {
                        std::lock_guard<std::mutex> g(mu_);
                        auto it = assemblers_.find(c.fd);
                        if (it == assemblers_.end()) return;
                        it->second.push(c.payload.data(), c.payload.size(), frames);
                        for (auto &p : peers_) if (p.fd == c.fd) p.last_heartbeat = now_ms();
                    }
                    for (const auto &frame : frames) dispatch(frame, c.fd);
                }
            }
            void dispatch(const std::string &frame, socket_t fd)
            {
                const auto first = frame.find('\n');
                const auto second = first == std::string::npos
                    ? std::string::npos : frame.find('\n', first + 1);
                NetMessage m;
                if (first != std::string::npos && second != std::string::npos)
                {
                    m.topic = frame.substr(0, first);
                    m.from = frame.substr(first + 1, second - first - 1);
                    m.body = frame.substr(second + 1);
                }
                else { m.topic = "RAW"; m.body = frame; }
                if (m.topic == "HELLO")
                {
                    std::lock_guard<std::mutex> g(mu_);
                    for (auto &p : peers_) if (p.fd == fd) p.id = m.body;
                    return;
                }
                OnMessage cb;
                {
                    std::lock_guard<std::mutex> g(mu_);
                    cb = callback_;
                }
                if (cb) strand_.execute(pool_, [cb, m]() { cb(m); });
            }

            std::string id_;
            DistributedCoordinator coordinator_;
            ThreadPool pool_;
            Strand strand_;
            OverlappedIo io_;
            std::atomic<bool> running_;
            std::thread io_thread_;
            socket_t listener_ = kInvalidSocket;
            uint16_t port_ = 0;
            mutable std::mutex mu_;
            std::vector<PeerInfo> peers_;
            std::unordered_map<socket_t, FrameAssembler> assemblers_;
            OnMessage callback_;
            std::string last_error_;
        };

    }
}