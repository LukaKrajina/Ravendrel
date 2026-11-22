#pragma once
#include <string>
#include <cstring>
#include <cstdint>
#include <vector>

#include "rvdl_jit_host.hpp"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <unistd.h>
#endif

namespace rvdlrsp::editor
{

    enum class CompileStatus
    {
        Success,
        EmptyBuffer,
        DaemonUnreachable,
        CompileError,
    };

    enum class Backend
    {
        Daemon,
        Embedded,
    };

    struct CompileResult
    {
        CompileStatus status = CompileStatus::EmptyBuffer;
        std::string message;
        bool ok() const { return status == CompileStatus::Success; }
    };

    struct ExecuteResult
    {
        bool ok = false;
        std::string output;
        long long int_value = 0;
        double float_value = 0.0;
        std::string message;
    };

    class QkEditor
    {
    private:
        std::string buffer_;
        std::string host_ = "127.0.0.1";
        int port_ = 50052;
        int timeout_ms_ = 8000;
        std::string last_error_;

        Backend backend_ = Backend::Daemon;

        struct Socket
        {
#ifdef _WIN32
            using Handle = SOCKET;
            static constexpr Handle kInvalid = INVALID_SOCKET;
            Handle fd = kInvalid;
#else
            using Handle = int;
            static constexpr Handle kInvalid = -1;
            Handle fd = kInvalid;
#endif
            Socket() = default;
            ~Socket() { close(); }
            Socket(const Socket &) = delete;
            Socket &operator=(const Socket &) = delete;

            void close()
            {
                if (fd == kInvalid)
                    return;
#ifdef _WIN32
                closesocket(fd);
#else
                ::close(fd);
#endif
                fd = kInvalid;
            }
        };

        enum : uint8_t
        {
            kCmdHello = 0x00,
            kCmdCompile = 0x01,
            kCmdExecute = 0x02,
            kCmdAotCompile = 0x04,
            kCmdPing = 0x08,
            kCmdGetSnapshot = 0x09,
            kCmdExit = 0xFF,
        };
        static constexpr const char *kProtocolVersion = "RVDL_PROTO_V1";

        struct FrameRequest
        {
            uint8_t cmd;
            std::string payload;
        };

        bool send_all(const Socket &sock, const char *data, size_t size)
        {
            while (size > 0)
            {
                int sent = static_cast<int>(send(sock.fd, data, static_cast<int>(size), 0));
                if (sent <= 0)
                {
                    last_error_ = "send() failed";
                    return false;
                }
                data += sent;
                size -= static_cast<size_t>(sent);
            }
            return true;
        }

        bool send_frame(const Socket &sock, uint8_t cmd, const std::string &payload)
        {
            std::string body;
            body.reserve(payload.size() + 1);
            body.push_back(static_cast<char>(cmd));
            body += payload;
            const uint32_t len = static_cast<uint32_t>(body.size());
            char hdr[4] = {
                static_cast<char>((len >> 24) & 0xFF),
                static_cast<char>((len >> 16) & 0xFF),
                static_cast<char>((len >> 8) & 0xFF),
                static_cast<char>(len & 0xFF)};
            return send_all(sock, hdr, 4) && send_all(sock, body.data(), body.size());
        }

        bool recv_all(const Socket &sock, char *data, size_t size)
        {
            while (size > 0)
            {
                int n = static_cast<int>(recv(sock.fd, data, static_cast<int>(size), 0));
                if (n <= 0)
                    return false;
                data += n;
                size -= static_cast<size_t>(n);
            }
            return true;
        }

        bool recv_frame(const Socket &sock, std::string &out)
        {
            uint8_t hdr[4];
            if (!recv_all(sock, reinterpret_cast<char *>(hdr), 4))
                return false;
            const uint32_t len = (static_cast<uint32_t>(hdr[0]) << 24) |
                                 (static_cast<uint32_t>(hdr[1]) << 16) |
                                 (static_cast<uint32_t>(hdr[2]) << 8) |
                                 static_cast<uint32_t>(hdr[3]);
            if (len > 64u * 1024u * 1024u)
                return false;
            out.resize(len);
            return len == 0 || recv_all(sock, out.data(), len);
        }

