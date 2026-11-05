// ============================================================================
//  C++ 引导 stub 与 RVDL 内核入口之间的桥接
//
//  boot_x86_64.S 的 32→64 位引导完成后会调用 kernel_main()。该 shim：
//   1. 提供 rvdl_gc_alloc / rvdl_gc_free 的 freestanding 实现（早期内核的简单 bump 分配器）
//   2. 提供 QCOS syscall ABI（rvdl_sys_call / rvdl_sys_calld / rvdl_sys_log /
//      rvdl_sys_logi / rvdl_sys_callp），串口为唯一输出通道
//   3. 转调 RVDL 编译出的 rvdl_main()，结束后停机
// ============================================================================

#include "qcos/Qms.hpp"
#include "qcos_syscall.hpp"

extern "C" int rvdl_main();

// ---- 早期内核位图分配器（支持真正的 free，不再是 bump no-op）----
// 64 KB 堆按 16 B 块管理；qcos_block_len[i] 记录从块 i 开始的分配长度（块数），
// 0 = 空闲。alloc 找连续空闲块，free 依据块头长度清空并复用。
static unsigned char qcos_heap[65536];
static constexpr size_t kBlockSize = 16;
static constexpr size_t kBlockCount = sizeof(qcos_heap) / kBlockSize;
static unsigned char qcos_block_len[kBlockCount] = {0};

extern "C" void* rvdl_gc_alloc(long long n)
{
    if (n <= 0)
        return nullptr;
    size_t blocks = (static_cast<size_t>(n) + kBlockSize - 1) / kBlockSize;
    if (blocks > kBlockCount)
        return nullptr;
    for (size_t i = 0; i + blocks <= kBlockCount; ++i)
    {
        bool free_run = true;
        for (size_t j = 0; j < blocks; ++j)
            if (qcos_block_len[i + j] != 0) { free_run = false; break; }
        if (!free_run)
            continue;
        qcos_block_len[i] = static_cast<unsigned char>(blocks);  // 块头记录长度
        return qcos_heap + i * kBlockSize;
    }
    return nullptr;
}

extern "C" void rvdl_gc_free(void* p)
{
    if (!p)
        return;
    unsigned char* q = static_cast<unsigned char*>(p);
    if (q < qcos_heap || q >= qcos_heap + sizeof(qcos_heap))
        return;                                            // 越界指针，忽略
    size_t idx = static_cast<size_t>(q - qcos_heap) / kBlockSize;
    size_t blocks = qcos_block_len[idx];
    if (blocks == 0 || blocks > kBlockCount)
        return;                                            // 未分配 / 已损坏
    for (size_t j = 0; j < blocks; ++j)
        qcos_block_len[idx + j] = 0;                       // 释放并允许复用
}

// 闭包（lambda）的堆分配由 IR 生成器 emit 为 @malloc；映射到同一 bump 分配器。
extern "C" void* malloc(long long n)
{
    return rvdl_gc_alloc(n);
}

// ---------------------------------------------------------------------------
// rvdl 语言（server/src/ir.ts）声明的内核侧入口。
// 语义与 runtime/include/qhal/QcosSyscall.hpp（hosted）一致，但为 freestanding：
// 串口是唯一输出通道，SYS_TIME 用 TSC 单调时钟，SYS_EXIT 停转核心。
// ---------------------------------------------------------------------------
extern "C" int32_t rvdl_sys_call(int32_t no, int32_t a0, int32_t a1, int32_t a2)
{
    return qcos::sys::call(no, a0, a1, a2);
}

extern "C" double rvdl_sys_calld(int32_t no, double a0, double a1)
{
    return qcos::sys::calld(no, a0, a1);
}

extern "C" void rvdl_sys_log(int32_t level, const char* msg)
{
    qcos::sys::emit_log(level, msg);
}

extern "C" int32_t rvdl_sys_logi(int32_t level, int32_t value)
{
    return qcos::sys::logi(level, value);
}

extern "C" void* rvdl_sys_callp(int32_t no, int64_t a0, int64_t a1, int64_t a2)
{
    return qcos::sys::callp(no, a0, a1, a2);
}

// ---------------------------------------------------------------------------
// 量子服务下沉（QMS）：把谱隙 / 混合界 / 方差收缩时间下沉到裸机内核，
// 供纯 RVDL 内核经 rvdl_qms_gap / rvdl_mix_bound / rvdl_qms_conc 直接调用。
// ---------------------------------------------------------------------------
extern "C" double rvdl_qms_gap(int32_t model, double p, double q)
{
    return qcos::qms::spectral_gap(model, p, q);
}

extern "C" double rvdl_mix_bound(double gap, double n, double eps)
{
    return qcos::qms::mixing_bound(gap, n, eps);
}

extern "C" double rvdl_qms_conc(double gap, double eps)
{
    return qcos::qms::variance_contraction_time(gap, eps);
}

extern "C" void kernel_main()
{
    qcos::sys::com1_init();
    rvdl_main();
    // rvdl_main 返回后停机
    while (true)
    {
        __asm__ volatile("hlt");
    }
}