#include <iostream>
#include <string>
#include <memory>
#include <sstream>
#include <vector>
#include <thread>
#include <map>
#include <mutex>
#include <filesystem>
#include <cstdint>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <cstdio>
#else
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <sys/wait.h>
#include <cstdio>
#include <cstdlib>
#include <limits.h>
#endif

#include "../include/qhal/RuntimeApi.h"
#include "../include/qml/ModelInterop.hpp"

#define DAEMON_PORT 50052
#define BUFFER_SIZE 65536

static std::map<int, rvdl_mmi *> g_mmi_modules;
static int g_mmi_next_id = 1;
static std::mutex g_mmi_mutex;

// 解析 AOT 请求头行中的可选工具链覆盖项（"clangxx=... sysroot=... rtdir=...
// extra_libs=..."，位于 "<arch> <mode> <name>" 之后），用于 Android 交叉编译，
// 优先于 daemon 进程的环境变量（RVDL_ANDROID_*）。
static void parse_aot_overrides(std::istream &ss,
                                std::string &clangxx, std::string &sysroot,
                                std::string &rtdir, std::string &extra_libs)
{
    std::string tok;
    while (ss >> tok)
    {
        const size_t eq = tok.find('=');
        if (eq == std::string::npos)
            continue;
        const std::string k = tok.substr(0, eq);
        const std::string v = tok.substr(eq + 1);
        if (k == "clangxx")
            clangxx = v;
        else if (k == "sysroot")
            sysroot = v;
        else if (k == "rtdir")
            rtdir = v;
        else if (k == "extra_libs")
        {
            // '|' 是发送端对空格的编码（头行本身按空白分隔 token）。
            std::string s = v;
            for (char &ch : s)
                if (ch == '|')
                    ch = ' ';
            extra_libs = s;
        }
    }
}

// 模型容器转换（导出 / 导入）：stdin 文本命令与二进制协议
// （CMD_EXPORT_MODEL / CMD_IMPORT_MODEL）共用，定义在 process_stream 之后。
void handle_model_convert(const std::string &verb, const std::string &args,
                          std::ostream &out_stream);

