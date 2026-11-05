#include "daemon_client.h"
#include <chrono>
#include <cstring>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#endif

namespace qgui {

// 与 daemon（runtime/src/main.cpp）的二进制帧协议对齐：
//   帧格式: [uint32 大端长度][payload]，payload 首字节为命令。
static const uint8_t kCmdHello = 0x00;         // 握手: payload[1..] = "RVDL_PROTO_V1"
static const uint8_t kCmdGetSnapshot = 0x09;   // 请求快照
static const char *kProtoVersion = "RVDL_PROTO_V1";

#ifdef _WIN32
using Socket = SOCKET;
static const Socket kInvalidSocket = INVALID_SOCKET;
#else
using Socket = int;
static const Socket kInvalidSocket = -1;
#endif

static void close_sock(Socket sock)
{
#ifdef _WIN32
    closesocket(sock);
#else
    close(sock);
#endif
}

static bool send_all(Socket sock, const void *buf, size_t n)
{
    const char *p = reinterpret_cast<const char *>(buf);
    size_t sent = 0;
    while (sent < n)
    {
        int r = static_cast<int>(send(sock, p + sent, static_cast<int>(n - sent), 0));
        if (r <= 0)
            return false;
        sent += static_cast<size_t>(r);
    }
    return true;
}

static bool recv_all(Socket sock, void *buf, size_t n)
{
    char *p = reinterpret_cast<char *>(buf);
    size_t got = 0;
    while (got < n)
    {
        int r = static_cast<int>(recv(sock, p + got, static_cast<int>(n - got), 0));
        if (r <= 0)
            return false;
        got += static_cast<size_t>(r);
    }
    return true;
}

static bool send_frame(Socket sock, const std::string &payload)
{
    uint32_t len = static_cast<uint32_t>(payload.size());
    uint8_t lenbuf[4] = {
        static_cast<uint8_t>((len >> 24) & 0xFF),
        static_cast<uint8_t>((len >> 16) & 0xFF),
        static_cast<uint8_t>((len >> 8) & 0xFF),
        static_cast<uint8_t>(len & 0xFF)};
    if (!send_all(sock, lenbuf, 4))
        return false;
    return payload.empty() || send_all(sock, payload.data(), payload.size());
}

static bool recv_frame(Socket sock, std::string &payload)
{
    uint8_t lenbuf[4];
    if (!recv_all(sock, lenbuf, 4))
        return false;
    uint32_t len = (static_cast<uint32_t>(lenbuf[0]) << 24) |
                   (static_cast<uint32_t>(lenbuf[1]) << 16) |
                   (static_cast<uint32_t>(lenbuf[2]) << 8) |
                   static_cast<uint32_t>(lenbuf[3]);
    if (len > 64u * 1024u * 1024u) // 防御:单帧最大 64MB
        return false;
    payload.resize(len);
    if (len > 0 && !recv_all(sock, payload.data(), len))
        return false;
    return true;
}

// 确保长连接就绪：若 socket 无效则建立连接并完成 HELLO 握手。
static bool ensure_connected(Socket &sock, const std::string &host, int port)
{
    if (sock != kInvalidSocket)
        return true;

    sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == kInvalidSocket)
        return false;

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<unsigned short>(port));
    inet_pton(AF_INET, host.c_str(), &addr.sin_addr);

#ifdef _WIN32
    if (connect(sock, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0)
    {
        close_sock(sock);
        sock = kInvalidSocket;
        return false;
    }
    DWORD timeout = 2000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&timeout), sizeof(timeout));
#else
    if (connect(sock, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0)
    {
        close_sock(sock);
        sock = kInvalidSocket;
        return false;
    }
    timeval tv{2, 0};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const void *>(&tv), sizeof(tv));
#endif

    // 握手帧
    std::string hello;
    hello.push_back(static_cast<char>(kCmdHello));
    hello += kProtoVersion;
    if (!send_frame(sock, hello))
    {
        close_sock(sock);
        sock = kInvalidSocket;
        return false;
    }
    return true;
}

// 通过长连接拉取一次快照；连接失效时关闭并在下次自动重连。
static bool fetch_snapshot(Socket &sock, const std::string &host, int port, StateSnapshot &out)
{
    if (!ensure_connected(sock, host, port))
        return false;

    std::string req;
    req.push_back(static_cast<char>(kCmdGetSnapshot));
    if (!send_frame(sock, req))
    {
        close_sock(sock);
        sock = kInvalidSocket;
        return false;
    }

    std::string data;
    if (!recv_frame(sock, data))
    {
        close_sock(sock);
        sock = kInvalidSocket;
        return false;
    }

    if (data.empty())
        return false;
    return deserialize(data, out);
}

void DaemonClient::run(const std::string &host, int port)
{
#ifdef _WIN32
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);
#endif

    while (running_)
    {
        StateSnapshot snap;
        if (fetch_snapshot(sock_, host, port, snap))
        {
            connected_ = true;
            uint64_t gen = snap.generation;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                snapshot_ = std::move(snap);
            }
            generation_ = gen;
        }
        else
        {
            connected_ = false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    if (sock_ != kInvalidSocket)
    {
        close_sock(sock_);
        sock_ = kInvalidSocket;
    }

#ifdef _WIN32
    WSACleanup();
#endif
}

void DaemonClient::start(const std::string &host, int port)
{
    if (running_.exchange(true))
        return;
    worker_ = std::thread([this, host, port]
                          { run(host, port); });
}

void DaemonClient::stop()
{
    running_ = false;
    if (worker_.joinable())
        worker_.join();
}

} // namespace qgui
