// ============================================================================
//  宿主机冒烟测试
//
//  这是 hosted 程序（用 <cstdio>），但只测试 include/qcos/*.hpp 里
//  freestanding 头文件的**逻辑**。真正的 freestanding 编译由 qcos_core 保证。
//
//  注意：千万别用 assert() —— NDEBUG(Release) 下 assert 连同表达式一起被编译掉，
//  导致测试空转。这里用始终生效的 CHECK 宏。
// ============================================================================

#include "qcos/Arch.hpp"
#include "qcos/LibcSubset.hpp"
#include "qcos/Spinlock.hpp"
#include "qcos/Pmm.hpp"
#include "qcos/DragonPmm.hpp"
#include "qcos/Sched.hpp"
#include "qcos/Vmm.hpp"
#include "qcos/Vmm_x86_64.hpp"

#include <cstdio>

#define CHECK(expr)                                                        \
    do {                                                                   \
        if (!(expr)) {                                                     \
            std::printf("CHECK FAILED: %s (line %d)\n", #expr, __LINE__); \
            return 1;                                                      \
        }                                                                  \
    } while (0)

static int g_counter = 0;

static void inc1(void*) { g_counter += 1; }
static void inc2(void*) { g_counter += 2; }

int main()
{
    using namespace qcos;

    // ---- LibcSubset ----
    {
        char buf[16];
        memset(buf, 0, sizeof(buf));
        memcpy(buf, "hello", 6);
        CHECK(strlen(buf) == 5);
        CHECK(memcmp(buf, "hello", 5) == 0);
        std::printf("libc subset OK\n");
    }

    // ---- BitmapPmm ----
    {
        static u8 region[PAGE_SIZE * 32];   // 32 页
        BitmapPmm pmm;
        pmm.init(reinterpret_cast<u64>(region), sizeof(region));
        CHECK(pmm.total_page_count() == 32);
        CHECK(pmm.free_page_count() == 31);   // 位图自身占 1 页

        const u64 p = pmm.alloc(3);
        CHECK(p != 0);
        CHECK(pmm.free_page_count() == 28);

        pmm.free(p, 3);
        CHECK(pmm.free_page_count() == 31);
        std::printf("pmm OK\n");
    }

    // ---- Scheduler ----
    {
        Scheduler sched;
        CHECK(sched.add(inc1, nullptr));
        CHECK(sched.add(inc2, nullptr));
        CHECK(sched.task_count() == 2);
        sched.run_all();
        CHECK(g_counter == 3);
        std::printf("sched counter=%d OK\n", g_counter);
    }

    // ---- Spinlock ----
    {
        Spinlock lk;
        CHECK(lk.try_lock());
        lk.unlock();
        CHECK(lk.try_lock());
        lk.unlock();
        std::printf("spinlock OK\n");
    }

    // ---- VmmX86_64：map/translate/unmap（不 activate，以免改宿主 CR3）----
    {
        alignas(4096) static u8 pml4_buf[PAGE_SIZE];
        alignas(4096) static u8 pool_buf[PAGE_SIZE * 16];   // 16 页页表池

        VmmX86_64 vmm(reinterpret_cast<u64>(pml4_buf),
                      reinterpret_cast<u64>(pool_buf),
                      sizeof(pool_buf));

        const u64 vaddr = 0xFFFF800000000000ull;   // 高半区虚拟地址
        const u64 paddr = 0x100000ull;             // 1 MB 物理地址
        PageAttrs attrs;
        attrs.present      = true;
        attrs.writable     = true;
        attrs.no_exec      = false;
        attrs.priv_no_exec = false;   // 完全可执行（用户态 + 特权态）

        CHECK(vmm.map(vaddr, paddr, attrs));
        CHECK(vmm.translate(vaddr) == paddr);
        CHECK(vmm.translate(vaddr + 100) == paddr + 100);   // 页内偏移
        CHECK(vmm.unmap(vaddr));
        CHECK(vmm.translate(vaddr) == 0);
        std::printf("vmm OK\n");
    }

    // ---- DragonPmm（驯龙系统）单核：无锁快路径 + buddy merge ----
    {
        alignas(4096) static u8 dragon_region[PAGE_SIZE * 256];
        DragonPmm<10> dpmm;
        dpmm.init(reinterpret_cast<u64>(dragon_region), sizeof(dragon_region)); // 单核
        CHECK(dpmm.total_page_count() == 256);
        CHECK(dpmm.free_page_count() == 256);
        CHECK(dpmm.core_count() == 1);

        const u64 x = dpmm.alloc(1);          // order0
        const u64 y = dpmm.alloc(1);          // order0，与 x 互为 buddy
        CHECK(x != 0 && y != 0);
        CHECK(dpmm.free_page_count() == 254);

        const u64 z = dpmm.alloc(3);          // 向上取整到 order2（4 页）
        CHECK(z != 0);
        CHECK(dpmm.free_page_count() == 250);

        dpmm.free(x, 1);
        dpmm.free(y, 1);                      // 与 x 合并
        dpmm.free(z, 3);
        CHECK(dpmm.free_page_count() == 256);

        // 合并后应能一次性分配整段（order8 = 256 页）
        const u64 big = dpmm.alloc(256);
        CHECK(big != 0);
        dpmm.free(big, 256);
        CHECK(dpmm.free_page_count() == 256);

        // 超出容量应失败
        CHECK(dpmm.alloc(257) == 0);
        std::printf("dragon pmm (single-core) OK\n");
    }

    // ---- DragonPmm（驯龙系统）多核：per-CPU 缓存 + 中央池 ----
    {
        alignas(4096) static u8 dragon_region2[PAGE_SIZE * 256];
        static DragonPmm<10>::CpuCache caches[4];   // 4 核 per-CPU 缓存
        DragonPmm<10> dpmm;
        dpmm.init(reinterpret_cast<u64>(dragon_region2), sizeof(dragon_region2), 4, caches);
        CHECK(dpmm.core_count() == 4);
        CHECK(dpmm.free_page_count() == 256);

        // order-0 走各自 per-CPU 私有缓存（无锁）
        const u64 a0 = dpmm.alloc(1, 0);
        const u64 a1 = dpmm.alloc(1, 1);
        const u64 a2 = dpmm.alloc(1, 2);
        const u64 a3 = dpmm.alloc(1, 3);
        CHECK(a0 && a1 && a2 && a3);
        CHECK(dpmm.free_page_count() == 252);

        // order>=1 走中央池（加票据锁）
        const u64 mid = dpmm.alloc(8);        // order3
        CHECK(mid != 0);
        CHECK(dpmm.free_page_count() == 244);

        dpmm.free(a0, 1, 0);
        dpmm.free(a1, 1, 1);
        dpmm.free(a2, 1, 2);
        dpmm.free(a3, 1, 3);
        dpmm.free(mid, 8);
        CHECK(dpmm.free_page_count() == 256);

        // TicketSpinlock 公平性冒烟
        TicketSpinlock tk;
        CHECK(tk.try_lock());
        tk.unlock();
        CHECK(tk.try_lock());
        tk.unlock();
        std::printf("dragon pmm (multi-core) OK\n");
    }

    // ---- DragonPmm 多核但未提供缓存数组 → 安全退化单核 ----
    {
        alignas(4096) static u8 dragon_region3[PAGE_SIZE * 64];
        DragonPmm<10> dpmm;
        dpmm.init(reinterpret_cast<u64>(dragon_region3), sizeof(dragon_region3), 8); // 无缓存数组
        CHECK(dpmm.core_count() == 1);          // 已退化为单核
        const u64 p = dpmm.alloc(1);
        CHECK(p != 0);
        dpmm.free(p, 1);
        CHECK(dpmm.free_page_count() == 64);
        std::printf("dragon pmm (fallback single-core) OK\n");
    }

    std::printf("ALL QCOS SMOKE TESTS PASSED\n");
    return 0;
}