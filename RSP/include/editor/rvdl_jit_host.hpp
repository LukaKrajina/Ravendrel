#pragma once
#include <string>

namespace rvdlrsp::editor {

    struct JitResult {
        bool ok = false;
        std::string message;
        long long ret = 0;
        double fret = 0.0;
    };

    bool embedded_jit_available();
    JitResult embedded_jit_compile(const std::string &ir);
    JitResult embedded_jit_execute_int(const std::string &func_name);
    JitResult embedded_jit_execute_float(const std::string &func_name);
    JitResult embedded_jit_execute_void(const std::string &func_name);
    JitResult embedded_jit_compile_and_run(const std::string &ir, const std::string &func_name);
    void embedded_jit_shutdown();
}