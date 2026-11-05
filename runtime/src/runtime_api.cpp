#include "../include/qhal/RuntimeApi.h"

#include "../include/qhal/QM.hpp"
#include "../include/qhal/QVM.hpp"
#include "../include/qhal/VisualizationService.hpp"
#include "../include/qhal/JIT.hpp"
#include "../include/qhal/Compiler.hpp"
#include "../include/qhal/MMI.hpp"
#include "../include/qhal/MirModuleBuilder.hpp"
#include "../include/qml/Inference.hpp"
#include "../include/qml/QrcAbi.hpp"
#include "../include/qml/TQNFAbi.hpp"
#include "../include/qml/QStateAbi.hpp"
#include "../include/qml/QMathAbi.hpp"
#include "../include/qml/QAttentionAbi.hpp"
#include "../include/qml/QEntropyAbi.hpp"
#include "../include/qml/QGateAbi.hpp"
#include "../include/qml/RetrocausalAbi.hpp"
#include "../include/qml/RetrocausalCTCAbi.hpp"
#include "../include/qml/GeodesicAbi.hpp"
#include "../include/qml/SoftLogicAbi.hpp"
#include "../include/gui/protocol.hpp"
#include "../include/verify/IntervalAbstract.hpp"
#include "../include/utils/Ta.hpp"
#include "../include/utils/ShuttleRuntime.hpp"

#include "llvm/Support/SourceMgr.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/raw_ostream.h"

#include <mutex>
#include <string>
#include <memory>
#include <iostream>
#include <fstream>
#include <sstream>
#include <cstring>
#include <cstdlib>
#include <thread>
#include <map>
#include <vector>
#include <set>
#include <functional>
#include <atomic>
#include <cmath>
#include <algorithm>
#include <type_traits>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/socket.h>
#include <sys/select.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>
#endif

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

qhal::IQuantumBackend *global_qm = nullptr;
std::string global_string_buffer;

// 活跃 runtime（供 IR 生成的 rvdl_topology_entry 经 rvdl_runtime_run_topology 访问 JIT）
static rvdl_runtime *g_active_runtime = nullptr;

// ─── 可选 GPU：dlopen 延迟加载 CUDA（不硬链 libcuda/libcudart）──────────
// Kokkos 以 CUDA 后端编译但不链接 CUDA 库，运行时据此决定走 CUDA 还是 CPU。
namespace rvdl {
    static std::atomic<bool> s_gpu_backend_available{false};
    bool gpu_backend_available() noexcept { return s_gpu_backend_available.load(); }
    void set_gpu_backend_available(bool available) noexcept { s_gpu_backend_available.store(available); }
}

namespace
{
    // 探测 NVIDIA GPU 是否可用：
    //   1. 加载 CUDA runtime（RTLD_GLOBAL）以解析 libkokkoscore.so 里未解析的
    //      cudaMalloc / cudaMemcpy 等运行时符号；
    //   2. 加载 CUDA driver（libcuda.so.1，RTLD_GLOBAL）以解析 cuInit 等驱动符号；
    //   3. 用 driver API 的 cuInit + cuDeviceGetCount 确认存在可用设备。
    // 任一步失败即视为无 GPU，运行时回退 CPU（OpenMP/Serial）。
    bool detect_gpu_available()
    {
        // 环境变量 RVDL_NO_GPU=1/true/yes 强制禁用 GPU 探测（诊断 / 无 GPU 环境 / 规避驱动问题）。
        // RVDL_NO_GPU=0/false/no 或未设置时正常探测。
        const char *nogpu = std::getenv("RVDL_NO_GPU");
        if (nogpu && nogpu[0] != '\0' &&
            std::strcmp(nogpu, "0") != 0 && std::strcmp(nogpu, "false") != 0 &&
            std::strcmp(nogpu, "no") != 0 && std::strcmp(nogpu, "off") != 0)
            return false;
#ifdef _WIN32
        for (const char *n : {"cudart64_13.dll", "cudart64_12.dll", "cudart64_110.dll", "cudart64_100.dll"})
            LoadLibraryA(n);
        HMODULE cuda = LoadLibraryA("nvcuda.dll");
        if (!cuda)
            return false;
        using cuInitFn = int (*)(unsigned int);
        using cuDeviceGetCountFn = int (*)(int *);
        auto cuInit = reinterpret_cast<cuInitFn>(GetProcAddress(cuda, "cuInit"));
        auto cuGetCount = reinterpret_cast<cuDeviceGetCountFn>(GetProcAddress(cuda, "cuDeviceGetCount"));
        if (!cuInit || !cuGetCount)
            return false;
        if (cuInit(0) != 0)
            return false;
        int count = 0;
        if (cuGetCount(&count) != 0)
            return false;
        return count > 0;
#else
        for (const char *n : {"libcudart.so", "libcudart.so.13", "libcudart.so.12", "libcudart.so.11.0"})
            dlopen(n, RTLD_NOW | RTLD_GLOBAL);
        void *cuda = dlopen("libcuda.so.1", RTLD_NOW | RTLD_GLOBAL);
        if (!cuda)
            return false;
        using cuInitFn = int (*)(unsigned int);
        using cuDeviceGetCountFn = int (*)(int *);
        auto cuInit = reinterpret_cast<cuInitFn>(dlsym(cuda, "cuInit"));
        auto cuGetCount = reinterpret_cast<cuDeviceGetCountFn>(dlsym(cuda, "cuDeviceGetCount"));
        if (!cuInit || !cuGetCount)
            return false;
        if (cuInit(0) != 0)
            return false;
        int count = 0;
        if (cuGetCount(&count) != 0)
            return false;
        return count > 0;
#endif
    }

