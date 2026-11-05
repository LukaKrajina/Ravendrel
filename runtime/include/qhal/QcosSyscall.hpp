#pragma once
// ============================================================================
//  Ravendrel Compute OS（RC OS）系统调用层
//
//  server/src/ir.ts 早已 declare 了下列 QCOS 符号，但运行时从未提供实现，
//  导致 JIT 链接期符号解析失败：
//      rvdl_sys_call / rvdl_sys_calld / rvdl_sys_log / rvdl_sys_logi / rvdl_gc_alloc
//
//  补完上述符号，并新增两个需要的入口：
//      rvdl_sys_callp —— 返回指针的 syscall（SYS_MAP 需要 64 位返回值）
//      rvdl_gc_free   —— 归还 rvdl_gc_alloc 分配的内存
//
//  能力校验：每个 syscall 号绑定一个 CAP_* 位，掩码由 qcos::set_caps()
//     设置，对应 RVDL 源码的 `requires <cap>` 声明。hosted 模式默认 CAP_ALL。
//  宿主钩子 HostHooks：QPU / IRQ / yield / exit / panic 的真实实现由
//     qcos_core 安装；未安装时返回 QCOS_ENOSYS，绝不崩溃。
//  与 JIT.hpp 集成方式一致：声明无条件，定义仅在 RVDL_RT_BUILD 下编译。
// ============================================================================

#include "Export.hpp"

#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <unordered_map>

#ifndef RVDL_HOST_EXPORT
#define RVDL_HOST_EXPORT extern "C" RVDL_RT_API
#endif

namespace qcos
{
    // ASCII 换行符（避免源码出现字面反斜杠，便于各类生成器/工具链处理）
    inline char nl() { return static_cast<char>(10); }

    // ---- 系统调用号 ----------------------------------------------------
    enum SyscallNumber : std::int32_t
    {
        SYS_LOG          = 1,
        SYS_YIELD        = 2,
        SYS_EXIT         = 3,
        SYS_MAP          = 4,
        SYS_UNMAP        = 5,
        SYS_IRQ_REGISTER = 6,
        SYS_IRQ_ACK      = 7,
        SYS_QPU_SUBMIT   = 8,
        SYS_QPU_AWAIT    = 9,
        SYS_TIME         = 10,
        SYS_PANIC        = 11,
    };

    // ---- 能力位 --------------------------------------------------------
    enum CapabilityBits : std::uint32_t
    {
        CAP_LOG  = 1u << 0,
        CAP_PROC = 1u << 1,
        CAP_MAP  = 1u << 2,
        CAP_IRQ  = 1u << 3,
        CAP_QPU  = 1u << 4,
        CAP_TIME = 1u << 5,
        CAP_ALL  = 0xFFFFFFFFu,
    };

    inline std::uint32_t required_cap(std::int32_t num)
    {
        switch (num)
        {
        case SYS_LOG:          return CAP_LOG;
        case SYS_YIELD:        return CAP_PROC;
        case SYS_EXIT:         return CAP_PROC;
        case SYS_MAP:          return CAP_MAP;
        case SYS_UNMAP:        return CAP_MAP;
        case SYS_IRQ_REGISTER: return CAP_IRQ;
        case SYS_IRQ_ACK:      return CAP_IRQ;
        case SYS_QPU_SUBMIT:   return CAP_QPU;
        case SYS_QPU_AWAIT:    return CAP_QPU;
        case SYS_TIME:         return CAP_TIME;
        case SYS_PANIC:        return CAP_LOG;
        default:               return 0;
        }
    }

    // ---- 错误码 --------------------------------------------------------
    enum StatusCode : std::int32_t
    {
        QCOS_OK     = 0,
        QCOS_EPERM  = -1,
        QCOS_EINVAL = -2,
        QCOS_ENOSYS = -3,
        QCOS_ENOMEM = -4,
    };

    // ---- 执行上下文能力掩码 --------------------------------------------
    inline std::uint32_t &caps_slot()
    {
        static std::uint32_t caps = CAP_ALL;
        return caps;
    }
    inline void          set_caps(std::uint32_t m) { caps_slot() = m; }
    inline std::uint32_t get_caps()                { return caps_slot(); }
    inline bool          has_cap(std::uint32_t b)  { return (caps_slot() & b) != 0u; }

    // ---- 宿主钩子 --------------------
    struct HostHooks
    {
        std::int32_t (*qpu_submit)(std::uint64_t job_id, std::uint32_t gate_count,
                                   std::uint32_t shots, void *circuit) = nullptr;
        std::int32_t (*qpu_await)(std::uint64_t job_id, std::uint32_t timeout_ms) = nullptr;
        void (*irq_register)(std::uint32_t vec, void *handler) = nullptr;
        void (*irq_ack)(std::uint32_t vec) = nullptr;
        void (*yield_fn)(void) = nullptr;
        void (*exit_fn)(std::int32_t code) = nullptr;
        void (*panic_fn)(const char *msg) = nullptr;
    };

    inline HostHooks &hooks()
    {
        static HostHooks h;
        return h;
    }

