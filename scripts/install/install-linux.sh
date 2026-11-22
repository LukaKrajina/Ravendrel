#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
RUNTIME_DIR="$(cd "$SCRIPT_DIR/../../runtime" && pwd)"

VERSION="${1:-}"
BUILD_DIR="${2:-$RUNTIME_DIR/build-linux}"

if [ -z "$VERSION" ]; then
    echo "用法：$0 <version> [build-dir]"
    echo "示例：$0 0.1.0"
    exit 1
fi

if [ ! -f "$BUILD_DIR/runtime" ] || [ ! -f "$BUILD_DIR/librvdl_rt.so" ]; then
    echo "[install-linux] 未找到构建产物，请先运行 ./scripts/build/linux/build-linux.sh"
    echo "  期望：$BUILD_DIR/runtime 与 $BUILD_DIR/librvdl_rt.so"
    exit 1
fi

BIN_DIR="$HOME/.ravendrel/toolchains/$VERSION/bin"
mkdir -p "$BIN_DIR"

echo "[install-linux] 安装到 $BIN_DIR"

install -m 0755 "$BUILD_DIR/runtime"           "$BIN_DIR/runtime"
install -m 0755 "$BUILD_DIR/librvdl_rt.so"    "$BIN_DIR/librvdl_rt.so"

KOKKOS_LIB_DIR="${KOKKOS_LIB_DIR:-$RUNTIME_DIR/../.deps/kokkos-install-lazy/lib}"
for so in libkokkoscore.so.5.2 libkokkoscontainers.so.5.2; do
    if [ -f "$KOKKOS_LIB_DIR/$so" ]; then
        install -m 0755 "$KOKKOS_LIB_DIR/$so" "$BIN_DIR/$so"
    fi
done

for exe in transmitter qvm_visualizer; do
    if [ -f "$BUILD_DIR/$exe" ]; then
        install -m 0755 "$BUILD_DIR/$exe" "$BIN_DIR/$exe"
    fi
done
if [ -f "$BUILD_DIR/rvdlRSP/rvdlRSP" ]; then
    install -m 0755 "$BUILD_DIR/rvdlRSP/rvdlRSP" "$BIN_DIR/rvdlRSP"
fi

cat > "$BIN_DIR/ravendrel" <<'EOF'
#!/usr/bin/env bash
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export LD_LIBRARY_PATH="$DIR${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
exec "$DIR/runtime" "$@"
EOF
chmod +x "$BIN_DIR/ravendrel"

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