    bool is_fpga_host_available(const std::string &ip, int port)
    {
#ifdef _WIN32
        WSADATA wsaData;
        if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0)
            return false;
        SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (sock == INVALID_SOCKET)
        {
            WSACleanup();
            return false;
        }
#else
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0)
            return false;
#endif

        sockaddr_in serverAddress{};
        serverAddress.sin_family = AF_INET;
        serverAddress.sin_port = htons(static_cast<u_short>(port));
        inet_pton(AF_INET, ip.c_str(), &serverAddress.sin_addr);
        bool connected = false;
#ifdef _WIN32
        u_long nonblock = 1;
        ioctlsocket(sock, FIONBIO, &nonblock);
        int result = connect(sock, (struct sockaddr *)&serverAddress, sizeof(serverAddress));
        if (result == 0)
        {
            connected = true;
        }
        else if (WSAGetLastError() == WSAEWOULDBLOCK)
        {
            fd_set wfds, efds;
            FD_ZERO(&wfds);
            FD_ZERO(&efds);
            FD_SET(sock, &wfds);
            FD_SET(sock, &efds);
            timeval tv{};
            tv.tv_sec = 0;
            tv.tv_usec = 200000;
            int sel = select(0, nullptr, &wfds, &efds, &tv);
            if (sel > 0 && FD_ISSET(sock, &wfds) && !FD_ISSET(sock, &efds))
                connected = true;
        }
        nonblock = 0;
        ioctlsocket(sock, FIONBIO, &nonblock);
#else
        int flags = fcntl(sock, F_GETFL, 0);
        fcntl(sock, F_SETFL, flags | O_NONBLOCK);
        int result = connect(sock, (struct sockaddr *)&serverAddress, sizeof(serverAddress));
        if (result == 0)
        {
            connected = true;
        }
        else if (errno == EINPROGRESS)
        {
            fd_set wfds, efds;
            FD_ZERO(&wfds);
            FD_ZERO(&efds);
            FD_SET(sock, &wfds);
            FD_SET(sock, &efds);
            timeval tv{};
            tv.tv_sec = 0;
            tv.tv_usec = 200000;
            int sel = select(sock + 1, nullptr, &wfds, &efds, &tv);
            if (sel > 0 && FD_ISSET(sock, &wfds) && !FD_ISSET(sock, &efds))
                connected = true;
        }
        fcntl(sock, F_SETFL, flags);
#endif

        bool is_valid = false;
        if (connected)
        {
            char ping = static_cast<char>(0x99);
            send(sock, &ping, 1, 0);
            char pong = 0x00;
            int bytes_received = recv(sock, &pong, 1, 0);

            if (bytes_received > 0 && pong == static_cast<char>(0xAA))
            {
                is_valid = true;
            }
        }

#ifdef _WIN32
        closesocket(sock);
        WSACleanup();
#else
        close(sock);
#endif
        return is_valid;
    }

    std::string &result_buffer()
    {
        thread_local std::string buf;
        return buf;
    }

}

struct rvdl_runtime
{
    std::unique_ptr<qhal::IQuantumBackend> backend;
    std::unique_ptr<qhal::JIT> jit;
    std::unique_ptr<qhal::VisualizationService> viz;
    // 已绑定的 .rvdli 模块（保持生命周期，避免导出符号地址悬垂）
    std::vector<std::shared_ptr<qhal::MMIModule>> bound_mmis;
    std::mutex mutex;
};