    // ---- 日志 ----------------------------------------------------------
    using LogSink = void (*)(void *user, int level, const char *msg);

    inline LogSink &log_sink()    { static LogSink s = nullptr; return s; }
    inline void *&log_sink_user() { static void *u = nullptr;   return u; }

    inline void set_log_sink(LogSink s, void *user)
    {
        log_sink() = s;
        log_sink_user() = user;
    }

    inline const char *level_name(int level)
    {
        switch (level)
        {
        case 0:  return "ERROR";
        case 1:  return "WARN ";
        case 3:  return "DEBUG";
        default: return "INFO ";
        }
    }

    inline void emit_log(int level, const char *msg)
    {
        if (log_sink())
        {
            log_sink()(log_sink_user(), level, msg ? msg : "");
            return;
        }
        std::fprintf(stdout, "[QCOS:%s] %s%c", level_name(level),
                     msg ? msg : "", nl());
        std::fflush(stdout);
    }

    // ---- 分配登记表（rvdl_gc_alloc 与 SYS_MAP 共用）----------------------
    inline std::mutex &registry_mutex()
    {
        static std::mutex m;
        return m;
    }
    inline std::unordered_map<void *, std::size_t> &alloc_registry()
    {
        static std::unordered_map<void *, std::size_t> r;
        return r;
    }
    inline std::atomic<std::size_t> &alloc_bytes()
    {
        static std::atomic<std::size_t> n{0};
        return n;
    }
    inline std::atomic<std::size_t> &alloc_live()
    {
        static std::atomic<std::size_t> n{0};
        return n;
    }

    inline void *tracked_alloc(std::size_t size)
    {
        if (size == 0)
            return nullptr;
        void *p = std::calloc(1, size);
        if (!p)
            return nullptr;
        {
            std::lock_guard<std::mutex> g(registry_mutex());
            alloc_registry()[p] = size;
        }
        alloc_bytes().fetch_add(size, std::memory_order_relaxed);
        alloc_live().fetch_add(1, std::memory_order_relaxed);
        return p;
    }

    inline bool tracked_free(void *p)
    {
        if (!p)
            return false;
        std::size_t size = 0;
        {
            std::lock_guard<std::mutex> g(registry_mutex());
            auto it = alloc_registry().find(p);
            if (it == alloc_registry().end())
                return false;
            size = it->second;
            alloc_registry().erase(it);
        }
        alloc_bytes().fetch_sub(size, std::memory_order_relaxed);
        alloc_live().fetch_sub(1, std::memory_order_relaxed);
        std::free(p);
        return true;
    }

    // ---- 单调时钟 ------------------------------------------------------
    inline std::chrono::steady_clock::time_point &epoch()
    {
        static std::chrono::steady_clock::time_point t = std::chrono::steady_clock::now();
        return t;
    }

    inline double monotonic_seconds()
    {
        return std::chrono::duration<double>(
                   std::chrono::steady_clock::now() - epoch()).count();
    }

    inline std::int64_t monotonic_millis()
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now() - epoch()).count();
    }

}

// ============================================================================
//  C ABI —— 与 server/src/ir.ts 中的 declare 一一对应
// ============================================================================
RVDL_HOST_EXPORT std::int32_t rvdl_sys_call(std::int32_t num, std::int32_t a0,
                                           std::int32_t a1, std::int32_t a2);
RVDL_HOST_EXPORT double       rvdl_sys_calld(std::int32_t num, double a0, double a1);
RVDL_HOST_EXPORT void         rvdl_sys_log(std::int32_t level, const char *msg);
RVDL_HOST_EXPORT std::int32_t rvdl_sys_logi(std::int32_t level, std::int32_t value);
RVDL_HOST_EXPORT void        *rvdl_gc_alloc(std::int64_t size);
RVDL_HOST_EXPORT void        *rvdl_sys_callp(std::int32_t num, std::int64_t a0,
                                            std::int64_t a1, std::int64_t a2);
RVDL_HOST_EXPORT void         rvdl_gc_free(void *ptr);

#if defined(RVDL_RT_BUILD)

// ---------------------------------------------------------------------------
// 能力校验：失败时记录一条 WARN 并返回 false
// ---------------------------------------------------------------------------
static bool qcos_require(std::int32_t num)
{
    const std::uint32_t need = qcos::required_cap(num);
    if (need != 0u && !qcos::has_cap(need))
    {
        char buf[128];
        std::snprintf(buf, sizeof(buf),
                      "syscall %d denied: missing capability 0x%x",
                      static_cast<int>(num), static_cast<unsigned>(need));
        qcos::emit_log(1, buf);
        return false;
    }
    return true;
}