void process_stream(std::istream &in_stream, std::ostream &out_stream, rvdl_runtime *rt)
{
    std::string command;
    while (std::getline(in_stream, command))
    {
        if (command == "EXIT")
            break;

        if (command == "PING")
        {
            out_stream << "RESPONSE: PONG\n";
        }
        else if (command == "GET_SNAPSHOT")
        {
            out_stream << rvdl_runtime_snapshot(rt);
        }
        else if (command == "COMPILE")
        {
            std::string ir_payload;
            std::string line;

            while (std::getline(in_stream, line))
            {
                if (line == "END_COMPILE")
                    break;
                ir_payload += line + "\n";
            }

            out_stream << rvdl_runtime_compile(rt, ir_payload.c_str());
        }
        else if (command == "VERIFY")
        {
            std::string vc_protocol;
            std::string line;

            while (std::getline(in_stream, line))
            {
                if (line == "END_VERIFY")
                    break;
                vc_protocol += line + "\n";
            }

            out_stream << rvdl_runtime_verify(rt, vc_protocol.c_str());
        }
        else if (command.rfind("AOT_COMPILE ", 0) == 0)
        {
            std::stringstream cmd_ss(command);
            std::string action, arch, mode, filename;
            cmd_ss >> action >> arch >> mode >> filename;

            std::string clangxx, sysroot, rtdir, extra_libs;
            parse_aot_overrides(cmd_ss, clangxx, sysroot, rtdir, extra_libs);

            std::string ir_payload;
            std::string line;

            while (std::getline(in_stream, line))
            {
                if (line == "END_COMPILE")
                    break;
                ir_payload += line + "\n";
            }

            out_stream << rvdl_runtime_aot_compile_ex(rt, arch.c_str(), mode.c_str(),
                                                      filename.c_str(), ir_payload.c_str(),
                                                      clangxx.empty() ? nullptr : clangxx.c_str(),
                                                      sysroot.empty() ? nullptr : sysroot.c_str(),
                                                      rtdir.empty() ? nullptr : rtdir.c_str(),
                                                      extra_libs.empty() ? nullptr : extra_libs.c_str());
        }
        else if (command.rfind("EXECUTE ", 0) == 0)
        {
            std::stringstream cmd_ss(command);
            std::string action, command_type, func_name;
            cmd_ss >> action >> command_type >> func_name;

            if (func_name.empty())
            {
                func_name = command_type;
                command_type = "int32";
            }

            if (command_type == "int32" || command_type == "int")
            {
                out_stream << rvdl_runtime_execute_int(rt, func_name.c_str());
            }
            else if (command_type == "void")
            {
                out_stream << rvdl_runtime_execute_void(rt, func_name.c_str());
            }
            else if (command_type == "float")
            {
                out_stream << rvdl_runtime_execute_float(rt, func_name.c_str());
            }
            else
            {
                out_stream << "RESPONSE: ERROR - Unsupported return type '" << command_type
                           << "' for function " << func_name << "\n";
            }
        }
        else if (command.rfind("QCHAIN ", 0) == 0)
        {
            std::stringstream cmd_ss(command);
            std::string action, op;
            cmd_ss >> action >> op;
            if (op == "wallet")
                out_stream << rvdl_runtime_qchain_wallet(rt);
            else if (op == "mint")
            {
                std::string addr; uint64_t amt = 0; cmd_ss >> addr >> amt;
                out_stream << rvdl_runtime_qchain_mint(rt, addr.c_str(), amt);
            }
            else if (op == "transfer")
            {
                std::string from, to; uint64_t amt = 0; cmd_ss >> from >> to >> amt;
                out_stream << rvdl_runtime_qchain_transfer(rt, from.c_str(), to.c_str(), amt);
            }
            else if (op == "balance")
            {
                std::string addr; cmd_ss >> addr;
                out_stream << rvdl_runtime_qchain_balance(rt, addr.c_str());
            }
            else if (op == "mine")
                out_stream << rvdl_runtime_qchain_mine(rt);
            else if (op == "height")
                out_stream << rvdl_runtime_qchain_height(rt);
            else if (op == "verify")
                out_stream << rvdl_runtime_qchain_verify(rt);
            else if (op == "qkd")
            {
                int32_t rounds = 0; cmd_ss >> rounds;
                out_stream << rvdl_runtime_qchain_qkd(rt, rounds);
            }
            else if (op == "qdba")
            {
                int32_t parties = 0; cmd_ss >> parties;
                out_stream << rvdl_runtime_qchain_qdba(rt, parties);
            }
            else
                out_stream << "RESPONSE: ERROR - Unknown qchain op '" << op << "'\n";
        }
        else if (command.rfind("LOAD_MMI ", 0) == 0)
        {
            std::string path = command.substr(9);
            std::lock_guard<std::mutex> lk(g_mmi_mutex);
            rvdl_mmi *m = rvdl_runtime_load_mmi(rt, path.c_str());
            if (m)
            {
                int id = g_mmi_next_id++;
                g_mmi_modules[id] = m;
                out_stream << "RESPONSE: MMI_LOADED " << id << "\n";
            }
            else
            {
                out_stream << "RESPONSE: ERROR - MMI load failed\n";
            }
        }
        else if (command.rfind("MMI_INVOKE ", 0) == 0)
        {
            std::stringstream cmd_ss(command);
            std::string action;
            int id = 0;
            std::string func_name;
            cmd_ss >> action >> id >> func_name;

            std::string args_json;
            std::getline(cmd_ss, args_json);
            size_t pos = args_json.find_first_not_of(" \t");
            args_json = (pos == std::string::npos) ? "" : args_json.substr(pos);

            std::lock_guard<std::mutex> lk(g_mmi_mutex);
            auto it = g_mmi_modules.find(id);
            if (it != g_mmi_modules.end())
            {
                out_stream << rvdl_runtime_mmi_invoke(it->second, func_name.c_str(), args_json.c_str());
            }
            else
            {
                out_stream << "RESPONSE: ERROR - MMI handle not found\n";
            }
        }
        else if (command.rfind("MMI_UNLOAD ", 0) == 0)
        {
            int id = std::stoi(command.substr(11));
            std::lock_guard<std::mutex> lk(g_mmi_mutex);
            auto it = g_mmi_modules.find(id);
            if (it != g_mmi_modules.end())
            {
                rvdl_runtime_mmi_unload(it->second);
                g_mmi_modules.erase(it);
                out_stream << "RESPONSE: MMI_UNLOADED\n";
            }
            else
            {
                out_stream << "RESPONSE: ERROR - MMI handle not found\n";
            }
        }
        else if (command.rfind("LOAD_NATIVE ", 0) == 0)
        {
            std::string path = command.substr(12);
            int32_t ok = rvdl_runtime_load_native(rt, path.c_str());
            if (ok)
                out_stream << "RESPONSE: NATIVE_LOADED\n";
            else
                out_stream << "RESPONSE: ERROR - native load failed\n";
        }
        // 模型容器转换（导出 / 导入）：文本与二进制协议共用同一实现。
        //
        // 语法（文本命令与 CMD_EXPORT_MODEL / CMD_IMPORT_MODEL 的 payload 同形）：
        //   EXPORT_SAFETENSORS <in.rvdlm> <out.safetensors> [张量名] [类型]
        //   EXPORT_GGUF         <in.rvdlm> <out.gguf>        [张量名] [类型]
        //   IMPORT_SAFETENSORS <in.safetensors> <out.rvdlm> [张量名]
        //   IMPORT_GGUF         <in.gguf>        <out.rvdlm> [张量名]
        //
        // 导出为什么必须有"类型"：`.rvdlm` 的头部（QKMHeader）**不记录元素
        // 类型**，只有元素个数和形状。读取方必须声明类型 —— 声明错了不会报错，
        // 只会得到一份"看起来像张量"的垃圾（f64 的字节按 f32 重组）。所以默认
        // f32 并在应答里**回显实际使用的类型**，让调用方有机会发现不一致。
        // 导入为什么**不需要**：容器自带 dtype（safetensors JSON / GGUF 张量
        // 信息段），按容器 dtype 自动分发 —— 两侧的信息不对称决定了语法不对称。
        else if (command.rfind("EXPORT_SAFETENSORS ", 0) == 0 ||
                 command.rfind("EXPORT_GGUF ", 0) == 0 ||
                 command.rfind("IMPORT_SAFETENSORS ", 0) == 0 ||
                 command.rfind("IMPORT_GGUF ", 0) == 0)
        {
            std::stringstream ss(command);
            std::string verb, rest;
            ss >> verb;
            std::getline(ss, rest);
            const size_t p = rest.find_first_not_of(' ');
            handle_model_convert(verb, p == std::string::npos ? std::string() : rest.substr(p),
                                 out_stream);
        }
    }
}

