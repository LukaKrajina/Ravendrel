#!/usr/bin/env bash
# ============================================================================
# build-linux-wsl.sh — 在 WSL(Ubuntu) 内构建「自包含」Linux 运行时，并收集到 bin/linux-x64
#
# 产物（随 vsix 一起分发，目标机无需安装任何第三方库）：
#   bin/linux-x64/runtime          执行壳（socket daemon + stdin 交互）
#   bin/linux-x64/librvdl_rt.so   量子核心（内含**静态** LLVM + **静态** Kokkos）
#   bin/linux-x64/libvulkan.so.1, libglfw.so.3, libomp.so.5
#
# 关键点（踩过的坑，改动见此）：
#   * LLVM 必须**自建静态版**：发行版的 libLLVM.so 有 132 MB，且 Ubuntu 的静态
#     组件库带全部后端（产物 139 MB）。这里只编 X86/NVPTX/AArch64 三个后端。
#   * Kokkos 静态 + PIC，直接并进 librvdl_rt.so。
#   * glibc 旧发行版兼容由 runtime/src/glibc_compat.cpp 兜住（新符号版本
#     fmod/acosf/sqrtf/__isoc23_strto*/arc4random → 回绑 @GLIBC_2.2.5 或自实现），
#     并把 `-static-libstdc++` 打开消除 GLIBCXX 依赖（libgcc 保持动态）。
#
# 前置（WSL 内一次性安装）：
#   sudo apt-get install -y build-essential cmake ninja-build clang lld llvm-dev \
#        libomp-dev libvulkan-dev glslc libglfw3-dev zlib1g-dev libzstd-dev libtinfo-dev
#
# 用法（WSL 内，仓库根目录）：bash scripts/build-linux-wsl.sh
#   LLVM_TARBALL=/mnt/c/src/llvm.tar.gz   # 可选：复用本地的 llvm-project 源码包
#   STRIP=1                               # 默认 1：strip 后再收集（体积减半）
# ============================================================================
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LLVM_PREFIX="${LLVM_PREFIX:-$HOME/.local/llvm-static}"
KOKKOS_PREFIX="${KOKKOS_PREFIX:-$HOME/.local/kokkos-install-static}"
WORK="${WORK:-$HOME/.cache/ravendrel-linux-build}"
JOBS="${JOBS:-$(nproc)}"
STRIP="${STRIP:-1}"

# 注意：构建期用 root/普通用户都行，但 LLVM_POST 之类前缀要可写
mkdir -p "$WORK"

echo "== [1/5] 依赖检查 =="
for t in cmake ninja clang clang++ gcc; do
    command -v "$t" >/dev/null || { echo "缺少 $t（见脚本头部 apt 安装行）"; exit 1; }
done
LLVM_DIR="$(ls -d /usr/lib/llvm-*/lib/cmake/llvm 2>/dev/null | head -1 || true)"
[ -n "$LLVM_DIR" ] || { echo "未找到 llvm-dev 的 LLVMConfig.cmake"; exit 1; }
echo "  系统 LLVM(_DIR)=$LLVM_DIR"

