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

    // ─── 编译 / 执行结果 ────────────────────────────────────────────
    enum class CompileStatus
    {
        Success,
        EmptyBuffer,
        DaemonUnreachable,
        CompileError,
    };

    // ─── 编译后端选择 ───────────────────────────────────────────────
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

    // 超轻量 rvdl 脚本编辑器（内置）—— 支持两种编译后端：
    //   1) Daemon   连接 ravendrel 守护进程（thin client）
    //   2) Embedded 进程内复用 runtime 的 JIT（qhal::JIT + QVM）
    class QkEditor
    {
    private:
        std::string buffer_;             // 当前持有的 rvdl 源码 / LLVM IR 文本
        std::string host_ = "127.0.0.1"; // daemon 地址
        // daemon 端口：单一来源是 server/src/protocol.ts 的 DAEMON_PORT
        // （TS 侧全部 import 它）；C++ 侧保留本地值，改端口时以那里为准。
        int port_ = 50052;
        int timeout_ms_ = 8000;          // 收发超时
        std::string last_error_;

        Backend backend_ = Backend::Daemon; // 当前编译后端

        // ── RAII socket 封装 ────────────────────────────────────────
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

        // ── 二进制帧协议（与 runtime daemon 的帧格式一致）────────────
        //   请求帧：[4 字节大端长度][命令字节 + payload]
        //   响应帧：[4 字节大端长度][文本 payload]（无命令字节）
        // 旧实现发送「文本行 + END_COMPILE 哨兵」并用 EXIT\n 结束——daemon 早已
        // 改为长度前缀二进制帧（见 runtime/src/main.cpp process_frame），旧文本
        // 只会被当成垃圾长度解析，编辑器 daemon 后端因此静默失效；此处对齐协议。
        enum : uint8_t
        {
            kCmdHello = 0x00,       // payload = "RVDL_PROTO_V1"
            kCmdCompile = 0x01,     // payload = LLVM IR
            kCmdExecute = 0x02,     // payload = "return_type func_name"
            kCmdAotCompile = 0x04,  // payload = "arch mode name\nIR"
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
            if (len > 64u * 1024u * 1024u) // 与 daemon 侧一致的单帧上限（64MB）
                return false;
            out.resize(len);
            return len == 0 || recv_all(sock, out.data(), len);
        }

        // ── 一次「请求 + 响应」事务──
        // 发送 requests 中的每个请求帧，再依序读取同数量的响应帧。
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

            // 握手帧（daemon 对 HELLO 不回响应，仅做版本协商）。
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

            // 礼貌关闭（daemon 收到 EXIT 即关闭连接；失败不影响本次事务结果）。
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

        // 内嵌后端编译（JitResult -> CompileResult）
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

        // 内嵌后端执行（JitResult -> ExecuteResult）
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
        // ── 缓冲区管理 ───────────────────────────────────────────────
        void set_text(const std::string &src) { buffer_ = src; }
        const std::string &text() const { return buffer_; }
        void clear() { buffer_.clear(); }
        bool empty() const { return buffer_.empty(); }
        size_t size() const { return buffer_.size(); }

        // ── 连接 / 后端配置 ──────────────────────────────────────────
        void set_daemon(const std::string &host, int port)
        {
            host_ = host;
            port_ = port;
        }
        void set_timeout(int ms) { timeout_ms_ = ms; }
        void set_backend(Backend b) { backend_ = b; }
        Backend backend() const { return backend_; }
        const std::string &last_error() const { return last_error_; }

        // ── 编译 ─────────────────────────────────────────────────────
        // 复用 runtime 的 JIT：按所选后端，把 buffer_（LLVM IR）交给
        // daemon 或内嵌 JIT 编译。
        // rvdl 源码 -> IR 的前端目前由 TS 语言服务器(server/src/ir.ts)完成，
        //       C++ 侧此处直接消费 IR。
        CompileResult compile()
        {
            if (backend_ == Backend::Embedded)
            {
                return embedded_compile(buffer_);
            }
            return send_compile(kCmdCompile, buffer_);
        }

        // 显式编译一段给定的 IR（不依赖 buffer_）。
        CompileResult compile_ir(const std::string &ir)
        {
            if (backend_ == Backend::Embedded)
            {
                return embedded_compile(ir);
            }
            return send_compile(kCmdCompile, ir);
        }

        // AOT 编译：AOT_COMPILE <arch> <mode> <filename>（仅 daemon 后端支持）
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
            // 二进制帧 payload："arch mode filename\nIR"（与 daemon 端解析一致）。
            return send_compile(kCmdAotCompile, arch + " " + mode + " " + filename + "\n" + buffer_);
        }

        // ── 执行 ─────────────────────────────────────────────────────
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

        // 编译 + 执行一步完成。
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
            // 两个响应依次为编译、执行结果（与旧文本协议的拼接输出等价）。
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

        // ── 响应检查 / 状态 ──────────────────────────────────────────
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