        bool transact(const std::vector<FrameRequest> &requests,
                      std::vector<std::string> &responses)
        {
            responses.clear();
            last_error_.clear();

#ifdef _WIN32
            static bool wsa_ready = false;
            if (!wsa_ready)
            {
                WSADATA wsa;
                wsa_ready = (WSAStartup(MAKEWORD(2, 2), &wsa) == 0);
                if (!wsa_ready)
                {
                    last_error_ = "WSAStartup failed";
                    return false;
                }
            }
#endif

            Socket sock;
#ifdef _WIN32
            sock.fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
#else
            sock.fd = socket(AF_INET, SOCK_STREAM, 0);
#endif
            if (sock.fd == Socket::kInvalid)
            {
                last_error_ = "socket() failed";
                return false;
            }

            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_port = htons(static_cast<uint16_t>(port_));
            inet_pton(AF_INET, host_.c_str(), &addr.sin_addr);

#ifdef _WIN32
            if (connect(sock.fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0)
            {
                last_error_ = "daemon unreachable at " + host_ + ":" + std::to_string(port_);
                return false;
            }
            DWORD to = static_cast<DWORD>(timeout_ms_);
            setsockopt(sock.fd, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&to), sizeof(to));
            setsockopt(sock.fd, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char *>(&to), sizeof(to));
#else
            if (connect(sock.fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0)
            {
                last_error_ = "daemon unreachable at " + host_ + ":" + std::to_string(port_);
                return false;
            }
            timeval tv{timeout_ms_ / 1000, (timeout_ms_ % 1000) * 1000};
            setsockopt(sock.fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            setsockopt(sock.fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif

            if (!send_frame(sock, kCmdHello, kProtocolVersion))
                return false;

            for (const auto &req : requests)
                if (!send_frame(sock, req.cmd, req.payload))
                    return false;

            responses.reserve(requests.size());
            for (size_t i = 0; i < requests.size(); ++i)
            {
                std::string resp;
                if (!recv_frame(sock, resp))
                {
                    last_error_ = "recv() failed or timeout";
                    return false;
                }
                responses.push_back(std::move(resp));
            }

            send_frame(sock, kCmdExit, "");
            return true;
        }

        CompileResult send_compile(uint8_t cmd, const std::string &payload)
        {
            CompileResult r;
            if (payload.empty())
            {
                r.status = CompileStatus::EmptyBuffer;
                r.message = "Editor buffer is empty; nothing to compile.";
                return r;
            }

            std::vector<std::string> responses;
            if (!transact({{cmd, payload}}, responses) || responses.empty())
            {
                r.status = CompileStatus::DaemonUnreachable;
                r.message = last_error_.empty() ? "No response from daemon." : last_error_;
                return r;
            }

            const std::string &response = responses[0];
            if (response.find("SUCCESS") != std::string::npos)
            {
                r.status = CompileStatus::Success;
            }
            else
            {
                r.status = CompileStatus::CompileError;
            }
            r.message = response.empty() ? "No response from daemon." : response;
            return r;
        }

        CompileResult embedded_compile(const std::string &payload)
        {
            CompileResult r;
            if (payload.empty())
            {
                r.status = CompileStatus::EmptyBuffer;
                r.message = "Editor buffer is empty; nothing to compile.";
                return r;
            }

            JitResult jr = embedded_jit_compile(payload);
            if (jr.ok)
            {
                r.status = CompileStatus::Success;
            }
            else
            {
                r.status = CompileStatus::CompileError;
            }
            r.message = jr.message;
            return r;
        }

        ExecuteResult embedded_execute(const JitResult &jr)
        {
            ExecuteResult r;
            r.ok = jr.ok;
            r.int_value = jr.ret;
            r.float_value = jr.fret;
            r.message = jr.message;
            r.output = jr.message;
            return r;
        }

    public:
        void set_text(const std::string &src) { buffer_ = src; }
        const std::string &text() const { return buffer_; }
        void clear() { buffer_.clear(); }
        bool empty() const { return buffer_.empty(); }
        size_t size() const { return buffer_.size(); }

        void set_daemon(const std::string &host, int port)
        {
            host_ = host;
            port_ = port;
        }
        void set_timeout(int ms) { timeout_ms_ = ms; }
        void set_backend(Backend b) { backend_ = b; }
        Backend backend() const { return backend_; }
        const std::string &last_error() const { return last_error_; }

        CompileResult compile()
        {
            if (backend_ == Backend::Embedded)
            {
                return embedded_compile(buffer_);
            }
            return send_compile(kCmdCompile, buffer_);
        }

        CompileResult compile_ir(const std::string &ir)
        {
            if (backend_ == Backend::Embedded)
            {
                return embedded_compile(ir);
            }
            return send_compile(kCmdCompile, ir);
        }

        CompileResult aot_compile(const std::string &arch,
                                  const std::string &mode,
                                  const std::string &filename)
        {
            if (backend_ == Backend::Embedded)
            {
                CompileResult r;
                r.status = CompileStatus::CompileError;
                r.message = "AOT compilation is only available with the daemon backend.";
                return r;
            }
            if (buffer_.empty())
            {
                CompileResult r;
                r.status = CompileStatus::EmptyBuffer;
                r.message = "Editor buffer is empty; nothing to compile.";
                return r;
            }
            return send_compile(kCmdAotCompile, arch + " " + mode + " " + filename + "\n" + buffer_);
        }

        ExecuteResult execute_int(const std::string &func = "rvdl_main")
        {
            if (backend_ == Backend::Embedded)
            {
                return embedded_execute(embedded_jit_execute_int(func));
            }

            ExecuteResult r;
            std::vector<std::string> responses;
            if (!transact({{kCmdExecute, "int32 " + func}}, responses) || responses.empty())
            {
                r.message = last_error_.empty() ? "No response from daemon." : last_error_;
                return r;
            }
            const std::string &response = responses[0];
            r.output = response;
            r.ok = response.find("SUCCESS") != std::string::npos;
            r.message = response;

            auto pos = response.find("(Returned: ");
            if (pos != std::string::npos)
            {
                r.int_value = std::stoll(response.substr(pos + 11));
            }
            return r;
        }

        ExecuteResult execute_float(const std::string &func)
        {
            if (backend_ == Backend::Embedded)
            {
                return embedded_execute(embedded_jit_execute_float(func));
            }

            ExecuteResult r;
            std::vector<std::string> responses;
            if (!transact({{kCmdExecute, "float " + func}}, responses) || responses.empty())
            {
                r.message = last_error_.empty() ? "No response from daemon." : last_error_;
                return r;
            }
            const std::string &response = responses[0];
            r.output = response;
            r.ok = response.find("SUCCESS") != std::string::npos;
            r.message = response;

            auto pos = response.find("(Returned: ");
            if (pos != std::string::npos)
            {
                r.float_value = std::stod(response.substr(pos + 11));
            }
            return r;
        }

        ExecuteResult execute_void(const std::string &func)
        {
            if (backend_ == Backend::Embedded)
            {
                return embedded_execute(embedded_jit_execute_void(func));
            }

            ExecuteResult r;
            std::vector<std::string> responses;
            if (!transact({{kCmdExecute, "void " + func}}, responses) || responses.empty())
            {
                r.message = last_error_.empty() ? "No response from daemon." : last_error_;
                return r;
            }
            const std::string &response = responses[0];
            r.output = response;
            r.ok = response.find("SUCCESS") != std::string::npos;
            r.message = response;
            return r;
        }

        ExecuteResult compile_and_run(const std::string &func = "rvdl_main")
        {
            if (backend_ == Backend::Embedded)
            {
                return embedded_execute(embedded_jit_compile_and_run(buffer_, func));
            }
            if (buffer_.empty())
            {
                ExecuteResult r;
                r.message = "Editor buffer is empty; nothing to run.";
                return r;
            }

            ExecuteResult r;
            std::vector<std::string> responses;
            if (!transact({{kCmdCompile, buffer_}, {kCmdExecute, "int32 " + func}}, responses) ||
                responses.size() != 2)
            {
                r.message = last_error_.empty() ? "No response from daemon." : last_error_;
                return r;
            }
            r.output = responses[0] + responses[1];
            r.ok = responses[0].find("SUCCESS") != std::string::npos &&
                   responses[1].find("SUCCESS") != std::string::npos;
            r.message = r.output;

            auto pos = r.output.find("(Returned: ");
            if (pos != std::string::npos)
            {
                r.int_value = std::stoll(r.output.substr(pos + 11));
            }
            return r;
        }

        bool ping()
        {
            if (backend_ == Backend::Embedded)
            {
                return embedded_jit_available();
            }
            std::vector<std::string> responses;
            if (!transact({{kCmdPing, ""}}, responses) || responses.empty())
                return false;
            return responses[0].find("PONG") != std::string::npos;
        }

        std::string snapshot()
        {
            if (backend_ == Backend::Embedded)
            {
                last_error_ = "snapshot is only available with the daemon backend.";
                return "";
            }
            std::vector<std::string> responses;
            if (!transact({{kCmdGetSnapshot, ""}}, responses) || responses.empty())
            {
                last_error_ = last_error_.empty() ? "snapshot failed" : last_error_;
                return "";
            }
            return responses[0];
        }
    };
}