#pragma once
#include <thread>
#include <atomic>
#include <mutex>
#include <string>
#include "../protocol.hpp"

#ifdef _WIN32
#include <winsock2.h>
#endif

namespace qgui {

class DaemonClient {
public:
    DaemonClient() = default;
    ~DaemonClient() { stop(); }

    DaemonClient(const DaemonClient &) = delete;
    DaemonClient &operator=(const DaemonClient &) = delete;

    void start(const std::string &host, int port);
    void stop();

    StateSnapshot snapshot()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return snapshot_;
    }

    bool connected() const { return connected_.load(); }
    uint64_t generation() const { return generation_.load(); }

private:
    void run(const std::string &host, int port);

#ifdef _WIN32
    using Socket = SOCKET;
    static constexpr Socket kInvalidSocket = INVALID_SOCKET;
#else
    using Socket = int;
    static constexpr Socket kInvalidSocket = -1;
#endif

    Socket sock_ = kInvalidSocket;
    std::thread worker_;
    std::atomic<bool> running_{false};
    std::atomic<bool> connected_{false};
    std::atomic<uint64_t> generation_{0};
    std::mutex mutex_;
    StateSnapshot snapshot_;
};

}