rvdl_runtime *rvdl_runtime_create(void)
{
    try
    {
        // 可选 GPU：启动时 dlopen 探测 NVIDIA 驱动，决定走 CUDA 还是 CPU 后端。
        const bool gpu = detect_gpu_available();
        rvdl::set_gpu_backend_available(gpu);
        if (gpu)
            std::cout << "[Ravendrel JIT] CUDA GPU detected. Using Kokkos CUDA backend." << std::endl;
        else
            std::cout << "[Ravendrel JIT] No CUDA GPU. Falling back to CPU (OpenMP/Serial) backend." << std::endl;

        // Kokkos 必须在使用任何 View / parallel_for 之前初始化（GPU 后端需要）。
        // 无 GPU 时跳过默认（CUDA）执行空间初始化，CPU 路径走显式 OpenMP 空间（惰性初始化）。
        if (gpu && !Kokkos::is_initialized())
            Kokkos::initialize();

        auto *rt = new rvdl_runtime();

        const char *backend_env = std::getenv("RVDL_BACKEND");
        const char *endpoint_env = std::getenv("RVDL_QPU_ENDPOINT");
        const char *node_env = std::getenv("RVDL_QPU_NODE");

        if (backend_env && std::string(backend_env) == "qvm")
        {
            std::cout << "[Ravendrel JIT] Backend forced to local QVM." << std::endl;
            rt->backend = std::make_unique<qhal::QVM>();
        }
        else if (backend_env && std::string(backend_env).rfind("qm:", 0) == 0)
        {
            std::string m = std::string(backend_env).substr(3);
            qhal::HardwareModality modality = qhal::HardwareModality::Superconducting;
            if (m == "ion" || m == "trappedion") modality = qhal::HardwareModality::TrappedIon;
            else if (m == "atom" || m == "neutralatom") modality = qhal::HardwareModality::NeutralAtom;
            else if (m == "photon" || m == "photonic") modality = qhal::HardwareModality::Photonic;
            size_t node = node_env ? static_cast<size_t>(std::strtoul(node_env, nullptr, 10)) : 0;
            std::cout << "[Ravendrel JIT] Backend forced to QM (" << m << ", node " << node << ")." << std::endl;
            rt->backend = std::make_unique<qhal::QM>(modality, node);
        }
        else
        {
            // 默认端点是**回环**而非某台特定机器的私网 IP：曾经的默认值
            // 192.168.1.100:50051 是作者局域网的地址，对其它环境毫无意义，
            // 还会无谓探测别人的局域网。没有 RVDL_QPU_ENDPOINT 就没有真实
            // 硬件 → 探测失败 → 回落本地 QVM（下方已有诚实日志）。
            std::string endpoint = endpoint_env ? std::string(endpoint_env) : std::string("127.0.0.1:50051");
            std::string ip = endpoint;
            int port = 50051;
            auto colon = endpoint.rfind(':');
            if (colon != std::string::npos)
            {
                ip = endpoint.substr(0, colon);
                port = std::atoi(endpoint.substr(colon + 1).c_str());
            }
            if (is_fpga_host_available(ip, port))
            {
                std::cout << "[Ravendrel JIT] Hardware detected! Starting quantum machine, SuperconductingBackend." << std::endl;
                rt->backend = std::make_unique<qhal::QM>(qhal::HardwareModality::Superconducting, 0);
            }
            else
            {
                std::cout << "[Ravendrel JIT] Hardware offline. Falling back to local QVM." << std::endl;
                rt->backend = std::make_unique<qhal::QVM>();
            }
        }

        global_qm = rt->backend.get();
        rt->jit = std::make_unique<qhal::JIT>(global_qm);
        rt->viz = std::make_unique<qhal::VisualizationService>(rt->backend.get());
        g_active_runtime = rt;

        return rt;
    }
    catch (const std::exception &e)
    {
        std::cerr << "Fatal JIT Initialization Error: " << e.what() << '\n';
        return nullptr;
    }
}

void rvdl_runtime_destroy(rvdl_runtime *rt)
{
    if (!rt)
        return;
    {
        std::lock_guard<std::mutex> lock(rt->mutex);
        rt->viz->stop();
        rt->jit.reset();
        rt->backend.reset();
        global_qm = nullptr;
        g_active_runtime = nullptr;
    }
    delete rt;
    if (Kokkos::is_initialized())
        Kokkos::finalize();
}

void rvdl_runtime_viz_start(rvdl_runtime *rt)
{
    if (rt)
        rt->viz->start();
}

void rvdl_runtime_viz_stop(rvdl_runtime *rt)
{
    if (rt)
        rt->viz->stop();
}

const char *rvdl_runtime_compile(rvdl_runtime *rt, const char *ir)
{
    std::string &out = result_buffer();
    out.clear();

    if (!rt || !ir)
    {
        out = "RESPONSE: ERROR - Invalid Runtime Instance\n";
        return out.c_str();
    }

    std::lock_guard<std::mutex> lock(rt->mutex);

    llvm::SMDiagnostic error;
    auto context = std::make_unique<llvm::LLVMContext>();
    auto mem_buffer = llvm::MemoryBuffer::getMemBuffer(ir);
    auto module = llvm::parseIR(*mem_buffer, error, *context);

    if (!module)
    {
        std::string err_str;
        llvm::raw_string_ostream os(err_str);
        error.print("RavendrelJIT", os);
        out = "RESPONSE: ERROR - Invalid IR Payload\n";
        out += os.str();
    }
    else
    {
        rt->jit->add_ir_module(std::move(module), std::move(context));
        out = "RESPONSE: SUCCESS - Module Compiled\n";
    }

    return out.c_str();
}

const char *rvdl_runtime_compile_mir(rvdl_runtime *rt, const char *mir_json)
{
    std::string &out = result_buffer();
    out.clear();

    if (!rt || !mir_json)
    {
        out = "RESPONSE: ERROR - Invalid Runtime Instance\n";
        return out.c_str();
    }

    std::lock_guard<std::mutex> lock(rt->mutex);

    try
    {
        qhal::MirModuleBuilder builder;
        qhal::BuiltMirModule built = builder.build(mir_json);
        rt->jit->add_ir_module(std::move(built.module), std::move(built.context));
        out = "RESPONSE: SUCCESS - MIR Module Compiled\n";
    }
    catch (const std::exception &e)
    {
        out = "RESPONSE: ERROR - MIR compile failed: ";
        out += e.what();
        out += "\n";
    }

    return out.c_str();
}

const char *rvdl_runtime_execute_int(rvdl_runtime *rt, const char *func_name)
{
    std::string &out = result_buffer();
    out.clear();

    if (!rt || !func_name)
    {
        out = "RESPONSE: ERROR - Invalid Runtime Instance\n";
        return out.c_str();
    }

    std::lock_guard<std::mutex> lock(rt->mutex);

    auto func = rt->jit->get_function<int()>(func_name);
    if (func)
    {
        int ret = func();
        if (!global_string_buffer.empty())
        {
            out = global_string_buffer;
            global_string_buffer.clear();
        }
        else
        {
            out = "RESPONSE: SUCCESS - Executed " + std::string(func_name) + " (Returned: " + std::to_string(ret) + ")\n";
        }
    }
    else
    {
        out = "RESPONSE: ERROR - Function " + std::string(func_name) + " not found in JIT\n";
    }

    return out.c_str();
}

