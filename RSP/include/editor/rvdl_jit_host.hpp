#pragma once
#include <string>

namespace rvdlrsp::editor {

    // ─── 内嵌 JIT 执行结果 ──────────────────────────────────────────
    struct JitResult {
        bool ok = false;
        std::string message;
        long long ret = 0;     // int 返回值
        double fret = 0.0;     // float 返回值
    };

    // 内嵌 JIT 主机：在 rvdlRSP 进程内实例化 qhal::JIT + 量子后端，
    // 无需外部 ravendrel 守护进程。
    bool embedded_jit_available();
    JitResult embedded_jit_compile(const std::string &ir);
    JitResult embedded_jit_execute_int(const std::string &func_name);
    JitResult embedded_jit_execute_float(const std::string &func_name);
    JitResult embedded_jit_execute_void(const std::string &func_name);
    JitResult embedded_jit_compile_and_run(const std::string &ir, const std::string &func_name);
    void embedded_jit_shutdown();
}