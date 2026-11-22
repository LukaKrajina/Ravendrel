#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
VERSION="$(node -p "require('$ROOT/package.json').version")"
DIST_DIR="$ROOT/dist"
MINGW_DEPS_ROOT="${MINGW_DEPS_ROOT:-$HOME/.local/mingw-deps}"

cd "$ROOT"
mkdir -p "$DIST_DIR"

echo "======================================================"
echo "[package-universal] 编译 client/server（npm run compile）..."
echo "======================================================"
npm run compile

echo ""
echo "[package-universal] 构建 Linux runtime ..."
"$SCRIPT_DIR/../../build/linux/build-linux.sh"

echo ""
echo "[package-universal] 构建 Windows runtime ..."
MINGW_DEPS_ROOT="$MINGW_DEPS_ROOT" "$SCRIPT_DIR/../../build/linux/build-windows-cross.sh"

echo ""
echo "[package-universal] 收集 runtime 产物 ..."
rm -rf "$ROOT/bin"
mkdir -p "$ROOT/bin/linux-x64" "$ROOT/bin/win32-x64"

install -m 0755 "$ROOT/runtime/build-linux/runtime" "$ROOT/bin/linux-x64/runtime"
for so in "$ROOT/runtime/build-linux/"*.so*; do
    [ -e "$so" ] && cp -a "$so" "$ROOT/bin/linux-x64/"
done

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

install -m 0755 "$ROOT/runtime/build-win/runtime.exe" "$ROOT/bin/win32-x64/runtime.exe"
for dll in "$ROOT/runtime/build-win/"*.dll; do
    [ -f "$dll" ] && cp -f "$dll" "$ROOT/bin/win32-x64/"
done

echo "[package-universal] bin/ 目录："
(cd "$ROOT/bin" && find . -maxdepth 2 \( -type f -o -type l \) | sed 's/^/  /')

OUTFILE="$DIST_DIR/ravendrel-lang-$VERSION.vsix"
echo ""
echo "[package-universal] vsce package -> $OUTFILE"
npx @vscode/vsce package --out "$OUTFILE"
echo "[package-universal] 完成：$OUTFILE"