// ─── 模型容器转换（导出 / 导入）──────────────────────────────
// 从 stdin 文本命令与二进制协议（CMD_EXPORT_MODEL / CMD_IMPORT_MODEL）
// 两路进入。错误一律以 RESPONSE: ERROR 行返回，不抛异常穿过协议边界。
void handle_model_convert(const std::string &verb, const std::string &args,
                          std::ostream &out_stream)
{
    std::stringstream ss(verb + " " + args);
    std::string v, in_path, out_path, tensor_name, type_name;
    ss >> v >> in_path >> out_path >> tensor_name >> type_name;

    const bool is_import = (v.rfind("IMPORT_", 0) == 0);
    const bool to_gguf = (v == "EXPORT_GGUF" || v == "IMPORT_GGUF");
    const bool fmt_ok = (v == "EXPORT_SAFETENSORS" || v == "EXPORT_GGUF" ||
                         v == "IMPORT_SAFETENSORS" || v == "IMPORT_GGUF");
    if (!fmt_ok || in_path.empty() || out_path.empty())
    {
        out_stream << "RESPONSE: ERROR - usage: "
                   << "EXPORT_SAFETENSORS|EXPORT_GGUF <in.rvdlm> <out> [tensor] [f32|f64|i32|i64|u8] | "
                   << "IMPORT_SAFETENSORS|IMPORT_GGUF <in> <out.rvdlm> [tensor]\n";
        return;
    }

    std::string err;
    bool ok = false;
    try
    {
        if (is_import)
        {
            // 导入：类型按容器 dtype 自动推导（张量名缺省 = 取第一个；
            // 多张量容器不猜，找不到时列出可用名）。
            ok = to_gguf
                     ? qml::interop::import_gguf_to_rvdlm_auto(in_path, out_path,
                                                               tensor_name, &err)
                     : qml::interop::import_safetensors_to_rvdlm_auto(in_path, out_path,
                                                                      tensor_name, &err);
            if (ok)
                out_stream << "RESPONSE: IMPORTED " << out_path << " (rvdlm, from "
                           << (to_gguf ? "gguf" : "safetensors")
                           << (tensor_name.empty() ? "" : ", tensor=" + tensor_name) << ")\n";
            else
                out_stream << "RESPONSE: ERROR - import failed: " << err << "\n";
            return;
        }

        // 导出：张量名缺省 "weight"（与导出 API 默认一致），类型缺省 f32。
        if (tensor_name.empty()) tensor_name = "weight";
        if (type_name.empty()) type_name = "f32";
        bool known = qml::interop::with_rvdlm_type(
            type_name,
            [&]<typename T>()
            {
                return to_gguf
                           ? qml::interop::export_rvdlm_to_gguf<T>(in_path, out_path,
                                                                   tensor_name, &err)
                           : qml::interop::export_rvdlm_to_safetensors<T>(in_path, out_path,
                                                                         tensor_name, &err);
            },
            ok);
        if (!known)
            out_stream << "RESPONSE: ERROR - unknown element type '" << type_name
                       << "' (f32|f64|i32|i64|u8)\n";
        else if (!ok)
            out_stream << "RESPONSE: ERROR - export failed: " << err << "\n";
        else
            out_stream << "RESPONSE: EXPORTED " << out_path << " ("
                       << (to_gguf ? "gguf" : "safetensors") << ", " << type_name
                       << ", tensor=" << tensor_name << ")\n";
    }
    catch (const std::exception &e)
    {
        out_stream << "RESPONSE: ERROR - " << (is_import ? "import" : "export")
                   << " failed: " << e.what() << "\n";
    }
}

