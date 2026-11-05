#include "qhal/QVM.hpp"
#include <Kokkos_Core.hpp>
#include <cstdio>
#include <dlfcn.h>

static bool gpu_available()
{
    for (const char *n : {"libcudart.so", "libcudart.so.13", "libcudart.so.12", "libcudart.so.11.0"})
        dlopen(n, RTLD_NOW | RTLD_GLOBAL);
    void *cuda = dlopen("libcuda.so.1", RTLD_NOW | RTLD_GLOBAL);
    if (!cuda) return false;
    auto cuInit = reinterpret_cast<int (*)(unsigned int)>(dlsym(cuda, "cuInit"));
    auto cuGetCount = reinterpret_cast<int (*)(int *)>(dlsym(cuda, "cuDeviceGetCount"));
    if (!cuInit || !cuGetCount || cuInit(0) != 0) return false;
    int c = 0;
    return cuGetCount(&c) == 0 && c > 0;
}

static const char *policy_name(int p)
{
    switch (p) {
    case 0: return "LIMDD_Compressed";
    case 1: return "Polyhedral_Graph";
    case 2: return "Dense_StateVector";
    case 3: return "Tensor_Network_MPS";
    default: return "?";
    }
}

int main()
{
    const bool gpu = gpu_available();
    rvdl::set_gpu_backend_available(gpu);
    Kokkos::initialize();
    std::printf("GPU available: %s\n", gpu ? "yes" : "no");

    int fails = 0;

    // 策略路由 + 数值正确性：apply_x(0) 翻转 |0⟩→|1⟩，measure(0) 应返回 1。
    // n ≤ 16 → host 循环；17-20 → device 批处理；21+ → MPS（方案2）。
    for (size_t n : {16u, 18u, 20u, 22u, 24u, 28u})
    {
        qhal::QVM vm;
        vm.allocate_qubits(n);
        const int p_alloc = static_cast<int>(vm.get_current_policy());
        vm.apply_rz(0, 0.37);   // 触发魔法态 → Dense/MPS
        const int p_magic = static_cast<int>(vm.get_current_policy());
        vm.apply_x(0);          // X 门（device 或 host 或 MPS 路径）
        const int m = vm.measure(0);
        const bool ok = (m == 1);
        if (!ok) ++fails;
        std::printf("  n=%-3zu alloc=%s(%-2d) magic=%s(%-2d)  apply_x(0)->measure=%d %s\n",
                    n, policy_name(p_alloc), p_alloc, policy_name(p_magic), p_magic,
                    m, ok ? "OK" : "FAIL");
    }

    std::printf("\n%s（失败 %d）\n", fails == 0 ? "策略路由 + 数值正确性：全部通过" : "有失败", fails);
    Kokkos::finalize();
    return fails == 0 ? 0 : 1;
}