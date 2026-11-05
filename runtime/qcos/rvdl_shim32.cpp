// ============================================================================
//  32 位引导桥接（QEMU Multiboot1 直启演示用）
//
//  与 rvdl_shim.cpp 同理：提供 rvdl_gc_alloc / rvdl_gc_free 的 freestanding 实现、
//  QCOS syscall ABI，并转调 RVDL 的 rvdl_main()。
//  32 位版本用 32 位指针与32 位 bump 游标。
// ============================================================================

#include "qcos_syscall.hpp"

extern "C" int rvdl_main();

// 与 rvdl_shim.cpp 同款的位图分配器：支持真正的 free（复用已释放块）。
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
        qcos_block_len[i] = static_cast<unsigned char>(blocks);
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
        return;
    size_t idx = static_cast<size_t>(q - qcos_heap) / kBlockSize;
    size_t blocks = qcos_block_len[idx];
    if (blocks == 0 || blocks > kBlockCount)
        return;
    for (size_t j = 0; j < blocks; ++j)
        qcos_block_len[idx + j] = 0;
}

// 闭包（lambda）的堆分配由 IR 生成器 emit 为 @malloc；映射到同一 bump 分配器。
extern "C" void* malloc(long long n)
{
    return rvdl_gc_alloc(n);
}

// ---- QCOS syscall ABI（与 rvdl_shim.cpp 一致，实现见 qcos_syscall.hpp）----
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

extern "C" void kernel_main()
{
    qcos::sys::com1_init();
    rvdl_main();
    while (true)
    {
        __asm__ volatile("hlt");
    }
}