// ─── 二进制协议(长度前缀 + 命令字节)──────────────────────
// 帧格式: [uint32 大端长度][payload],payload 首字节为命令。
// 相比旧的文本行协议,长度前缀消除边界歧义(IR 内容含哨兵行也不破坏),
// 命令字节消除字符串比较开销;HELLO 帧用于版本协商。
enum ProtocolCommand : uint8_t
{
    CMD_HELLO = 0x00,        // 握手: payload[1..] = "RVDL_PROTO_V1"
    CMD_COMPILE = 0x01,      // payload[1..] = LLVM IR
    CMD_EXECUTE = 0x02,      // payload[1..] = "return_type func_name"
    CMD_VERIFY = 0x03,       // payload[1..] = VC protocol
    CMD_AOT_COMPILE = 0x04,  // payload[1..] = "arch mode name [clangxx=... sysroot=... rtdir=... extra_libs=...]\nIR"
    CMD_LOAD_MMI = 0x05,     // payload[1..] = path
    CMD_MMI_INVOKE = 0x06,   // payload[1..] = "id func_name args_json"
    CMD_MMI_UNLOAD = 0x07,   // payload[1..] = "id"
    CMD_PING = 0x08,
    CMD_GET_SNAPSHOT = 0x09,
    CMD_QCHAIN = 0x10,       // payload[1..] = "op args..."（量子区块链服务）
    CMD_LOAD_NATIVE = 0x0A,  // payload[1..] = path（加载原生动态库，供 JIT 解析符号）
    CMD_BIND_MMI = 0x0B,     // payload[1..] = "alias path"（加载 .rvdli 并绑定到主 JIT）
    CMD_EXECUTE_TOPOLOGY = 0x0C, // payload[1..] = "int32 rvdl_topology_entry"（多维标签函数拓扑调度入口）
    CMD_COMPILE_MIR = 0x0D,      // payload[1..] = MIR JSON（下沉 LLVM C++ API 构建 Module）
    CMD_EXPORT_MODEL = 0x0E,     // payload[1..] = "<safetensors|gguf> <in.rvdlm> <out> [tensor] [f32|f64|i32|i64|u8]"
    CMD_IMPORT_MODEL = 0x0F,     // payload[1..] = "<safetensors|gguf> <in> <out.rvdlm> [tensor]"（类型按容器 dtype 推导）
    CMD_EXIT = 0xFF
};

static const char *kProtocolVersion = "RVDL_PROTO_V1";

