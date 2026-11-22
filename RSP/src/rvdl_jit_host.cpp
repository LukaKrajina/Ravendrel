#include "../include/editor/rvdl_jit_host.hpp"

#include "qhal/RuntimeApi.h"

#include <mutex>
#include <string>
#include <iostream>

namespace rvdlrsp::editor
{
    namespace
    {
        rvdl_runtime *g_rt = nullptr;
        std::mutex g_mutex;

        bool ensure_host()
        {
            if (g_rt)
                return true;
            g_rt = rvdl_runtime_create();
            return g_rt != nullptr;
        }

        JitResult do_compile(const std::string &ir)
        {
            JitResult r;
            if (!ensure_host())
            {
                r.message = "Embedded JIT unavailable.";
                return r;
            }
            if (ir.empty())
            {
                r.message = "Empty IR payload.";
                return r;
            }

            const char *resp = rvdl_runtime_compile(g_rt, ir.c_str());
            r.message = resp ? resp : "";
            r.ok = r.message.find("SUCCESS") != std::string::npos;
            return r;
        }

        JitResult do_execute_int(const std::string &func_name)
        {
            JitResult r;
            if (!ensure_host())
            {
                r.message = "Embedded JIT unavailable.";
                return r;
            }

            const char *resp = rvdl_runtime_execute_int(g_rt, func_name.c_str());
            r.message = resp ? resp : "";
            r.ok = r.message.find("ERROR") == std::string::npos;
            auto pos = r.message.find("(Returned: ");
            if (pos != std::string::npos)
                r.ret = std::stoll(r.message.substr(pos + 11));
            return r;
        }

        JitResult do_execute_float(const std::string &func_name)
        {
            JitResult r;
            if (!ensure_host())
            {
                r.message = "Embedded JIT unavailable.";
                return r;
            }

            const char *resp = rvdl_runtime_execute_float(g_rt, func_name.c_str());
            r.message = resp ? resp : "";
            r.ok = r.message.find("ERROR") == std::string::npos;
            auto pos = r.message.find("(Returned: ");
            if (pos != std::string::npos)
                r.fret = std::stod(r.message.substr(pos + 11));
            return r;
        }

        JitResult do_execute_void(const std::string &func_name)
        {
            JitResult r;
            if (!ensure_host())
            {
                r.message = "Embedded JIT unavailable.";
                return r;
            }

            const char *resp = rvdl_runtime_execute_void(g_rt, func_name.c_str());
            r.message = resp ? resp : "";
            r.ok = r.message.find("ERROR") == std::string::npos;
            return r;
        }
    }

    bool embedded_jit_available()
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        return ensure_host();
    }

    JitResult embedded_jit_compile(const std::string &ir)
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        return do_compile(ir);
    }

    JitResult embedded_jit_execute_int(const std::string &func_name)
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        return do_execute_int(func_name);
    }

    JitResult embedded_jit_execute_float(const std::string &func_name)
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        return do_execute_float(func_name);
    }

    JitResult embedded_jit_execute_void(const std::string &func_name)
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        return do_execute_void(func_name);
    }

    JitResult embedded_jit_compile_and_run(const std::string &ir, const std::string &func_name)
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        JitResult r = do_compile(ir);
        if (!r.ok)
            return r;
        return do_execute_int(func_name);
    }

    void embedded_jit_shutdown()
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        if (g_rt)
        {
            rvdl_runtime_destroy(g_rt);
            g_rt = nullptr;
        }
    }
}