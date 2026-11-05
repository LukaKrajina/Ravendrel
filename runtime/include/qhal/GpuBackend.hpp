#pragma once
// ============================================================================
//  GPU 后端与 CPU 向量扩展的检测与抽象
//
//  国产化加速适配：
//    - NVIDIA CUDA   nvcc：__CUDACC__ / __CUDA_ARCH__ / __NVCC__
//    - 摩尔线程 MUSA mcc：__MUSA__ / __MUSA_ARCH__（源码级兼容 CUDA）
//    - AMD HIP        hipcc：__HIPCC__ / __HIP_DEVICE_COMPILE__
//
//    - 龙芯 LoongArch LSX（128 位）/ LASX（256 位）向量扩展：
//        __loongarch_sx / __loongarch_asx（类比 SSE / AVX2）
//
//  摩尔线程 MUSA 与 CUDA 源码级兼容（可用musify 工具迁移）
// 所以我想二者统一为「类 CUDA」编程模型（RVDL_GPU_NVIDIA_LIKE），这样更好，除非它们发生变化，请随时更新。目前上层代码无需区分。
// ============================================================================

// RVDL_RT_API：Windows 下 DLL 导出/导入修饰（train_cli 等外部目标链 rvdl_rt.lib 时
// 需要该符号被导出，否则 lld-link 报 undefined symbol: set_gpu_backend_available）。
#include "Export.hpp"

// ---------------------------------------------------------------------------
// GPU 后端检测
// ---------------------------------------------------------------------------
#if defined(__CUDACC__) || defined(__CUDA_ARCH__) || defined(__NVCC__)
  #define RVDL_GPU_CUDA 1
#endif

#if defined(__MUSA__) || defined(__MUSA_ARCH__) || defined(MUSA) || defined(__MCC__)
  #define RVDL_GPU_MUSA 1
#endif

#if defined(__HIPCC__) || defined(__HIP_DEVICE_COMPILE__) || defined(__HIP_PLATFORM_AMD__)
  #define RVDL_GPU_HIP 1
#endif

// CUDA 与 MUSA 源码级兼容，统一为「类 CUDA」编程模型
#if defined(RVDL_GPU_CUDA) || defined(RVDL_GPU_MUSA)
  #define RVDL_GPU_NVIDIA_LIKE 1
#endif

// ---------------------------------------------------------------------------
// CPU 向量扩展检测（x86 AVX 优先，龙芯 LSX/LASX 次之）
// ---------------------------------------------------------------------------
#if defined(__AVX512F__)
  #define RVDL_SIMD_AVX512 1
#elif defined(__AVX2__)
  #define RVDL_SIMD_AVX2 1
#elif defined(__loongarch_asx)
  #define RVDL_SIMD_LASX 1       // 龙芯 256 位向量（类比 AVX2）
#elif defined(__loongarch_sx)
  #define RVDL_SIMD_LSX 1        // 龙芯 128 位向量（类比 SSE）
#endif

// ---------------------------------------------------------------------------
// 设备函数属性（CUDA / MUSA / HIP 用 __device__/__global__；CPU 编译展开为空）
// ---------------------------------------------------------------------------
#if defined(RVDL_GPU_CUDA) || defined(RVDL_GPU_MUSA) || defined(RVDL_GPU_HIP)
  #define RVDL_GPU_DEVICE      __device__
  #define RVDL_GPU_GLOBAL      __global__
  #define RVDL_GPU_HOST_DEVICE __host__ __device__
#else
  #define RVDL_GPU_DEVICE
  #define RVDL_GPU_GLOBAL
  #define RVDL_GPU_HOST_DEVICE
#endif

// ---------------------------------------------------------------------------
// 运行时 GPU 后端可用性（可选 GPU / dlopen 延迟加载）
//
//   Kokkos 以 CUDA 后端编译，但**不硬链** libcuda.so.1 / libcudart.so.*（见
//   Kokkos 的 FindTPLCUDA.cmake 补丁 + --allow-shlib-undefined）。运行时启动时
//   由 runtime_api.cpp 通过 dlopen("libcuda.so.1") / LoadLibrary("nvcuda.dll")
//   探测 NVIDIA 驱动是否可用，并用 cuDeviceGetCount 确认存在 GPU，随后把结果
//   写入该标志。Ga.hpp 据此在「CUDA 后端」与「CPU（OpenMP/Serial）后端」之间
//   派发，从而做到「有 GPU 加速、无 GPU 也能跑」。
// ---------------------------------------------------------------------------
namespace rvdl {
    RVDL_RT_API bool gpu_backend_available() noexcept;
    RVDL_RT_API void set_gpu_backend_available(bool available) noexcept;
}