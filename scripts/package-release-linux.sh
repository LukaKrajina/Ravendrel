#!/usr/bin/env bash
# ============================================================================
# package-release-linux.sh —— 组装 Linux 发布包并打成 tar.gz。
#
# 产物：
#   release/ravendrel-<package.json version>-linux-x64/  (runtime + librvdl_rt.so + 依赖)
#   release/ravendrel-<package.json version>-linux-x64.tar.gz
#
# 前置：先运行 ./scripts/build-linux.sh（构建 runtime + librvdl_rt.so）。
# ============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(dirname "$SCRIPT_DIR")"
# 版本号单一来源：package.json。曾经这里写死 0.6.6，与 package.json 的版本
# 各自漂移（发布目录名对不上实际版本，且没有任何一步会报错）。
VERSION="$(node -p "require('$ROOT/package.json').version" 2>/dev/null || echo "0.0.0-dev")"
REL_DIR="$ROOT/release/ravendrel-$VERSION-linux-x64"
BUILD_DIR="${1:-$ROOT/runtime/build-linux}"

if [ ! -d "$BUILD_DIR" ]; then
    echo "[package-release] build dir not found: $BUILD_DIR"
    echo "Run ./scripts/build-linux.sh first."
    exit 1
fi

mkdir -p "$REL_DIR"

# 核心产物
cp -f "$BUILD_DIR/runtime" "$REL_DIR/" 2>/dev/null || echo "warning: runtime not found"
for so in "$BUILD_DIR"/*.so*; do
    [ -f "$so" ] && cp -f "$so" "$REL_DIR/"
done

# lazy Kokkos（可选 GPU：无 libcuda/libcudart 硬链），使运行时在无 NVIDIA 环境也能加载
KOKKOS_LIB_DIR="${KOKKOS_LIB_DIR:-$ROOT/.deps/kokkos-install-lazy/lib}"
for so in libkokkoscore.so.5.2 libkokkoscontainers.so.5.2; do
    if [ -f "$KOKKOS_LIB_DIR/$so" ]; then
        cp -Lf "$KOKKOS_LIB_DIR/$so" "$REL_DIR/"
    fi
done

echo "=== release contents ==="
ls -1 "$REL_DIR" | sed 's/^/  /'

# 打包 tar.gz
TARBALL="$ROOT/release/ravendrel-$VERSION-linux-x64.tar.gz"
tar -czf "$TARBALL" -C "$ROOT/release" "ravendrel-$VERSION-linux-x64"
echo ""
echo "=== packaged ==="
echo "  $TARBALL"