// 按命令字节分发执行,结果追加到 out。
void process_command(const std::string &payload, std::string &out, rvdl_runtime *rt)
{
    if (payload.empty())
        return;
    uint8_t cmd = static_cast<uint8_t>(payload[0]);
    const std::string body = payload.substr(1);

    switch (cmd)
    {
    case CMD_PING:
        out += "RESPONSE: PONG\n";
        break;
    case CMD_GET_SNAPSHOT:
        out += rvdl_runtime_snapshot(rt);
        break;
    case CMD_COMPILE:
        out += rvdl_runtime_compile(rt, body.c_str());
        break;
    case CMD_COMPILE_MIR:
        out += rvdl_runtime_compile_mir(rt, body.c_str());
        break;
    case CMD_EXPORT_MODEL:
    case CMD_IMPORT_MODEL:
    {
        // payload 形如 "<verb> <in> <out> [tensor] [type]"，verb 接受两种写法：
        //   ① 显式动词：EXPORT_SAFETENSORS / EXPORT_GGUF / IMPORT_SAFETENSORS / IMPORT_GGUF
        //   ② 格式名：safetensors / gguf（由命令字节决定导入/导出方向）
        // 两种写法归一为动词后走与 stdin 文本命令同一实现。
        std::stringstream ss(body);
        std::string verb, rest;
        ss >> verb;
        std::getline(ss, rest);
        const size_t p = rest.find_first_not_of(' ');
        std::string args = (p == std::string::npos) ? std::string() : rest.substr(p);
        if (verb == "safetensors" || verb == "gguf")
        {
            const bool is_export = (cmd == CMD_EXPORT_MODEL);
            verb = std::string(is_export ? "EXPORT_" : "IMPORT_")
                   + (verb == "gguf" ? "GGUF" : "SAFETENSORS");
        }
        {
            // handle_model_convert 写入 std::ostream，而 process_command 的 out 是
            // std::string —— 需经 ostringstream 转接（原先直接传 string，导致
            // "no matching function for call to handle_model_convert" 编译失败）。
            std::ostringstream conv_out;
            handle_model_convert(verb, args, conv_out);
            out += conv_out.str();
        }
        break;
    }
    case CMD_VERIFY:
        out += rvdl_runtime_verify(rt, body.c_str());
        break;
    case CMD_AOT_COMPILE:
    {
        size_t nl = body.find('\n');
        std::string head = (nl == std::string::npos) ? body : body.substr(0, nl);
        std::string ir = (nl == std::string::npos) ? std::string() : body.substr(nl + 1);
        std::stringstream ss(head);
        std::string arch, mode, filename;
        ss >> arch >> mode >> filename;

        std::string clangxx, sysroot, rtdir, extra_libs;
        parse_aot_overrides(ss, clangxx, sysroot, rtdir, extra_libs);

        out += rvdl_runtime_aot_compile_ex(rt, arch.c_str(), mode.c_str(), filename.c_str(), ir.c_str(),
                                            clangxx.empty() ? nullptr : clangxx.c_str(),
                                            sysroot.empty() ? nullptr : sysroot.c_str(),
                                            rtdir.empty() ? nullptr : rtdir.c_str(),
                                            extra_libs.empty() ? nullptr : extra_libs.c_str());
        break;
    }
    case CMD_EXECUTE:
    {
        std::stringstream ss(body);
        std::string ret_type, func_name;
        ss >> ret_type >> func_name;
        if (func_name.empty())
        {
            func_name = ret_type;
            ret_type = "int32";
        }
        if (ret_type == "int32" || ret_type == "int")
            out += rvdl_runtime_execute_int(rt, func_name.c_str());
        else if (ret_type == "void")
            out += rvdl_runtime_execute_void(rt, func_name.c_str());
        else if (ret_type == "float")
            out += rvdl_runtime_execute_float(rt, func_name.c_str());
        else
            out += "RESPONSE: ERROR - Unsupported return type '" + ret_type + "' for function " + func_name + "\n";
        break;
    }
    case CMD_EXECUTE_TOPOLOGY:
    {
        // 多维标签函数拓扑调度入口：payload = "int32 rvdl_topology_entry"。
        // 执行 JIT 编译出的 rvdl_topology_entry，后者内部调用 rvdl_runtime_run_topology(json)
        // 完成按 (time, thread, coord) 的分层调度。
        std::stringstream ss(body);
        std::string ret_type, func_name;
        ss >> ret_type >> func_name;
        if (func_name.empty())
        {
            func_name = ret_type;
            ret_type = "int32";
        }
        out += rvdl_runtime_execute_int(rt, func_name.c_str());
        break;
    }
    case CMD_LOAD_NATIVE:
    {
        int32_t ok = rvdl_runtime_load_native(rt, body.c_str());
        if (ok)
            out += "RESPONSE: NATIVE_LOADED\n";
        else
            out += "RESPONSE: ERROR - native load failed\n";
        break;
    }
    case CMD_BIND_MMI:
    {
        // 格式 "alias path"；path 可能含空格，取首个空格之后的所有内容
        size_t sp = body.find(' ');
        std::string alias = (sp == std::string::npos) ? body : body.substr(0, sp);
        std::string path = (sp == std::string::npos) ? std::string() : body.substr(sp + 1);
        out += rvdl_runtime_bind_mmi(rt, alias.c_str(), path.c_str());
        break;
    }
    case CMD_LOAD_MMI:
    {
        std::lock_guard<std::mutex> lk(g_mmi_mutex);
        rvdl_mmi *m = rvdl_runtime_load_mmi(rt, body.c_str());
        if (m)
        {
            int id = g_mmi_next_id++;
            g_mmi_modules[id] = m;
            out += "RESPONSE: MMI_LOADED " + std::to_string(id) + "\n";
        }
        else
        {
            out += "RESPONSE: ERROR - MMI load failed\n";
        }
        break;
    }
    case CMD_MMI_INVOKE:
    {
        std::stringstream ss(body);
        int id = 0;
        std::string func_name;
        ss >> id >> func_name;
        std::string args_json;
        std::getline(ss, args_json);
        size_t pos = args_json.find_first_not_of(" \t");
        args_json = (pos == std::string::npos) ? std::string() : args_json.substr(pos);
        std::lock_guard<std::mutex> lk(g_mmi_mutex);
        auto it = g_mmi_modules.find(id);
        if (it != g_mmi_modules.end())
            out += rvdl_runtime_mmi_invoke(it->second, func_name.c_str(), args_json.c_str());
        else
            out += "RESPONSE: ERROR - MMI handle not found\n";
        break;
    }
    case CMD_MMI_UNLOAD:
    {
        int id = std::stoi(body);
        std::lock_guard<std::mutex> lk(g_mmi_mutex);
        auto it = g_mmi_modules.find(id);
        if (it != g_mmi_modules.end())
        {
            rvdl_runtime_mmi_unload(it->second);
            g_mmi_modules.erase(it);
            out += "RESPONSE: MMI_UNLOADED\n";
        }
        else
        {
            out += "RESPONSE: ERROR - MMI handle not found\n";
        }
        break;
    }
    case CMD_QCHAIN:
    {
        std::stringstream ss(body);
        std::string op;
        ss >> op;
        if (op == "wallet")
        {
            out += rvdl_runtime_qchain_wallet(rt);
        }
        else if (op == "mint")
        {
            std::string addr;
            uint64_t amt = 0;
            ss >> addr >> amt;
            out += rvdl_runtime_qchain_mint(rt, addr.c_str(), amt);
        }
        else if (op == "transfer")
        {
            std::string from, to;
            uint64_t amt = 0;
            ss >> from >> to >> amt;
            out += rvdl_runtime_qchain_transfer(rt, from.c_str(), to.c_str(), amt);
        }
        else if (op == "balance")
        {
            std::string addr;
            ss >> addr;
            out += rvdl_runtime_qchain_balance(rt, addr.c_str());
        }
        else if (op == "mine")
        {
            out += rvdl_runtime_qchain_mine(rt);
        }
        else if (op == "height")
        {
            out += rvdl_runtime_qchain_height(rt);
        }
        else if (op == "verify")
        {
            out += rvdl_runtime_qchain_verify(rt);
        }
        else if (op == "qkd")
        {
            int32_t rounds = 0;
            ss >> rounds;
            out += rvdl_runtime_qchain_qkd(rt, rounds);
        }
        else if (op == "qdba")
        {
            int32_t parties = 0;
            ss >> parties;
            out += rvdl_runtime_qchain_qdba(rt, parties);
        }
        else
        {
            out += "RESPONSE: ERROR - Unknown qchain op '" + op + "'\n";
        }
        break;
    }
    default:
        out += "RESPONSE: ERROR - Unknown command 0x" + std::to_string(static_cast<int>(cmd)) + "\n";
        break;
    }
}

