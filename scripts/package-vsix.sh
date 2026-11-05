#!/usr/bin/env bash
# ============================================================================
# package-vsix.sh — 打包 Ravendrel VS Code 扩展为 vsix（runtime 内置于 bin/）
#
# 产出（dist/）：
#   ravendrel-lang-<version>-linux-x64.vsix   —— Linux 原生 runtime
#   ravendrel-lang-<version>-win32-x64.vsix   —— Windows 交叉编译 runtime
#
# 用法：
#   ./scripts/package-vsix.sh --linux        # 仅 Linux
#   ./scripts/package-vsix.sh --win          # 仅 Windows（交叉编译）
#   ./scripts/package-vsix.sh --all          # 两个平台
#
# 前置：
#   - Linux：见 scripts/build-linux.sh（LLVM / Kokkos / Vulkan / GLFW）
#   - Windows：见 scripts/build-windows-cross.sh（MinGW-w64 + 交叉编译依赖）
#   - Node.js + npm + @vscode/vsce
# ============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(dirname "$SCRIPT_DIR")"
VERSION="$(node -p "require('$ROOT/package.json').version")"
DIST_DIR="$ROOT/dist"

cd "$ROOT"

# 解析参数
MODE="${1:---all}"
case "$MODE" in
    --linux) BUILD_LINUX=1; BUILD_WIN=0 ;;
    --win)   BUILD_LINUX=0; BUILD_WIN=1 ;;
    --all)   BUILD_LINUX=1; BUILD_WIN=1 ;;
    *) echo "用法：$0 [--linux|--win|--all]"; exit 1 ;;
esac

mkdir -p "$DIST_DIR"

# ─── 0. 编译 client + server（esbuild bundle）──────────────
echo "======================================================"
echo "[package-vsix] 编译 client/server（npm run compile）..."
echo "======================================================"
npm run compile

# ─── 1. Linux vsix ──────────────────────────────────────
if [ "$BUILD_LINUX" -eq 1 ]; then
    echo ""
    echo "======================================================"
    echo "[package-vsix] 打包 Linux vsix"
    echo "======================================================"

    # 1a. 构建 Linux runtime
    "$SCRIPT_DIR/build-linux.sh"

    # 1b. 清空 bin/ 并复制 Linux runtime（runtime + *.so）
    rm -rf "$ROOT/bin"
    "$SCRIPT_DIR/prepare-vsix-runtime.sh"

    # 1c. 打包
    OUTFILE="$DIST_DIR/ravendrel-lang-$VERSION-linux-x64.vsix"
    echo "[package-vsix] vsce package -> $OUTFILE"
    npx @vscode/vsce package --out "$OUTFILE"
    echo "[package-vsix] 完成：$OUTFILE"
fi

# ─── 2. Windows vsix（交叉编译）─────────────────────────
if [ "$BUILD_WIN" -eq 1 ]; then
    echo ""
    echo "======================================================"
    echo "[package-vsix] 打包 Windows vsix（交叉编译）"
    echo "======================================================"

    # 2a. 交叉编译 Windows runtime
    "$SCRIPT_DIR/build-windows-cross.sh"

    # 2b. 清空 bin/ 并复制 Windows runtime（runtime.exe + *.dll）
    WIN_BUILD_DIR="$ROOT/runtime/build-win"
    rm -rf "$ROOT/bin"
    mkdir -p "$ROOT/bin"
    install -m 0755 "$WIN_BUILD_DIR/runtime.exe" "$ROOT/bin/runtime.exe"
    for dll in "$WIN_BUILD_DIR"/*.dll; do
        [ -f "$dll" ] && cp -f "$dll" "$ROOT/bin/"
    done
    echo "[package-vsix] bin/ 目录："
    ls -1 "$ROOT/bin" | sed 's/^/  /'

    # 2c. 打包
    OUTFILE="$DIST_DIR/ravendrel-lang-$VERSION-win32-x64.vsix"
    echo "[package-vsix] vsce package -> $OUTFILE"
    npx @vscode/vsce package --out "$OUTFILE"
    echo "[package-vsix] 完成：$OUTFILE"
fi

echo ""
echo "======================================================"
echo "[package-vsix] 全部完成。dist/ 产物："
ls -1 "$DIST_DIR"/*.vsix 2>/dev/null | sed 's/^/  /' || true
echo "======================================================"
