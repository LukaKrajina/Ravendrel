// 单元测试入口:注册在多个 TU 中的测试在此统一运行。
#include "test_framework.hpp"
#include "Kokkos_Core.hpp"

int main() {
    int rc = qtest::run_all();
    // 若测试过程中经 PhysicsKernel::integrate_kokkos 惰性初始化了 Kokkos(CUDA 后端),
    // 必须在进程退出前显式 finalize,否则 CUDA driver 在静态析构阶段被卸载时崩溃
    // (cudaErrorCudartUnloading → std::terminate)。
    if (Kokkos::is_initialized())
        Kokkos::finalize();
    return rc;
}
