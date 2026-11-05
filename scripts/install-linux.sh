#!/usr/bin/env bash
# ============================================================================
# Ravendrel Runtime — Linux 安装脚本
# ----------------------------------------------------------------------------
# 用法：
#   ./scripts/install-linux.sh <version> [build-dir]
#
# 把构建产物安装到 ~/.ravendrel/toolchains/<version>/bin，并生成 ravendrel shim。
# shim 通过 LD_LIBRARY_PATH 让 runtime 找到同目录下的 librvdl_rt.so，
# 与 Windows 的 ravendrel.exe（Go 代理）保持一致的命令形态。
#
# 安装完成后，把下面路径加入 PATH：
#   export PATH="$HOME/.ravendrel/toolchains/<version>/bin:$PATH"
# ============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
RUNTIME_DIR="$(cd "$SCRIPT_DIR/../runtime" && pwd)"

VERSION="${1:-}"
BUILD_DIR="${2:-$RUNTIME_DIR/build-linux}"

if [ -z "$VERSION" ]; then
    echo "用法：$0 <version> [build-dir]"
    echo "示例：$0 0.1.0"
    exit 1
fi

if [ ! -f "$BUILD_DIR/runtime" ] || [ ! -f "$BUILD_DIR/librvdl_rt.so" ]; then
    echo "[install-linux] 未找到构建产物，请先运行 ./scripts/build-linux.sh"
    echo "  期望：$BUILD_DIR/runtime 与 $BUILD_DIR/librvdl_rt.so"
    exit 1
fi

BIN_DIR="$HOME/.ravendrel/toolchains/$VERSION/bin"
mkdir -p "$BIN_DIR"

echo "[install-linux] 安装到 $BIN_DIR"

# 1. 核心产物
install -m 0755 "$BUILD_DIR/runtime"           "$BIN_DIR/runtime"
install -m 0755 "$BUILD_DIR/librvdl_rt.so"    "$BIN_DIR/librvdl_rt.so"

# 1.5 lazy Kokkos（可选 GPU：无 libcuda/libcudart 硬链），使运行时在无 NVIDIA
#     环境也能加载（有 GPU 加速、无 GPU 回退 CPU）。
KOKKOS_LIB_DIR="${KOKKOS_LIB_DIR:-$RUNTIME_DIR/../.deps/kokkos-install-lazy/lib}"
for so in libkokkoscore.so.5.2 libkokkoscontainers.so.5.2; do
    if [ -f "$KOKKOS_LIB_DIR/$so" ]; then
        install -m 0755 "$KOKKOS_LIB_DIR/$so" "$BIN_DIR/$so"
    fi
done

# 2. 可选产物（存在则一并安装）
for exe in transmitter qvm_visualizer; do
    if [ -f "$BUILD_DIR/$exe" ]; then
        install -m 0755 "$BUILD_DIR/$exe" "$BIN_DIR/$exe"
    fi
done
if [ -f "$BUILD_DIR/rvdlRSP/rvdlRSP" ]; then
    install -m 0755 "$BUILD_DIR/rvdlRSP/rvdlRSP" "$BIN_DIR/rvdlRSP"
fi

# 3. ravendrel shim：让 runtime 从同目录加载 librvdl_rt.so
cat > "$BIN_DIR/ravendrel" <<'EOF'
#!/usr/bin/env bash
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export LD_LIBRARY_PATH="$DIR${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
exec "$DIR/runtime" "$@"
EOF
chmod +x "$BIN_DIR/ravendrel"

# 4. 记录当前激活版本（与 Windows 的 active_version.txt 保持一致）
ACTIVE_FILE="$HOME/.ravendrel/active_version.txt"
mkdir -p "$HOME/.ravendrel"
printf '%s' "$VERSION" > "$ACTIVE_FILE"

echo ""
echo "[install-linux] 安装完成。"
echo "  将以下路径加入 PATH 后即可使用 'ravendrel' 命令："
echo "    export PATH=\"$BIN_DIR:\$PATH\""
echo "  验证："
echo "    ravendrel --daemon          # 启动守护进程"
echo "    echo -e 'PING\\nEXIT' | ravendrel   # 交互式自检"