echo "== [2/5] 自建精简静态 LLVM（仅 X86;NVPTX;AArch64）=="
if [ ! -f "$LLVM_PREFIX/lib/cmake/llvm/LLVMConfig.cmake" ]; then
    cd "$WORK"
    if [ -n "${LLVM_TARBALL:-}" ] && [ -f "$LLVM_TARBALL" ]; then
        cp -f "$LLVM_TARBALL" llvm.tar.gz
    else
        curl -sSL -o llvm.tar.gz https://codeload.github.com/llvm/llvm-project/tar.gz/refs/tags/llvmorg-23.1.2
    fi
    rm -rf llvm-src llvm-build && mkdir -p llvm-src
    tar -xzf llvm.tar.gz -C llvm-src --strip-components=1
    cmake -S llvm-src/llvm -B llvm-build -G Ninja \
        -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
        -DCMAKE_INSTALL_PREFIX="$LLVM_PREFIX" \
        -DLLVM_TARGETS_TO_BUILD="X86;NVPTX;AArch64" \
        -DLLVM_INCLUDE_TESTS=OFF -DLLVM_INCLUDE_EXAMPLES=OFF -DLLVM_INCLUDE_BENCHMARKS=OFF \
        -DLLVM_ENABLE_ZLIB=OFF -DLLVM_ENABLE_ZSTD=OFF -DLLVM_ENABLE_LIBXML2=OFF \
        -DLLVM_ENABLE_RTTI=ON -DLLVM_ENABLE_EH=ON -DLLVM_OPTIMIZED_TABLEGEN=ON \
        -DLLVM_BUILD_LLVM_DYLIB=OFF
    cmake --build llvm-build --parallel "$JOBS"
    cmake --install llvm-build
else
    echo "  已存在：$LLVM_PREFIX"
fi

echo "== [3/5] 静态 Kokkos（Serial + OpenMP + PIC）=="
if [ ! -f "$KOKKOS_PREFIX/lib/cmake/Kokkos/KokkosConfig.cmake" ]; then
    cd "$WORK"
    curl -sSL -o kokkos.tar.gz https://codeload.github.com/kokkos/kokkos/tar.gz/refs/heads/master
    rm -rf kokkos-src kokkos-build && mkdir -p kokkos-src
    tar -xzf kokkos.tar.gz -C kokkos-src --strip-components=1
    cmake -S kokkos-src -B kokkos-build -G Ninja \
        -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=clang++ \
        -DBUILD_SHARED_LIBS=OFF -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
        -DCMAKE_INSTALL_PREFIX="$KOKKOS_PREFIX" \
        -DKokkos_ENABLE_SERIAL=ON -DKokkos_ENABLE_OPENMP=ON \
        -DKokkos_ENABLE_TESTS=OFF -DKokkos_ENABLE_EXAMPLES=OFF -DKokkos_ENABLE_BENCHMARKS=OFF
    cmake --build kokkos-build --parallel "$JOBS"
    cmake --install kokkos-build
else
    echo "  已存在：$KOKKOS_PREFIX"
fi

echo "== [4/5] 构建 rvdl_rt + runtime =="
BUILD="$REPO/runtime/build-linux"
cmake -S "$REPO/runtime" -B "$BUILD" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=clang++ \
    -DLLVM_DIR="$LLVM_PREFIX/lib/cmake/llvm" \
    -DKokkos_DIR="$KOKKOS_PREFIX/lib/cmake/Kokkos"
cmake --build "$BUILD" --target rvdl_rt runtime --parallel "$JOBS"

echo "== [5/5] 收集到 bin/linux-x64 =="
DST="$REPO/bin/linux-x64"
rm -rf "$DST" && mkdir -p "$DST"
install -m 0755 "$BUILD/runtime" "$DST/runtime"
install -m 0755 "$BUILD/librvdl_rt.so" "$DST/librvdl_rt.so"
if [ "$STRIP" = "1" ]; then
    strip --strip-unneeded "$DST/runtime" "$DST/librvdl_rt.so"
fi
# 只依赖 libc/libm 的三个库（GLFW 3.4 用 dlopen 载入 X11/Wayland）
for l in libvulkan.so.1 libglfw.so.3 libomp.so.5; do
    cp -Lf "/usr/lib/x86_64-linux-gnu/$l" "$DST/$l"
done

echo "== 校验：>GLIBC_2.34 的符号需求（应为空）=="
for f in "$DST/runtime" "$DST/librvdl_rt.so"; do
    printf '  %-16s : ' "$(basename "$f")"
    readelf --version-info "$f" | grep -o 'GLIBC_2\.\(3[5-9]\|4[0-9]\)' | sort -u | tr '\n' ' '
    echo
done
echo "== 产物 =="
ls -lh "$DST" | sed 's/^/  /'