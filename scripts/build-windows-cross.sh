#!/usr/bin/env bash
# ============================================================================
# build-windows-cross.sh — 在 Linux 上交叉编译 Windows (x86_64) runtime
#
# 产物：runtime/build-win/runtime.exe + rvdl_rt.dll（+ 依赖 DLL）
#   后续用 scripts/package-vsix.sh --win 复制到 bin/ 并打包 Windows vsix。
#
# 前置（需自行准备）：
#   - MinGW-w64 工具链：x86_64-w64-mingw32-gcc / g++ / windres
#   - 交叉编译依赖（LLVM / Kokkos / Vulkan / zlib / zstd），
#     路径通过 MINGW_DEPS_ROOT 提供（见 mingw-w64-toolchain.cmake 头部约定）
#
# 用法：
#   MINGW_DEPS_ROOT=/path/to/mingw-deps ./scripts/build-windows-cross.sh
# ============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(dirname "$SCRIPT_DIR")"
RUNTIME_DIR="$ROOT/runtime"
BUILD_DIR="${1:-$ROOT/runtime/build-win}"
TOOLCHAIN="$SCRIPT_DIR/mingw-w64-toolchain.cmake"

# ─── 1. 检查 MinGW 工具链 ────────────────────────────────
echo "[build-win] 检查 MinGW-w64 工具链 ..."
for exe in x86_64-w64-mingw32-gcc x86_64-w64-mingw32-g++ x86_64-w64-mingw32-windres; do
    if ! command -v "$exe" >/dev/null 2>&1; then
        echo "[build-win] 错误：未找到 $exe"
        echo "  安装：sudo apt-get install g++-mingw-w64-x86-64 (Debian/Ubuntu)"
        exit 1
    fi
done
echo "  x86_64-w64-mingw32-gcc/g++/windres 就绪"

# ─── 2. 检查交叉编译依赖 ────────────────────────────────
if [ -z "${MINGW_DEPS_ROOT:-}" ]; then
    echo "[build-win] 错误：请设置 MINGW_DEPS_ROOT 指向 MinGW 交叉编译依赖根目录"
    echo "  该目录应包含 llvm/ kokkos/ vulkan/ zlib/ zstd/ 子目录"
    echo "  （详见 scripts/mingw-w64-toolchain.cmake 头部说明）"
    exit 1
fi
for sub in llvm kokkos vulkan zlib zstd; do
    if [ ! -d "$MINGW_DEPS_ROOT/$sub" ]; then
        echo "[build-win] 警告：缺少 $MINGW_DEPS_ROOT/$sub（可能影响链接）"
    fi
done

# ─── 3. CMake 配置（交叉编译）───────────────────────────
echo "[build-win] CMake 配置（toolchain: $TOOLCHAIN）..."
cmake -B "$BUILD_DIR" -S "$RUNTIME_DIR" \
    -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" \
    -DMINGW_DEPS_ROOT="$MINGW_DEPS_ROOT" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_POSITION_INDEPENDENT_CODE=OFF \
    -DRVDL_GPU_BACKEND=OFF

# ─── 4. 构建 rvdl_rt + runtime（打包只需这两个目标）────
echo "[build-win] 构建 rvdl_rt + runtime ..."
cmake --build "$BUILD_DIR" --config Release --target rvdl_rt runtime --parallel

# ─── 5. 收集 MinGW 运行时 DLL 依赖 ────────────────────────
echo ""
echo "[build-win] 收集 MinGW 运行时 DLL ..."
# librvdl_rt.dll / runtime.exe 链接 libgcc/libgomp/libstdc++/libwinpthread，
# 需随 vsix 打包。用 -print-file-name 定位（跨发行版可靠）。
for dll in libgcc_s_seh-1.dll libgomp-1.dll libstdc++-6.dll libwinpthread-1.dll; do
    src="$(x86_64-w64-mingw32-g++ -print-file-name="$dll" 2>/dev/null)"
    if [ -f "$src" ]; then
        cp -f "$src" "$BUILD_DIR/"
        echo "  + $dll"
    else
        echo "  ! 未找到 $dll（跳过，可能在目标机系统提供）"
    fi
done

echo "[build-win] 完成。产物："
ls -lh "$BUILD_DIR/runtime.exe" "$BUILD_DIR/librvdl_rt.dll" 2>/dev/null || true
ls "$BUILD_DIR"/*.dll 2>/dev/null | sed 's/^/  /' || true
echo ""
echo "  下一步：./scripts/package-vsix.sh --win 打包 Windows vsix（runtime 进 bin/）"