#ifdef _WIN32
typedef SOCKET SocketHandle;
#define INVALID_SOCKET_HANDLE INVALID_SOCKET
#define CLOSE_SOCKET_HANDLE(s) closesocket(s)
#else
typedef int SocketHandle;
#define INVALID_SOCKET_HANDLE (-1)
#define CLOSE_SOCKET_HANDLE(s) close(s)
#endif

// 写一帧(4 字节大端长度 + payload)
static bool write_frame(SocketHandle sock, const std::string &payload)
{
    uint32_t len = static_cast<uint32_t>(payload.size());
    uint8_t lenbuf[4] = {
        static_cast<uint8_t>((len >> 24) & 0xFF),
        static_cast<uint8_t>((len >> 16) & 0xFF),
        static_cast<uint8_t>((len >> 8) & 0xFF),
        static_cast<uint8_t>(len & 0xFF)};

    size_t sent = 0;
    while (sent < 4)
    {
        int r = send(sock, reinterpret_cast<const char *>(lenbuf) + sent,
                     4 - static_cast<int>(sent), 0);
        if (r <= 0)
            return false;
        sent += static_cast<size_t>(r);
    }
    sent = 0;
    while (sent < payload.size())
    {
        int r = send(sock, payload.data() + sent,
                     static_cast<int>(payload.size() - sent), 0);
        if (r <= 0)
            return false;
        sent += static_cast<size_t>(r);
    }
    return true;
}

// 每个客户端连接的状态（增量解析「4 字节大端长度 + payload」帧，处理 TCP 分片）
struct Connection
{
    SocketHandle fd = INVALID_SOCKET_HANDLE;
    uint8_t lenbuf[4] = {0, 0, 0, 0};
    size_t len_pos = 0;      // 已读长度字节数（0..4）
    uint32_t payload_len = 0;
    std::string payload;
    size_t payload_pos = 0;  // 已读 payload 字节数
    bool have_len = false;   // 是否已读完 4 字节长度
};

// 处理一个完整帧；返回 false 表示应关闭连接。响应追加到 response（可能为空）。
static bool process_frame(const std::string &frame, std::string &response, rvdl_runtime *rt)
{
    if (frame.empty())
        return false;
    uint8_t cmd = static_cast<uint8_t>(frame[0]);
    if (cmd == CMD_EXIT)
        return false;

    // 握手：校验版本（向后兼容：未发送 HELLO 也继续处理）
    if (cmd == CMD_HELLO)
    {
        if (frame.substr(1) != kProtocolVersion)
        {
            response = "PROTO_ERR: version mismatch";
            return false;
        }
        return true;
    }

    try
    {
        process_command(frame, response, rt);
    }
    catch (const std::exception &e)
    {
        response = std::string("[QHAL ERROR] ") + e.what() + "\n";
    }
    return true;
}

