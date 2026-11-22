#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
VERSION="$(node -p "require('$ROOT/package.json').version")"
DIST_DIR="$ROOT/dist"

cd "$ROOT"

MODE="${1:---all}"
case "$MODE" in
    --linux) BUILD_LINUX=1; BUILD_WIN=0 ;;
    --win)   BUILD_LINUX=0; BUILD_WIN=1 ;;
    --all)   BUILD_LINUX=1; BUILD_WIN=1 ;;
    *) echo "用法：$0 [--linux|--win|--all]"; exit 1 ;;
esac

mkdir -p "$DIST_DIR"

echo "======================================================"
echo "[package-vsix] 编译 client/server（npm run compile）..."
echo "======================================================"
npm run compile

if [ "$BUILD_LINUX" -eq 1 ]; then
    echo ""
    echo "======================================================"
    echo "[package-vsix] 打包 Linux vsix"
    echo "======================================================"

    "$SCRIPT_DIR/../../build/linux/build-linux.sh"

    rm -rf "$ROOT/bin"
    "$SCRIPT_DIR/../../build/linux/prepare-vsix-runtime.sh"

    OUTFILE="$DIST_DIR/ravendrel-lang-$VERSION-linux-x64.vsix"
    echo "[package-vsix] vsce package -> $OUTFILE"
    npx @vscode/vsce package --out "$OUTFILE"
    echo "[package-vsix] 完成：$OUTFILE"
fi

if [ "$BUILD_WIN" -eq 1 ]; then
    echo ""
    echo "======================================================"
    echo "[package-vsix] 打包 Windows vsix（交叉编译）"
    echo "======================================================"

    "$SCRIPT_DIR/../../build/linux/build-windows-cross.sh"

    WIN_BUILD_DIR="$ROOT/runtime/build-win"
    rm -rf "$ROOT/bin"
    mkdir -p "$ROOT/bin"
    install -m 0755 "$WIN_BUILD_DIR/runtime.exe" "$ROOT/bin/runtime.exe"
    for dll in "$WIN_BUILD_DIR"/*.dll; do
        [ -f "$dll" ] && cp -f "$dll" "$ROOT/bin/"
    done
    echo "[package-vsix] bin/ 目录："
    ls -1 "$ROOT/bin" | sed 's/^/  /'

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