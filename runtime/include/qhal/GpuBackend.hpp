#pragma once

#include "Export.hpp"

#if defined(__CUDACC__) || defined(__CUDA_ARCH__) || defined(__NVCC__)
  #define RVDL_GPU_CUDA 1
#endif

#if defined(__MUSA__) || defined(__MUSA_ARCH__) || defined(MUSA) || defined(__MCC__)
  #define RVDL_GPU_MUSA 1
#endif

#if defined(__HIPCC__) || defined(__HIP_DEVICE_COMPILE__) || defined(__HIP_PLATFORM_AMD__)
  #define RVDL_GPU_HIP 1
#endif

#if defined(RVDL_GPU_CUDA) || defined(RVDL_GPU_MUSA)
  #define RVDL_GPU_NVIDIA_LIKE 1
#endif

#if defined(__AVX512F__)
  #define RVDL_SIMD_AVX512 1
#elif defined(__AVX2__)
  #define RVDL_SIMD_AVX2 1
#elif defined(__loongarch_asx)
  #define RVDL_SIMD_LASX 1
#elif defined(__loongarch_sx)
  #define RVDL_SIMD_LSX 1
#endif

#if defined(RVDL_GPU_CUDA) || defined(RVDL_GPU_MUSA) || defined(RVDL_GPU_HIP)
  #define RVDL_GPU_DEVICE      __device__
  #define RVDL_GPU_GLOBAL      __global__
  #define RVDL_GPU_HOST_DEVICE __host__ __device__
#else
  #define RVDL_GPU_DEVICE
  #define RVDL_GPU_GLOBAL
  #define RVDL_GPU_HOST_DEVICE
#endif

namespace rvdl {
    RVDL_RT_API bool gpu_backend_available() noexcept;
    RVDL_RT_API void set_gpu_backend_available(bool available) noexcept;
}