void run_daemon(rvdl_runtime *rt)
{
#ifdef _WIN32
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);
#endif

    SocketHandle server_fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in address{};
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, (const char *)&opt, sizeof(opt));

    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(DAEMON_PORT);

    bind(server_fd, (struct sockaddr *)&address, sizeof(address));
    listen(server_fd, 16);

    std::cout << "[Ravendrel Daemon] Service online. Listening on port " << DAEMON_PORT << "..." << std::endl;

    // select 多路复用：主线程同时监听 server_fd 与所有客户端连接，避免某个长连接
    // （如 IDE 侧边栏的 GET_SNAPSHOT）独占串行处理、阻塞其它客户端（rvdl run / rvdl compile）。
    // COMPILE / EXECUTE 仍由主线程执行：Kokkos 量子后端（Threads/CUDA 执行空间）只能在
    // 主线程初始化与执行，detached worker 线程会触发
    // "ThreadsInternal::start FAILED: Called by a worker thread"。
    std::map<SocketHandle, Connection> conns;
    fd_set master;
    FD_ZERO(&master);
    FD_SET(server_fd, &master);

    while (true)
    {
        fd_set readfds = master;
#ifdef _WIN32
        int ready = select(0, &readfds, nullptr, nullptr, nullptr);
#else
        int ready = select(FD_SETSIZE, &readfds, nullptr, nullptr, nullptr);
#endif
        if (ready <= 0)
            continue;

        // 新连接
        if (FD_ISSET(server_fd, &readfds))
        {
            struct sockaddr_in client_addr{};
#ifdef _WIN32
            int addrlen = sizeof(client_addr);
            SocketHandle client = accept(server_fd, (struct sockaddr *)&client_addr, &addrlen);
#else
            socklen_t addrlen = sizeof(client_addr);
            SocketHandle client = accept(server_fd, (struct sockaddr *)&client_addr, &addrlen);
#endif
            if (client != INVALID_SOCKET_HANDLE)
            {
                FD_SET(client, &master);
                Connection c;
                c.fd = client;
                conns[client] = c;
            }
        }

        // 处理已有连接（有数据可读者）
        for (auto it = conns.begin(); it != conns.end();)
        {
            SocketHandle fd = it->first;
            Connection &c = it->second;
            if (!FD_ISSET(fd, &readfds))
            {
                ++it;
                continue;
            }

            bool closed = false;
            if (!c.have_len)
            {
                // 读 4 字节长度（可能只读到一部分）
                int r = recv(fd, reinterpret_cast<char *>(c.lenbuf) + c.len_pos,
                             static_cast<int>(4 - c.len_pos), 0);
                if (r <= 0)
                {
                    closed = true;
                }
                else
                {
                    c.len_pos += static_cast<size_t>(r);
                    if (c.len_pos == 4)
                    {
                        c.payload_len = (static_cast<uint32_t>(c.lenbuf[0]) << 24) |
                                        (static_cast<uint32_t>(c.lenbuf[1]) << 16) |
                                        (static_cast<uint32_t>(c.lenbuf[2]) << 8) |
                                        static_cast<uint32_t>(c.lenbuf[3]);
                        if (c.payload_len > 64u * 1024u * 1024u) // 防御：单帧最大 64MB
                        {
                            closed = true;
                        }
                        else
                        {
                            c.payload.resize(c.payload_len);
                            c.have_len = true;
                        }
                    }
                }
            }
            else
            {
                // 读 payload（可能只读到一部分）
                int r = recv(fd, c.payload.data() + c.payload_pos,
                             static_cast<int>(c.payload_len - c.payload_pos), 0);
                if (r <= 0)
                {
                    closed = true;
                }
                else
                {
                    c.payload_pos += static_cast<size_t>(r);
                    if (c.payload_pos == c.payload_len)
                    {
                        // 一帧完整，处理命令并回写响应
                        std::string response;
                        bool keep = process_frame(c.payload, response, rt);
                        if (!response.empty())
                            write_frame(fd, response);
                        // 重置帧解析状态，继续读下一帧
                        c.len_pos = 0;
                        c.have_len = false;
                        c.payload.clear();
                        c.payload_pos = 0;
                        if (!keep)
                            closed = true;
                    }
                }
            }

            if (closed)
            {
                FD_CLR(fd, &master);
                CLOSE_SOCKET_HANDLE(fd);
                it = conns.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }
}

#ifdef _WIN32
static std::wstring get_exe_path()
{
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(NULL, buf, MAX_PATH);
    return std::wstring(buf);
}

static bool install_autostart()
{
    std::wstring cmd = L"\"" + get_exe_path() + L"\" --daemon";
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
                      L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                      0, KEY_SET_VALUE, &key) != ERROR_SUCCESS)
        return false;
    LSTATUS st = RegSetValueExW(key, L"RavendrelRuntime", 0, REG_SZ,
                                (const BYTE *)cmd.c_str(),
                                (DWORD)((cmd.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
    return st == ERROR_SUCCESS;
}

static bool uninstall_autostart()
{
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
                      L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                      0, KEY_SET_VALUE, &key) != ERROR_SUCCESS)
        return false;
    LSTATUS st = RegDeleteValueW(key, L"RavendrelRuntime");
    RegCloseKey(key);
    return st == ERROR_SUCCESS || st == ERROR_FILE_NOT_FOUND;
}

