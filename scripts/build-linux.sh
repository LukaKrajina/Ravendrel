#!/usr/bin/env bash
# ============================================================================
# Linux 构建脚本
# ----------------------------------------------------------------------------
# 用法：
#   ./scripts/build-linux.sh [--cuda] [build-dir]
#
#   默认        ：CPU 版（系统 Kokkos + clang++）
#   --cuda      ：CUDA 版（Kokkos CUDA 后端 + nvcc_wrapper + /opt/cuda）
#
# 前置依赖（系统级，需自行安装）：
#   - cmake >= 3.20, ninja, clang / gcc（C++20）
#   - LLVM 开发包（提供 LLVMConfig.cmake）
#   - Vulkan SDK（提供 find_package(Vulkan)）
#   - Kokkos（CPU 版：系统包 /usr/lib/cmake/Kokkos；
#             CUDA 版：安装到 /usr/local，见 --cuda 说明）
#   - GLFW（系统包 glfw）
#
# --cuda 模式额外前置：
#   - CUDA Toolkit 已安装（/opt/cuda，nvcc 可用）
#   - Kokkos 已用 CUDA 后端编译并安装到 /usr/local（lib/cmake/Kokkos + bin/nvcc_wrapper）
#     编译方法（在 kokkos 源码目录）：
#       export PATH=/opt/cuda/bin:$PATH
#       cmake -B build -S . -DBUILD_SHARED_LIBS=ON -DCMAKE_CXX_COMPILER=<kokkos>/bin/nvcc_wrapper \
#             -DKokkos_ENABLE_CUDA=ON -DKokkos_ARCH_AMPERE86=ON
#       sudo cmake --build build --target install --prefix /usr/local
#       sudo ldconfig
# ============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
RUNTIME_DIR="$(cd "$SCRIPT_DIR/../runtime" && pwd)"

USE_CUDA=0
BUILD_DIR="$RUNTIME_DIR/build-linux"

# 解析参数
while [[ $# -gt 0 ]]; do
    case "$1" in
        --cuda) USE_CUDA=1; shift ;;
        *) BUILD_DIR="$1"; shift ;;
    esac
done

# ---------------------------------------------------------------------------
# 选择编译器与 Kokkos
# ---------------------------------------------------------------------------
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

if [ "$USE_CUDA" -eq 1 ]; then
    export PATH="/opt/cuda/bin:$PATH"
    # nvcc_wrapper 查找：环境变量 NVCC_WRAPPER > .deps/kokkos-install/bin > PATH
    if [ -z "${NVCC_WRAPPER:-}" ]; then
        NVCC_WRAPPER="$ROOT_DIR/.deps/kokkos-install/bin/nvcc_wrapper"
        [ -x "$NVCC_WRAPPER" ] || NVCC_WRAPPER="$(command -v nvcc_wrapper || true)"
    fi
    CXX_COMPILER="$NVCC_WRAPPER"
    if [ -z "$CXX_COMPILER" ] || [ ! -x "$CXX_COMPILER" ]; then
        echo "[build-linux] 未找到 nvcc_wrapper，请确认 Kokkos CUDA 版已安装（.deps/kokkos-install 或 /usr/local）"
        exit 1
    fi
    # Kokkos CUDA 查找：环境变量 KOKKOS_DIR > .deps/kokkos-install > /usr/local
    if [ -z "${KOKKOS_DIR:-}" ]; then
        if [ -d "$ROOT_DIR/.deps/kokkos-install/lib/cmake/Kokkos" ]; then
            KOKKOS_DIR="$ROOT_DIR/.deps/kokkos-install/lib/cmake/Kokkos"
        else
            KOKKOS_DIR="/usr/local/lib/cmake/Kokkos"
        fi
    fi
    echo "[build-linux] 模式：CUDA（nvcc_wrapper=$CXX_COMPILER + Kokkos @ $KOKKOS_DIR）"
else
    CXX_COMPILER="clang++"
    # Kokkos 查找顺序：环境变量 KOKKOS_DIR > ~/.local 用户目录 > 系统路径
    if [ -z "${KOKKOS_DIR:-}" ]; then
        if [ -d "$HOME/.local/kokkos-install/lib/cmake/Kokkos" ]; then
            KOKKOS_DIR="$HOME/.local/kokkos-install/lib/cmake/Kokkos"
        else
            KOKKOS_DIR="/usr/lib/cmake/Kokkos"
        fi
    fi
    echo "[build-linux] 模式：CPU（clang++ + Kokkos @ $KOKKOS_DIR）"
fi

# ---------------------------------------------------------------------------
# CMake 配置
# ---------------------------------------------------------------------------
# GLFW / Vulkan 头文件：优先项目自带（.deps），避免依赖系统包未安装导致
# "cannot find -lglfw" / "Could NOT find Vulkan" 而中断构建。
EXTRA_LIB_DIRS=""
if [ -d "$ROOT_DIR/.deps/glfw-install/lib" ]; then
    EXTRA_LIB_DIRS="$ROOT_DIR/.deps/glfw-install/lib"
    # CMakeLists 里是裸名 `glfw`（生成 -lglfw），链接器仍需 -L / LIBRARY_PATH。
    # CMAKE_LIBRARY_PATH 只影响 find_library，对裸名链接无效，故导出 LIBRARY_PATH。
    export LIBRARY_PATH="$EXTRA_LIB_DIRS${LIBRARY_PATH:+:$LIBRARY_PATH}"
fi
if [ -d "$ROOT_DIR/.deps/Vulkan-Headers/include" ]; then
    export VULKAN_SDK="$ROOT_DIR/.deps/Vulkan-Headers"
fi

echo "[build-linux] CMake 配置..."
cmake -B "$BUILD_DIR" -S "$RUNTIME_DIR" \
    -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER=gcc \
    -DCMAKE_CXX_COMPILER="$CXX_COMPILER" \
    -DKokkos_DIR="$KOKKOS_DIR" \
    ${EXTRA_LIB_DIRS:+-DCMAKE_LIBRARY_PATH="$EXTRA_LIB_DIRS"}

# ---------------------------------------------------------------------------
# 构建
# ---------------------------------------------------------------------------
echo "[build-linux] 构建..."
# 打包 vsix 只需 rvdl_rt + runtime 两个目标；transmitter/qvm_visualizer 依赖
# Qt/GLFW 显示相关，可能失败且不在 vsix 范围内，故不在此构建。
cmake --build "$BUILD_DIR" --config Release --target rvdl_rt runtime --parallel

echo ""
echo "[build-linux] 完成。产物："
ls -lh "$BUILD_DIR"/runtime "$BUILD_DIR"/librvdl_rt.so 2>/dev/null || true
ls -lh "$BUILD_DIR"/transmitter "$BUILD_DIR"/qvm_visualizer 2>/dev/null || true
ls -lh "$BUILD_DIR"/rvdlRSP/rvdlRSP 2>/dev/null || true
echo ""
echo "  下一步：./scripts/install-linux.sh <version> $BUILD_DIR 将产物安装到 ~/.ravendrel"