const char *rvdl_runtime_execute_float(rvdl_runtime *rt, const char *func_name)
{
    std::string &out = result_buffer();
    out.clear();

    if (!rt || !func_name)
    {
        out = "RESPONSE: ERROR - Invalid Runtime Instance\n";
        return out.c_str();
    }

    std::lock_guard<std::mutex> lock(rt->mutex);

    auto func = rt->jit->get_function<float()>(func_name);
    if (func)
    {
        out = "RESPONSE: SUCCESS - Executed " + std::string(func_name) + " (Returned: " + std::to_string(func()) + ")\n";
    }
    else
    {
        out = "RESPONSE: ERROR - Function " + std::string(func_name) + " not found in JIT\n";
    }

    return out.c_str();
}

const char *rvdl_runtime_execute_void(rvdl_runtime *rt, const char *func_name)
{
    std::string &out = result_buffer();
    out.clear();

    if (!rt || !func_name)
    {
        out = "RESPONSE: ERROR - Invalid Runtime Instance\n";
        return out.c_str();
    }

    std::lock_guard<std::mutex> lock(rt->mutex);

    auto func = rt->jit->get_function<void()>(func_name);
    if (func)
    {
        func();
        out = "RESPONSE: SUCCESS - Executed " + std::string(func_name) + "\n";
    }
    else
    {
        out = "RESPONSE: ERROR - Function " + std::string(func_name) + " not found in JIT\n";
    }

    return out.c_str();
}

// AOT 编译实现：overrides 为来自请求头行的工具链覆盖项（优先于环境变量）。
static const char *aot_compile_impl(rvdl_runtime *rt,
                                    const char *arch,
                                    const char *mode,
                                    const char *output_name,
                                    const char *ir,
                                    const rvdl::AotToolchainOverrides &overrides)
{
    std::string &out = result_buffer();
    out.clear();

    if (!rt || !arch || !mode || !output_name || !ir)
    {
        out = "RESPONSE: ERROR - Invalid Runtime Instance\n";
        return out.c_str();
    }

    std::lock_guard<std::mutex> lock(rt->mutex);

    llvm::SMDiagnostic error;
    auto context = std::make_unique<llvm::LLVMContext>();
    auto mem_buffer = llvm::MemoryBuffer::getMemBuffer(ir);
    auto module = llvm::parseIR(*mem_buffer, error, *context);

    if (!module)
    {
        std::string err_str;
        llvm::raw_string_ostream os(err_str);
        error.print("Ravendrel AOT", os);
        out = "RESPONSE: ERROR - Invalid IR Payload\n";
        out += os.str();
    }
    else
    {
        bool success = rvdl::AOTCompiler::compile_to_binary(module.get(), arch, mode, output_name, overrides);
        if (success)
        {
            out = "RESPONSE: SUCCESS - AOT Compilation Finished (" + std::string(output_name) + ")\n";
        }
        else
        {
            out = "RESPONSE: ERROR - AOT Compilation Failed\n";
        }
    }

    return out.c_str();
}

const char *rvdl_runtime_aot_compile(rvdl_runtime *rt,
                                      const char *arch,
                                      const char *mode,
                                      const char *output_name,
                                      const char *ir)
{
    return aot_compile_impl(rt, arch, mode, output_name, ir, rvdl::AotToolchainOverrides{});
}

const char *rvdl_runtime_aot_compile_ex(rvdl_runtime *rt,
                                         const char *arch,
                                         const char *mode,
                                         const char *output_name,
                                         const char *ir,
                                         const char *clangxx,
                                         const char *sysroot,
                                         const char *rt_dir,
                                         const char *extra_libs)
{
    rvdl::AotToolchainOverrides overrides;
    if (clangxx)
        overrides.clangxx = clangxx;
    if (sysroot)
        overrides.sysroot = sysroot;
    if (rt_dir)
        overrides.rt_dir = rt_dir;
    if (extra_libs)
        overrides.extra_libs = extra_libs;
    return aot_compile_impl(rt, arch, mode, output_name, ir, overrides);
}

const char *rvdl_runtime_snapshot(rvdl_runtime *rt)
{
    std::string &out = result_buffer();
    out.clear();

    if (!rt)
    {
        out = "RESPONSE: ERROR - Invalid Runtime Instance\n";
        return out.c_str();
    }

    out = qgui::serialize(rt->viz->snapshot());
    return out.c_str();
}

const char *rvdl_runtime_verify(rvdl_runtime *rt, const char *vc_protocol)
{
    std::string &out = result_buffer();
    out.clear();

    if (!rt || !vc_protocol)
    {
        out = "VERIFY_ERROR invalid arguments\n";
        return out.c_str();
    }

    std::lock_guard<std::mutex> lock(rt->mutex);
    out = qhal::verify::verifyProtocolCombined(std::string(vc_protocol));
    return out.c_str();
}

// ─── QChain 量子区块链服务 ABI ───────────────────────────────────────────
// 内部复用 qchain_bridge 的单例 QChainService（绑定 global_qm 后端）。