static void redirect_to_log()
{
    wchar_t dir[MAX_PATH];
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", dir, MAX_PATH) == 0)
        return;
    std::wstring log_dir = std::wstring(dir) + L"\\Ravendrel";
    CreateDirectoryW(log_dir.c_str(), NULL);
    std::wstring log_path = log_dir + L"\\runtime.log";
    if (_wfreopen(log_path.c_str(), L"a", stdout) != NULL)
        _wfreopen(log_path.c_str(), L"a", stderr);
}
#else
static std::string get_exe_path()
{
    char buf[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", buf, PATH_MAX);
    if (n <= 0)
        return "runtime";
    return std::string(buf, n);
}

static bool run_command(const std::vector<std::string> &args)
{
    if (args.empty())
        return false;
    pid_t pid = fork();
    if (pid == 0)
    {
        std::vector<char *> argv;
        argv.reserve(args.size() + 1);
        for (const auto &a : args)
            argv.push_back(const_cast<char *>(a.c_str()));
        argv.push_back(nullptr);
        execvp(argv[0], argv.data());
        _exit(127); // exec 失败
    }
    if (pid < 0)
        return false;
    int status = 0;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

static bool install_autostart()
{
    const char *home = getenv("HOME");
    if (!home)
        return false;

    std::string exe = get_exe_path();
    std::string exe_dir = exe.substr(0, exe.find_last_of('/'));
    std::string unit_dir = std::string(home) + "/.config/systemd/user";
    std::string unit_path = unit_dir + "/ravendrel-runtime.service";
    std::error_code ec;
    std::filesystem::create_directories(unit_dir, ec);
    if (ec)
        return false;

    std::string unit =
        "[Unit]\n"
        "Description=Ravendrel Runtime Daemon (Quantum JIT Service)\n"
        "After=network-online.target\n"
        "Wants=network-online.target\n"
        "\n"
        "[Service]\n"
        "Type=simple\n"
        "ExecStart=" +
        exe + " --daemon\n"
              "Environment=LD_LIBRARY_PATH=" +
        exe_dir + "\n"
                  "Restart=on-failure\n"
                  "RestartSec=3\n"
                  "\n"
                  "[Install]\n"
                  "WantedBy=default.target\n";

    FILE *f = fopen(unit_path.c_str(), "w");
    if (!f)
        return false;
    fputs(unit.c_str(), f);
    fclose(f);
    run_command({"systemctl", "--user", "daemon-reload"});
    run_command({"systemctl", "--user", "enable", "ravendrel-runtime.service"});
    run_command({"systemctl", "--user", "start", "ravendrel-runtime.service"});
    const char *user = getenv("USER");
    if (user)
    {
        run_command({"loginctl", "enable-linger", user});
    }

    FILE *check = fopen(unit_path.c_str(), "r");
    if (check)
    {
        fclose(check);
        return true;
    }
    return false;
}

static bool uninstall_autostart()
{
    run_command({"systemctl", "--user", "stop", "ravendrel-runtime.service"});
    run_command({"systemctl", "--user", "disable", "ravendrel-runtime.service"});
    run_command({"systemctl", "--user", "daemon-reload"});

    const char *home = getenv("HOME");
    if (home)
    {
        std::string unit_path = std::string(home) + "/.config/systemd/user/ravendrel-runtime.service";
        remove(unit_path.c_str());
    }
    return true;
}
#endif

int main(int argc, char *argv[])
{
    bool daemon_mode = false;
    bool game_mode = false;
    std::string game_path;
    for (int i = 1; i < argc; ++i)
    {
        std::string arg = argv[i];
        if (arg == "--daemon")
        {
            daemon_mode = true;
        }
        else if (arg == "--game")
        {
            game_mode = true;
            if (i + 1 < argc)
                game_path = argv[++i];
        }
        else if (arg == "--install-autostart")
        {
            bool ok = install_autostart();
            return ok ? 0 : 1;
        }
        else if (arg == "--uninstall-autostart")
        {
            bool ok = uninstall_autostart();
            return ok ? 0 : 1;
        }
    }

#ifdef _WIN32
    if (daemon_mode)
        redirect_to_log();
#endif

    std::setvbuf(stdout, NULL, _IONBF, 0);
    std::setvbuf(stderr, NULL, _IONBF, 0);

    std::cout << "[Ravendrel JIT] Scanning network for Quantum FPGA Host..." << std::endl;

    rvdl_runtime *rt = rvdl_runtime_create();
    if (!rt)
    {
        return 1;
    }

    if (game_mode)
    {
        // 游戏模式：加载主程序 .rvdli（自动加载其 import 依赖）并调用入口
        std::cerr << "[Ravendrel] loading game .rvdli: " << game_path << std::endl;
        rvdl_mmi *game = rvdl_runtime_load_mmi(rt, game_path.c_str());
        if (game)
        {
            // 优先多维标签函数拓扑入口（@layer 程序），失败回退到 rvdl_main（旧程序）
            std::cerr << "[Ravendrel] invoking rvdl_topology_entry" << std::endl;
            const char *ret = rvdl_runtime_mmi_invoke(game, "rvdl_topology_entry", "[]");
            if (ret && std::string(ret).find("ERROR") == std::string::npos)
            {
                std::cerr << "[Ravendrel] rvdl_topology_entry returned: " << ret << std::endl;
            }
            else
            {
                std::cerr << "[Ravendrel] topology entry not found, invoking rvdl_main" << std::endl;
                const char *ret2 = rvdl_runtime_mmi_invoke(game, "rvdl_main", "[]");
                std::cerr << "[Ravendrel] rvdl_main returned: " << (ret2 ? ret2 : "<null>") << std::endl;
            }
            rvdl_runtime_mmi_unload(game);
        }
        else
        {
            std::cerr << "[Ravendrel] Failed to load game .rvdli: " << game_path << std::endl;
            rvdl_runtime_destroy(rt);
            return 1;
        }
    }
    else if (daemon_mode)
    {
        rvdl_runtime_viz_start(rt);
        run_daemon(rt);
        rvdl_runtime_viz_stop(rt);
    }
    else
    {
        std::cout << "READY" << std::endl;
        process_stream(std::cin, std::cout, rt);
    }

    rvdl_runtime_destroy(rt);
    return 0;
}