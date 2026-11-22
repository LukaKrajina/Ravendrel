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
#include "../include/utils/A2LIntegration.hpp"

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

static rvdl_runtime *g_active_runtime = nullptr;

namespace rvdl {
    static std::atomic<bool> s_gpu_backend_available{false};
    bool gpu_backend_available() noexcept { return s_gpu_backend_available.load(); }
    void set_gpu_backend_available(bool available) noexcept { s_gpu_backend_available.store(available); }
}

namespace
{
    bool detect_gpu_available()
    {
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
    std::vector<std::shared_ptr<qhal::MMIModule>> bound_mmis;
    std::mutex mutex;
};

rvdl_runtime *rvdl_runtime_create(void)
{
    try
    {
        (void)detect_gpu_available();
        const rvdl::accel::DeviceInfo dev = rvdl::accel::select(true, false);
        const bool gpu = dev.usable;
        rvdl::set_gpu_backend_available(gpu);
        rvdl::accel::set_device_ready(gpu);
        std::cout << "[Ravendrel JIT] " << rvdl::accel::describe(dev) << std::endl;

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
    A2L_TOUCH("runtime_api");
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
        rt->bound_mmis.push_back(mod);
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

        std::set<std::string> callees;
        if (doc.has("callGraph") && doc.at("callGraph").is_array())
        {
            for (const auto &ce : doc.at("callGraph").array())
                if (ce.is_object() && ce.has("callee"))
                    callees.insert(ce.at("callee").string());
        }

        std::vector<ta::Block> blocks;
        for (const auto &bv : doc.at("blocks").array())
        {
            if (!bv.is_object() || !bv.has("name"))
                continue;
            const std::string name = bv.at("name").string();
            if (callees.count(name))
                continue;
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

        rvdl::ShuttleRuntime shuttle_rt(&std::cout);
        shuttle_rt.parse(doc);

        int ancilla_bits = -1;
        if (const char *env = std::getenv("RVDL_ANCILLA_BITS"))
            ancilla_bits = std::atoi(env);

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

        ta::TopologyAnalyzer ana(std::move(blocks));
        const auto threads = ana.build_virtual_threads();
        const auto concurrency = ana.residual_concurrency(threads);

        int min_dist = 1;
        if (const char *env = std::getenv("RVDL_TOPOLOGY_MIN_DISTANCE"))
            min_dist = std::atoi(env);
        const auto layout = ana.logical_qubit_layout(threads, min_dist);

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

        std::map<std::string, size_t> thread_of;
        for (size_t t = 0; t < threads.size(); ++t)
            for (size_t id : threads[t].blocks)
                thread_of[ana.blocks()[id].name] = t;

        std::map<std::string, std::function<int()>> funcs;
        for (const auto &b : ana.blocks())
            if (auto fn = rt->jit->get_function<int()>(b.name))
                funcs[b.name] = fn;

        std::map<int, std::vector<const ta::Block *>> by_time;
        for (const auto &b : ana.blocks())
            by_time[b.time].push_back(&b);

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

            std::vector<std::string> layer_names;
            layer_names.reserve(layer.size());
            for (const ta::Block *b : layer)
                layer_names.push_back(b->name);
            shuttle_rt.layer_enter(layer_names);

            if (min_dist <= 1)
            {
                run_batch(layer);
            }
            else
            {
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