const char *rvdl_runtime_qchain_wallet(rvdl_runtime *rt)
{
    std::string &out = result_buffer();
    out.clear();
    if (!rt)
    {
        out = "RESPONSE: ERROR - Invalid Runtime Instance\n";
        return out.c_str();
    }
    std::lock_guard<std::mutex> lock(rt->mutex);
    auto &w = qchain_bridge::service().create_wallet();
    out = qchain::to_hex(w.address);
    return out.c_str();
}

const char *rvdl_runtime_qchain_mint(rvdl_runtime *rt, const char *addr, uint64_t amount)
{
    std::string &out = result_buffer();
    out.clear();
    if (!rt || !addr)
    {
        out = "RESPONSE: ERROR - Invalid arguments\n";
        return out.c_str();
    }
    std::lock_guard<std::mutex> lock(rt->mutex);
    qchain_bridge::service().mint_to_address(qchain_bridge::parse_address(addr), amount);
    out = "RESPONSE: SUCCESS - Minted " + std::to_string(amount) + " to " + std::string(addr) + "\n";
    return out.c_str();
}

const char *rvdl_runtime_qchain_transfer(rvdl_runtime *rt, const char *from, const char *to, uint64_t amount)
{
    std::string &out = result_buffer();
    out.clear();
    if (!rt || !from || !to)
    {
        out = "RESPONSE: ERROR - Invalid arguments\n";
        return out.c_str();
    }
    std::lock_guard<std::mutex> lock(rt->mutex);
    bool ok = qchain_bridge::service().transfer_addresses(
        qchain_bridge::parse_address(from), qchain_bridge::parse_address(to), amount);
    out = ok ? "RESPONSE: SUCCESS - Transferred\n"
             : "RESPONSE: ERROR - Transfer failed (insufficient balance)\n";
    return out.c_str();
}

const char *rvdl_runtime_qchain_balance(rvdl_runtime *rt, const char *addr)
{
    std::string &out = result_buffer();
    out.clear();
    if (!rt || !addr)
    {
        out = "RESPONSE: ERROR - Invalid arguments\n";
        return out.c_str();
    }
    std::lock_guard<std::mutex> lock(rt->mutex);
    uint64_t bal = qchain_bridge::service().balance_of_address(qchain_bridge::parse_address(addr));
    out = std::to_string(bal);
    return out.c_str();
}

const char *rvdl_runtime_qchain_mine(rvdl_runtime *rt)
{
    std::string &out = result_buffer();
    out.clear();
    if (!rt)
    {
        out = "RESPONSE: ERROR - Invalid Runtime Instance\n";
        return out.c_str();
    }
    std::lock_guard<std::mutex> lock(rt->mutex);
    // 出块失败时返回错误（旧实现无论成败都返回高度，调用方无法察觉失败）。
    if (!qchain_bridge::service().mine_block())
    {
        out = "RESPONSE: ERROR - Mining failed (PoW / invalid mempool tx / append rejected)\n";
        return out.c_str();
    }
    out = std::to_string(qchain_bridge::service().get_chain().height());
    return out.c_str();
}

const char *rvdl_runtime_qchain_height(rvdl_runtime *rt)
{
    std::string &out = result_buffer();
    out.clear();
    if (!rt)
    {
        out = "RESPONSE: ERROR - Invalid Runtime Instance\n";
        return out.c_str();
    }
    std::lock_guard<std::mutex> lock(rt->mutex);
    out = std::to_string(qchain_bridge::service().get_chain().height());
    return out.c_str();
}

const char *rvdl_runtime_qchain_verify(rvdl_runtime *rt)
{
    std::string &out = result_buffer();
    out.clear();
    if (!rt)
    {
        out = "RESPONSE: ERROR - Invalid Runtime Instance\n";
        return out.c_str();
    }
    std::lock_guard<std::mutex> lock(rt->mutex);
    out = qchain_bridge::service().verify_chain()
              ? "RESPONSE: SUCCESS - Chain valid\n"
              : "RESPONSE: ERROR - Chain invalid\n";
    return out.c_str();
}

const char *rvdl_runtime_qchain_qkd(rvdl_runtime *rt, int32_t rounds)
{
    std::string &out = result_buffer();
    out.clear();
    if (!rt)
    {
        out = "RESPONSE: ERROR - Invalid Runtime Instance\n";
        return out.c_str();
    }
    std::lock_guard<std::mutex> lock(rt->mutex);
    auto res = qchain_bridge::service().establish_qkd(static_cast<size_t>(rounds > 0 ? rounds : 64));
    out = qchain::to_hex(res.shared_key);
    return out.c_str();
}

const char *rvdl_runtime_qchain_qdba(rvdl_runtime *rt, int32_t parties)
{
    std::string &out = result_buffer();
    out.clear();
    if (!rt)
    {
        out = "RESPONSE: ERROR - Invalid Runtime Instance\n";
        return out.c_str();
    }
    std::lock_guard<std::mutex> lock(rt->mutex);
    auto res = qchain_bridge::service().run_qdba(static_cast<size_t>(parties), {}, 20);
    out = res.agreement ? "1" : "0";
    return out.c_str();
}

struct rvdl_mmi
{
    std::shared_ptr<qhal::MMIModule> module;
};

