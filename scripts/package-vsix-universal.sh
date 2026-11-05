#!/usr/bin/env bash
# ============================================================================
# package-vsix-universal.sh — 打包「通用」vsix：一个包同时携带 linux-x64 + win32-x64 runtime
#
# 产物：dist/ravendrel-lang-<version>.vsix（无平台后缀）
#   bin/linux-x64/  → runtime + librvdl_rt.so + libkokkos*.so*
#   bin/win32-x64/  → runtime.exe + librvdl_rt.dll + MinGW 运行时 DLL
#   server.ts 的 resolveRuntime() 按 process.platform 选择对应子目录。
#
# 前置：
#   - Linux：见 scripts/build-linux.sh（LLVM / Kokkos / Vulkan / GLFW）
#   - Windows：见 scripts/build-windows-cross.sh（MinGW-w64 + MINGW_DEPS_ROOT）
#   - Node.js + npm + @vscode/vsce
# ============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(dirname "$SCRIPT_DIR")"
VERSION="$(node -p "require('$ROOT/package.json').version")"
DIST_DIR="$ROOT/dist"
MINGW_DEPS_ROOT="${MINGW_DEPS_ROOT:-$HOME/.local/mingw-deps}"

cd "$ROOT"
mkdir -p "$DIST_DIR"

# ─── 0. 编译 client + server ──────────────────────────────
echo "======================================================"
echo "[package-universal] 编译 client/server（npm run compile）..."
echo "======================================================"
npm run compile

# ─── 1. 构建两个平台的 runtime ───────────────────────────
echo ""
echo "[package-universal] 构建 Linux runtime ..."
"$SCRIPT_DIR/build-linux.sh"

echo ""
echo "[package-universal] 构建 Windows runtime ..."
MINGW_DEPS_ROOT="$MINGW_DEPS_ROOT" "$SCRIPT_DIR/build-windows-cross.sh"

# ─── 2. 收集产物到分平台目录 ─────────────────────────────
echo ""
echo "[package-universal] 收集 runtime 产物 ..."
rm -rf "$ROOT/bin"
mkdir -p "$ROOT/bin/linux-x64" "$ROOT/bin/win32-x64"

# Linux：runtime + librvdl_rt.so（+ 其他 .so）
install -m 0755 "$ROOT/runtime/build-linux/runtime" "$ROOT/bin/linux-x64/runtime"
for so in "$ROOT/runtime/build-linux/"*.so*; do
    [ -e "$so" ] && cp -a "$so" "$ROOT/bin/linux-x64/"
done

# Kokkos 共享库（libkokkoscore/containers/simd，含符号链接链保留 SONAME）
KOKKOS_LIB_DIR="${KOKKOS_LIB_DIR:-}"
if [ -z "$KOKKOS_LIB_DIR" ]; then
    if [ -d "$HOME/.local/kokkos-install/lib" ]; then
        KOKKOS_LIB_DIR="$HOME/.local/kokkos-install/lib"
    else
        KOKKOS_LIB_DIR="$ROOT/.deps/kokkos-install-lazy/lib"
    fi
fi
for so in "$KOKKOS_LIB_DIR"/libkokkos*.so*; do
    [ -e "$so" ] && cp -a "$so" "$ROOT/bin/linux-x64/"
done

# Windows：runtime.exe + librvdl_rt.dll + MinGW 运行时 DLL（build-win/ 已收集）
install -m 0755 "$ROOT/runtime/build-win/runtime.exe" "$ROOT/bin/win32-x64/runtime.exe"
for dll in "$ROOT/runtime/build-win/"*.dll; do
    [ -f "$dll" ] && cp -f "$dll" "$ROOT/bin/win32-x64/"
done

echo "[package-universal] bin/ 目录："
(cd "$ROOT/bin" && find . -maxdepth 2 \( -type f -o -type l \) | sed 's/^/  /')

# ─── 3. 打包 ─────────────────────────────────────────────
OUTFILE="$DIST_DIR/ravendrel-lang-$VERSION.vsix"
echo ""
echo "[package-universal] vsce package -> $OUTFILE"
npx @vscode/vsce package --out "$OUTFILE"
echo "[package-universal] 完成：$OUTFILE"
