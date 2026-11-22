# ============================================================================
# Android 轻量运行时（librvdl_rt.so）交叉编译
#
# 用法（在 Android SDK/NDK 环境变量就绪后）：
#   cmake -B build-android -S . -G Ninja \
#     -DCMAKE_TOOLCHAIN_FILE=$ANDROID_HOME/ndk/26.1.10909125/build/cmake/android.toolchain.cmake \
#     -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-21 \
#     -DCMAKE_BUILD_TYPE=Release \
#     -DKokkos_DIR=<android-kokkos>/lib/cmake/Kokkos
#   cmake --build build-android
# ============================================================================
cmake_minimum_required(VERSION 3.16)
project(rvdl_rt_android LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 纯 C++ 量子核心（无 LLVM JIT）
add_library(rvdl_rt SHARED runtime_android.cpp qlm_android.cpp)

target_include_directories(rvdl_rt PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include)

# QLM 推理层后端选择：
#   · Kokkos 可用 → 用与上游一致的 QVM（QLM_USE_QVM / RVDL_HAS_KOKKOS）；
#   · 没提供 Android-Kokkos → 用零依赖 BasicQVM（纯 CPU，手机端推荐）。
find_package(Kokkos QUIET)
if(Kokkos_FOUND)
    target_link_libraries(rvdl_rt PRIVATE Kokkos::kokkos)
    target_compile_definitions(rvdl_rt PRIVATE RVDL_HAS_KOKKOS=1 QLM_USE_QVM=1)
    message(STATUS "rvdl_rt android: Kokkos found -> QVM backend")
else()
    message(STATUS "rvdl_rt android: Kokkos not found -> BasicQVM (pure CPU)")
endif()

# Android 平台库（log / android）
target_link_libraries(rvdl_rt PRIVATE log android)

# rvdl_main / rvdl_topology_entry 由 AOT 编译出的 librvdl_main.so 在运行时提供。
# NDK 默认 --no-undefined（= --unresolved-symbols=report-all）会拒绝这类符号；
# 用 ignore-all 覆盖，允许这些符号在运行时从 librvdl_main.so 动态解析。
target_link_options(rvdl_rt PRIVATE -Wl,--unresolved-symbols=ignore-all)