const char *rvdl_runtime_export_mmi(rvdl_runtime *rt,
                                     const char *header_json,
                                     const char *ir,
                                     const char *output_path)
{
    std::string &out = result_buffer();
    out.clear();

    if (!rt || !header_json || !ir || !output_path)
    {
        out = "RESPONSE: ERROR - Invalid arguments\n";
        return out.c_str();
    }

    std::lock_guard<std::mutex> lock(rt->mutex);

    // QOBF v2：二进制 header + ChaCha20 流加密 + HMAC 完整性（打开为乱码）。
    std::string module_name;
    qhal::qcrypt::Bytes binary_payload = qhal::mmi_header_to_binary(header_json, module_name);
    const char *ir_data = ir;
    binary_payload.insert(binary_payload.end(), ir_data, ir_data + std::strlen(ir_data));
    qhal::qcrypt::Bytes data = qhal::qcrypt::pack_mmi_v2(module_name, binary_payload);

    std::ofstream ofs(output_path, std::ios::binary);
    if (!ofs)
    {
        out = "RESPONSE: ERROR - Cannot open output file\n";
        return out.c_str();
    }
    ofs.write((const char *)data.data(), (std::streamsize)data.size());
    ofs.close();

    out = "RESPONSE: SUCCESS - Exported encrypted MMI (" + std::string(output_path) + ")\n";
    return out.c_str();
}

namespace
{
    std::string dir_of(const std::string &p)
    {
        size_t pos = p.find_last_of("/\\");
        if (pos == std::string::npos)
            return ".";
        if (pos == 0)
            return "/";
        return p.substr(0, pos);
    }
}

rvdl_mmi *rvdl_runtime_load_mmi(rvdl_runtime *rt, const char *path)
{
    if (!rt || !path)
        return nullptr;

    std::lock_guard<std::mutex> lock(rt->mutex);
    auto cache = std::make_shared<std::map<std::string, std::shared_ptr<qhal::MMIModule>>>();

    std::function<std::shared_ptr<qhal::MMIModule>(const std::string &, const std::string &)> load =
        [&](const std::string &p, const std::string &parent_dir) -> std::shared_ptr<qhal::MMIModule>
    {
        bool is_abs = (!p.empty() && (p[0] == '/' || p[0] == '\\')) ||
                      (p.size() > 1 && p[1] == ':');
        std::string resolved = is_abs ? p : (parent_dir + "/" + p);
        auto hit = cache->find(resolved);
        if (hit != cache->end())
            return hit->second;

        std::ifstream ifs(resolved, std::ios::binary);
        if (!ifs)
            return nullptr;
        std::stringstream ss;
        ss << ifs.rdbuf();

        std::string self_dir = dir_of(resolved);
        auto mod = std::make_shared<qhal::MMIModule>(ss.str(), self_dir, load);
        (*cache)[resolved] = mod;
        return mod;
    };

    try
    {
        // path 可为相对/绝对路径；相对路径以当前工作目录为基准（避免目录重复拼接）
        auto mod = load(path, ".");
        if (!mod)
            return nullptr;

        auto *m = new rvdl_mmi();
        m->module = mod;
        return m;
    }
    catch (const std::exception &e)
    {
        std::cerr << "[MMI] load failed: " << e.what() << "\n";
        return nullptr;
    }
}

int32_t rvdl_runtime_load_native(rvdl_runtime *rt, const char *path)
{
    (void)rt;
    if (!path)
        return 0;
#ifdef _WIN32
    HMODULE h = LoadLibraryA(path);
    return h ? 1 : 0;
#else
    void *h = dlopen(path, RTLD_NOW | RTLD_GLOBAL);
    return h ? 1 : 0;
#endif
}

void rvdl_runtime_register_native_symbol(rvdl_runtime *rt, const char *name, void *addr)
{
    (void)rt;
    if (name && addr)
        qhal::register_native_symbol(name, addr);
}

const char *rvdl_runtime_bind_mmi(rvdl_runtime *rt, const char *alias, const char *path)
{
    std::string &out = result_buffer();
    out.clear();
    if (!rt || !alias || !path)
    {
        out = "RESPONSE: ERROR - Invalid arguments\n";
        return out.c_str();
    }
    try
    {
        std::ifstream ifs(path, std::ios::binary);
        if (!ifs)
        {
            out = "RESPONSE: ERROR - bind_mmi: cannot open file\n";
            return out.c_str();
        }
        std::stringstream ss;
        ss << ifs.rdbuf();

        std::lock_guard<std::mutex> lock(rt->mutex);
        auto mod = std::make_shared<qhal::MMIModule>(ss.str(), dir_of(path), nullptr);
        rt->bound_mmis.push_back(mod); // 保持生命周期，避免导出地址悬垂
        int bound = 0;
        std::string detail;
        detail += "exports=" + std::to_string(mod->exports().size());
        for (const auto &ex : mod->exports())
        {
            void *addr = mod->lookup_export_address(ex.name);
            detail += std::string(" ") + ex.name + (addr ? ":ok" : ":MISS");
            if (addr)
            {
                rt->jit->bind_symbol(std::string(alias) + "_" + ex.name, addr);
                bound++;
            }
        }
        out = "RESPONSE: MMI_BOUND " + std::to_string(bound) + " (" + detail + ")\n";
    }
    catch (const std::exception &e)
    {
        out = "RESPONSE: ERROR - bind_mmi failed: ";
        out += e.what();
        out += "\n";
    }
    return out.c_str();
}

const char *rvdl_runtime_mmi_invoke(rvdl_mmi *m,
                                     const char *func_name,
                                     const char *args_json)
{
    std::string &out = result_buffer();
    out.clear();

    if (!m || !func_name || !args_json)
    {
        out = "RESPONSE: ERROR - Invalid arguments\n";
        return out.c_str();
    }

    try
    {
        out = m->module->invoke(func_name, args_json);
    }
    catch (const std::exception &e)
    {
        out = "RESPONSE: ERROR - ";
        out += e.what();
        out += "\n";
    }
    return out.c_str();
}