std::int32_t rvdl_sys_call(std::int32_t num, std::int32_t a0, std::int32_t a1, std::int32_t a2)
{
    if (!qcos_require(num))
        return qcos::QCOS_EPERM;

    switch (num)
    {
    case qcos::SYS_LOG:
        (void)a0; (void)a1; (void)a2;
        return qcos::QCOS_OK;

    case qcos::SYS_YIELD:
        if (qcos::hooks().yield_fn)
            qcos::hooks().yield_fn();
        else
            std::this_thread::yield();
        return qcos::QCOS_OK;

    case qcos::SYS_EXIT:
        if (qcos::hooks().exit_fn)
        {
            qcos::hooks().exit_fn(a0);
            return qcos::QCOS_OK;
        }
        // hosted 模式：直接 std::exit 会杀掉守护进程，故仅记录并忽略。
        qcos::emit_log(1, "SYS_EXIT ignored: no task exit hook (hosted mode)");
        return qcos::QCOS_OK;

    case qcos::SYS_TIME:
        return static_cast<std::int32_t>(qcos::monotonic_millis() & 0x7FFFFFFFl);

    case qcos::SYS_QPU_SUBMIT:
        if (!qcos::hooks().qpu_submit)
            return qcos::QCOS_ENOSYS;
        return qcos::hooks().qpu_submit(static_cast<std::uint64_t>(a0),
                                        static_cast<std::uint32_t>(a1),
                                        static_cast<std::uint32_t>(a2),
                                        nullptr);

    case qcos::SYS_QPU_AWAIT:
        if (!qcos::hooks().qpu_await)
            return qcos::QCOS_ENOSYS;
        return qcos::hooks().qpu_await(static_cast<std::uint64_t>(a0),
                                       static_cast<std::uint32_t>(a1));

    case qcos::SYS_IRQ_ACK:
        if (!qcos::hooks().irq_ack)
            return qcos::QCOS_ENOSYS;
        qcos::hooks().irq_ack(static_cast<std::uint32_t>(a0));
        return qcos::QCOS_OK;

    case qcos::SYS_PANIC:
        if (!qcos::hooks().panic_fn)
            return qcos::QCOS_ENOSYS;
        qcos::hooks().panic_fn("SYS_PANIC invoked from guest code");
        return qcos::QCOS_OK;

    default:
        // SYS_MAP / SYS_UNMAP / SYS_IRQ_REGISTER 需要 64 位参数或指针，
        // 请改用 rvdl_sys_callp。
        return qcos::QCOS_ENOSYS;
    }
}

double rvdl_sys_calld(std::int32_t num, double a0, double a1)
{
    if (!qcos_require(num))
        return static_cast<double>(qcos::QCOS_EPERM);

    switch (num)
    {
    case qcos::SYS_TIME:
        (void)a0; (void)a1;
        return qcos::monotonic_seconds();

    default:
        return static_cast<double>(qcos::QCOS_ENOSYS);
    }
}

void rvdl_sys_log(std::int32_t level, const char *msg)
{
    if (!qcos_require(qcos::SYS_LOG))
        return;
    qcos::emit_log(static_cast<int>(level), msg);
}

std::int32_t rvdl_sys_logi(std::int32_t level, std::int32_t value)
{
    if (!qcos_require(qcos::SYS_LOG))
        return qcos::QCOS_EPERM;
    char buf[96];
    std::snprintf(buf, sizeof(buf), "%d", static_cast<int>(value));
    qcos::emit_log(static_cast<int>(level), buf);
    return qcos::QCOS_OK;
}

void *rvdl_gc_alloc(std::int64_t size)
{
    if (!qcos_require(qcos::SYS_MAP))
        return nullptr;
    if (size <= 0)
        return nullptr;
    return qcos::tracked_alloc(static_cast<std::size_t>(size));
}

void rvdl_gc_free(void *ptr)
{
    if (!ptr)
        return;
    qcos::tracked_free(ptr);
}

void *rvdl_sys_callp(std::int32_t num, std::int64_t a0, std::int64_t a1, std::int64_t a2)
{
    if (!qcos_require(num))
        return nullptr;

    switch (num)
    {
    case qcos::SYS_MAP:
    {
        if (a0 <= 0)
            return nullptr;
        if (a2 > 1)
        {
            // 显式对齐请求：P0 的 hosted 分配器尚不支持，退回自然对齐。
            qcos::emit_log(1, "SYS_MAP: explicit alignment is reserved for qcos_core");
        }
        (void)a1;
        return qcos::tracked_alloc(static_cast<std::size_t>(a0));
    }

    case qcos::SYS_UNMAP:
    {
        if (a0 == 0)
            return nullptr;
        qcos::tracked_free(reinterpret_cast<void *>(static_cast<std::intptr_t>(a0)));
        return nullptr;
    }

    case qcos::SYS_IRQ_REGISTER:
    {
        if (!qcos::hooks().irq_register)
            return nullptr;
        qcos::hooks().irq_register(static_cast<std::uint32_t>(a0),
                                   reinterpret_cast<void *>(static_cast<std::intptr_t>(a1)));
        (void)a2;
        return nullptr;
    }

    case qcos::SYS_QPU_SUBMIT:
    {
        if (!qcos::hooks().qpu_submit)
            return nullptr;
        qcos::hooks().qpu_submit(static_cast<std::uint64_t>(a0),
                                 static_cast<std::uint32_t>(a1),
                                 static_cast<std::uint32_t>(a2),
                                 nullptr);
        return nullptr;
    }

    default:
        return nullptr;
    }
}

#endif