void rvdl_runtime_mmi_unload(rvdl_mmi *m)
{
    delete m;
}

// ============================================================================
// 多维标签函数执行拓扑（@layer）调度器
//
// 解析调度表 JSON，通过活跃 runtime 的 JIT 查找各块符号，做「更细粒度」的
// 拓扑线程调度（ta::TopologyAnalyzer）：
//   1. 按 coord 分组 → 虚拟拓扑线程（每条虚拟线程 = 一个逻辑 qubit 站点）；
//   2. 按 time 分层 → 层间串行、层内并行（异 coord 空间并置）；
//   3. 按码距做逻辑 qubit 布局 → 同层内用 coord 的 L1 码距把「过近」的虚拟
//      线程串行到不同批次（码距阈值 RVDL_TOPOLOGY_MIN_DISTANCE 可调），
//      码距足够大的虚拟线程才并行，避免量子串扰。
//
// 块内调用传播延迟（子函数 = 父时钟 + Δt）已由编译期 TopologyBuilder 静态
// 校验，且函数体内部的调用顺序由 CPU 顺序执行自然保证。
// ============================================================================
int32_t rvdl_runtime_run_topology(const char *json)
{
    if (!json)
        return -1;

    rvdl_runtime *rt = g_active_runtime;
    if (!rt || !rt->jit)
        return -1;

    try
    {
        qhal::json::Value doc = qhal::json::parse(std::string(json));
        if (!doc.is_object() || !doc.has("blocks") || !doc.at("blocks").is_array())
            return -1;

        // ── 调用传播延迟：收集「被调用的子函数」集合 ──
        // callGraph 由 TopologyBuilder 计算（caller→callee, startAt, deltaT）。
        // 被调用的子函数由父函数体内的 call 指令在调用点串行执行（调用传播延迟
        // 的运行时语义），不应再作为独立拓扑块并行调度，否则与父函数顺序调用冲突。
        std::set<std::string> callees;
        if (doc.has("callGraph") && doc.at("callGraph").is_array())
        {
            for (const auto &ce : doc.at("callGraph").array())
                if (ce.is_object() && ce.has("callee"))
                    callees.insert(ce.at("callee").string());
        }

        // ── 解析拓扑块（含 coord / cost / deadline）──
        std::vector<ta::Block> blocks;
        for (const auto &bv : doc.at("blocks").array())
        {
            if (!bv.is_object() || !bv.has("name"))
                continue;
            const std::string name = bv.at("name").string();
            if (callees.count(name))
                continue; // 被调用的子函数：不独立调度，由父函数 call 执行
            ta::Block b;
            b.name = name;
            if (bv.has("time"))
                b.time = bv.at("time").int_value();
            if (bv.has("thread"))
                b.thread = bv.at("thread").int_value();
            if (bv.has("cost"))
                b.cost = bv.at("cost").int_value();
            if (bv.has("deadline") && !bv.at("deadline").is_null())
                b.deadline = bv.at("deadline").int_value();
            if (bv.has("coord") && bv.at("coord").is_array())
                for (const auto &cv : bv.at("coord").array())
                    b.coord.push_back(cv.int_value());
            blocks.push_back(std::move(b));
        }
        if (blocks.empty())
            return 0;

        // ====================================================================
        // 「穿梭」与「追踪」运行期接入（docs/rvdl-shuttle-tracking.md）
        //
        // 执行体住在 include/utils/ShuttleRuntime.hpp —— 抽出为头文件的唯一目的是
        // **可测试**：它必须能被独立编译、并对着编译器真实产出的调度表运行
        // （runtime/tests/shuttle_tracking_runtime.cpp）。否则
        // 「权重是否按 logWeight 路由」「下溢对端是否仍被调度」
        // 「影子槽是否按周期释放」这三条只能靠人工阅读确认 —— 而那正是最易静默
        // 出错处。三条纪律见该头文件顶部注释。
        //
        // 旧调度表缺这两段 → 全部成为惰性空操作（与既有行为兼容）。
        // ====================================================================
        rvdl::ShuttleRuntime shuttle_rt(&std::cout);
        shuttle_rt.parse(doc);

        int ancilla_bits = -1; // 未声明 → 视为不可用 → Grover 诚实降级
        if (const char *env = std::getenv("RVDL_ANCILLA_BITS"))
            ancilla_bits = std::atoi(env);

        // 超立方体跨度：各维取格点跨度 hi − min(0, lo) + 1。
        //
        // 从 **blocks** 而非 shape.coord 推导：shape 是「从原点起算的外接尺寸」，
        // 对负 coord 会给出退化的跨度（全负地址 → span=1 → rounds=0 → 穿梭静默
        // 失效）。此处与 server/src/topology.ts 的 span 定义严格一致。
        long long coord_span = 1;
        {
            int dims = 0;
            for (const auto &b : blocks)
                dims = std::max(dims, static_cast<int>(b.coord.size()));
            for (int i = 0; i < dims; ++i)
            {
                int lo = 0, hi = 0;
                bool seen = false;
                for (const auto &b : blocks)
                {
                    const int v = (i < static_cast<int>(b.coord.size())) ? b.coord[i] : 0;
                    if (!seen) { lo = v; hi = v; seen = true; }
                    else { lo = std::min(lo, v); hi = std::max(hi, v); }
                }
                coord_span *= std::max<long long>(1, hi - std::min(0, lo) + 1);
            }
        }

        shuttle_rt.plan_all(ancilla_bits, coord_span);

        // ── 拓扑线程分析：虚拟线程 + 残差并发 + 码距逻辑 qubit 布局 ──
        ta::TopologyAnalyzer ana(std::move(blocks));
        const auto threads     = ana.build_virtual_threads();           // 按 coord 分组
        const auto concurrency = ana.residual_concurrency(threads);     // 残差并发

        int min_dist = 1; // 码距阈值（同层并行时两虚拟线程所需的最小码距）
        if (const char *env = std::getenv("RVDL_TOPOLOGY_MIN_DISTANCE"))
            min_dist = std::atoi(env);
        const auto layout = ana.logical_qubit_layout(threads, min_dist); // 码距布局

        std::cout << "[Topology] virtual_threads=" << threads.size()
                  << ", critical_path=" << concurrency.critical_path
                  << ", total_work=" << concurrency.total_work
                  << ", residual_parallelism=" << concurrency.residual_parallelism
                  << ", max_width=" << concurrency.max_width
                  << ", min_code_distance=" << layout.min_distance
                  << ", min_distance_threshold=" << min_dist << std::endl;
        for (size_t i = 0; i < threads.size(); ++i)
        {
            std::cout << "[Topology] LQ[" << layout.qubit_id[i] << "] coord=(";
            for (size_t d = 0; d < threads[i].coord.size(); ++d)
                std::cout << (d ? "," : "") << threads[i].coord[d];
            std::cout << ")";
            if (threads.size() > 1)
            {
                std::cout << " dist=[";
                for (size_t j = 0; j < threads.size(); ++j)
                    std::cout << (j ? "," : "") << layout.distance[i][j];
                std::cout << "]";
            }
            std::cout << std::endl;
        }

        // 块名 → 虚拟线程索引（供码距感知分批）
        std::map<std::string, size_t> thread_of;
        for (size_t t = 0; t < threads.size(); ++t)
            for (size_t id : threads[t].blocks)
                thread_of[ana.blocks()[id].name] = t;

        // 预查找所有块函数（主线程串行，避免并发 LLJIT lookup）
        std::map<std::string, std::function<int()>> funcs;
        for (const auto &b : ana.blocks())
            if (auto fn = rt->jit->get_function<int()>(b.name))
                funcs[b.name] = fn;

        // 按 time 分层，保证叠加链（同 coord 不同 time）的时序顺序
        std::map<int, std::vector<const ta::Block *>> by_time;
        for (const auto &b : ana.blocks())
            by_time[b.time].push_back(&b);

        // 执行一批块（批内并行）
        auto run_batch = [&funcs](const std::vector<const ta::Block *> &batch)
        {
            if (batch.size() == 1)
            {
                auto it = funcs.find(batch[0]->name);
                if (it != funcs.end())
                    it->second();
            }
            else
            {
                std::vector<std::thread> ths;
                ths.reserve(batch.size());
                for (const ta::Block *b : batch)
                    ths.emplace_back([&funcs, name = b->name]()
                    {
                        auto it = funcs.find(name);
                        if (it != funcs.end())
                            it->second();
                    });
                for (auto &th : ths)
                    th.join();
            }
        };

        for (const auto &entry : by_time)
        {
            const std::vector<const ta::Block *> &layer = entry.second;

            // ① 层入口：开追踪作用域（EXTRACT → COPY）+ 目标层 pre 相位投递
            //    相位相对**目标层** time-slot（docs §3.2）：目标把关联当输入看到。
            std::vector<std::string> layer_names;
            layer_names.reserve(layer.size());
            for (const ta::Block *b : layer)
                layer_names.push_back(b->name);
            shuttle_rt.layer_enter(layer_names);

            if (min_dist <= 1)
            {
                // 码距阈值 <= 1：异 coord 均码距 >= 1，整层并行（原有行为）
                run_batch(layer);
            }
            else
            {
                // 按码距做逻辑 qubit 布局：同层内把码距 < 阈值的块串行到不同批次
                std::vector<std::vector<const ta::Block *>> batches;
                for (const ta::Block *b : layer)
                {
                    const size_t ti = thread_of.count(b->name) ? thread_of[b->name] : 0;
                    bool placed = false;
                    for (auto &batch : batches)
                    {
                        bool ok = true;
                        for (const ta::Block *other : batch)
                        {
                            const size_t tj = thread_of.count(other->name) ? thread_of[other->name] : 0;
                            if (layout.distance[ti][tj] < min_dist) { ok = false; break; }
                        }
                        if (ok) { batch.push_back(b); placed = true; break; }
                    }
                    if (!placed)
                        batches.push_back({b});
                }
                for (const auto &batch : batches)
                    run_batch(batch);
            }

            // ② 层出口：目标层 post 相位返回腿（双侧恢复的另一侧）
            //    → 源端捕获（把影子快照进渗透载荷）
            //    → 关作用域（RESTORE 保真校验，影子槽随周期释放，不构成持久克隆）
            shuttle_rt.layer_exit(layer_names);
        }
        return 0;
    }
    catch (const std::exception &e)
    {
        std::cerr << "[Topology] error: " << e.what() << std::endl;
        return -1;
    }
}