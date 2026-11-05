#!/usr/bin/env bash
# ============================================================================
# install-system.sh — 把 Ravendrel（runtime 二进制 + rvdl CLI）安装到标准位置，
#                     注册到 PATH，并配置 runtime 后台常驻（systemd user service）。
#
# 安装布局（默认用户级，无需 root，符合 XDG）：
#   ~/.local/lib/ravendrel/<version>/
#       ├── bin/runtime            （runtime 二进制 daemon）
#       ├── bin/librvdl_rt.so     （量子核心共享库）
#       ├── bin/libkokkos*.so*     （Kokkos 运行时）
#       ├── ravendrel                  （shim：设置 LD_LIBRARY_PATH 后 exec runtime）
#       └── rvdl                     （rvdl CLI 单文件 bundle，node shebang）
#   ~/.local/bin/ravendrel -> ...      （PATH 注册，立即可用）
#   ~/.local/bin/rvdl    -> ...
#
# 后台常驻：调用 `runtime --install-autostart`，写入
#   ~/.config/systemd/user/ravendrel-runtime.service
# 并 enable + start + loginctl enable-linger（注销后仍常驻）。
#
# 用法：
#   ./scripts/install-system.sh [version] [build-dir]
#   ./scripts/install-system.sh --uninstall         # 卸载（停服务 + 删文件）
#   ./scripts/install-system.sh --status            # 查看 daemon 状态
#
# 环境变量：
#   INSTALL_ROOT    安装根目录（默认 ~/.local/lib/ravendrel/<version>）
#   BIN_LINK_DIR    命令软链目录（默认 ~/.local/bin，需在 PATH 中）
# ============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

VERSION="$(node -p "require('$ROOT/package.json').version")"
if [ "${1:-}" = "--uninstall" ] || [ "${1:-}" = "--status" ]; then
    : # 版本仍从 package.json 取
else
    VERSION="${1:-$VERSION}"
fi

BUILD_DIR="${2:-$ROOT/runtime/build-linux}"
INSTALL_ROOT="${INSTALL_ROOT:-$HOME/.local/lib/ravendrel/$VERSION}"
BIN_LINK_DIR="${BIN_LINK_DIR:-$HOME/.local/bin}"

# Kokkos 共享库目录
KOKKOS_LIB_DIR="${KOKKOS_LIB_DIR:-}"
if [ -z "$KOKKOS_LIB_DIR" ]; then
    if [ -d "$HOME/.local/kokkos-install/lib" ]; then
        KOKKOS_LIB_DIR="$HOME/.local/kokkos-install/lib"
    else
        KOKKOS_LIB_DIR="$ROOT/.deps/kokkos-install-lazy/lib"
    fi
fi

# ─────────────────────────────────────────────────────────
# 状态查询
# ─────────────────────────────────────────────────────────
if [ "${1:-}" = "--status" ]; then
    echo "=== 安装位置 ==="
    [ -d "$INSTALL_ROOT" ] && echo "  $INSTALL_ROOT" || echo "  未安装（$INSTALL_ROOT 不存在）"
    echo "=== PATH 注册 ==="
    ls -l "$BIN_LINK_DIR/ravendrel" "$BIN_LINK_DIR/rvdl" 2>/dev/null || echo "  未注册"
    echo "=== daemon 状态 ==="
    systemctl --user status ravendrel-runtime.service 2>&1 | head -8 || echo "  服务未安装"
    exit 0
fi

# ─────────────────────────────────────────────────────────
# 卸载
# ─────────────────────────────────────────────────────────
if [ "${1:-}" = "--uninstall" ]; then
    echo "[install-system] 卸载 Ravendrel $VERSION ..."
    if [ -x "$INSTALL_ROOT/bin/runtime" ]; then
        # runtime 需 LD_LIBRARY_PATH 才能加载同目录的 librvdl_rt.so
        LD_LIBRARY_PATH="$INSTALL_ROOT/bin${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
            "$INSTALL_ROOT/bin/runtime" --uninstall-autostart || true
    else
        systemctl --user stop ravendrel-runtime.service 2>/dev/null || true
        systemctl --user disable ravendrel-runtime.service 2>/dev/null || true
    fi
    rm -f "$BIN_LINK_DIR/ravendrel" "$BIN_LINK_DIR/rvdl"
    rm -rf "$INSTALL_ROOT"
    echo "[install-system] 卸载完成。"
    exit 0
fi

# ─────────────────────────────────────────────────────────
# 安装
# ─────────────────────────────────────────────────────────
echo "======================================================"
echo "[install-system] 安装 Ravendrel $VERSION"
echo "  安装位置：$INSTALL_ROOT"
echo "  命令目录：$BIN_LINK_DIR"
echo "======================================================"

# 0. 前置检查：构建产物
if [ ! -f "$BUILD_DIR/runtime" ] || [ ! -f "$BUILD_DIR/librvdl_rt.so" ]; then
    echo "[install-system] 未找到 Linux 构建产物，请先运行 ./scripts/build-linux.sh"
    echo "  期望：$BUILD_DIR/runtime 与 $BUILD_DIR/librvdl_rt.so"
    exit 1
fi

# rvdl CLI 单文件 bundle（esbuild 产物）
if [ ! -f "$ROOT/server/out/rvdl.js" ]; then
    echo "[install-system] 未找到 rvdl CLI（server/out/rvdl.js），请先运行 npm run compile"
    exit 1
fi

# 1. 安装 runtime 二进制与共享库
echo "[install-system] 安装 runtime + librvdl_rt.so ..."
mkdir -p "$INSTALL_ROOT/bin"
install -m 0755 "$BUILD_DIR/runtime"        "$INSTALL_ROOT/bin/runtime"
install -m 0755 "$BUILD_DIR/librvdl_rt.so" "$INSTALL_ROOT/bin/librvdl_rt.so"

# Kokkos 共享库（含符号链接链，保留 SONAME）
echo "[install-system] 安装 Kokkos 共享库（来自 $KOKKOS_LIB_DIR）..."
for so in "$KOKKOS_LIB_DIR"/libkokkos*.so*; do
    [ -e "$so" ] && cp -a "$so" "$INSTALL_ROOT/bin/"
done

# GLFW 共享库：runtime 直接依赖 libglfw.so.3，系统未必安装（Arch 需显式装 glfw 包）。
# 与 Kokkos 一样随安装目录自带，做到自包含、无需 root。
GLFW_LIB_DIR="${GLFW_LIB_DIR:-}"
if [ -z "$GLFW_LIB_DIR" ] && [ -d "$ROOT/.deps/glfw-install/lib" ]; then
    GLFW_LIB_DIR="$ROOT/.deps/glfw-install/lib"
fi
if [ -n "$GLFW_LIB_DIR" ]; then
    echo "[install-system] 安装 GLFW 共享库（来自 $GLFW_LIB_DIR）..."
    for so in "$GLFW_LIB_DIR"/libglfw*.so*; do
        [ -e "$so" ] && cp -a "$so" "$INSTALL_ROOT/bin/"
    done
fi

# 2. 安装 rvdl CLI（单文件，自带 node shebang）
echo "[install-system] 安装 rvdl CLI ..."
install -m 0755 "$ROOT/server/out/rvdl.js" "$INSTALL_ROOT/rvdl"

# 3. ravendrel shim：让 runtime 从同目录的 bin/ 加载 librvdl_rt.so
cat > "$INSTALL_ROOT/ravendrel" <<'EOF'
#!/usr/bin/env bash
# Ravendrel runtime shim：注入 LD_LIBRARY_PATH 后启动 runtime
DIR="$(cd "$(dirname "$(readlink -f "${BASH_SOURCE[0]}")")" && pwd)"
export LD_LIBRARY_PATH="$DIR/bin${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
exec "$DIR/bin/runtime" "$@"
EOF
chmod +x "$INSTALL_ROOT/ravendrel"

# 4. 注册到 PATH（软链到 ~/.local/bin）
echo "[install-system] 注册命令到 $BIN_LINK_DIR ..."
mkdir -p "$BIN_LINK_DIR"
ln -sf "$INSTALL_ROOT/ravendrel" "$BIN_LINK_DIR/ravendrel"
ln -sf "$INSTALL_ROOT/rvdl"    "$BIN_LINK_DIR/rvdl"

# 5. 后台常驻（systemd user service + enable-linger）
echo "[install-system] 配置 runtime 后台常驻（systemd --user）..."
# 同卸载：注入 LD_LIBRARY_PATH，让 runtime 能加载 librvdl_rt.so
LD_LIBRARY_PATH="$INSTALL_ROOT/bin${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
    "$INSTALL_ROOT/bin/runtime" --install-autostart

# 6. 验证
echo ""
echo "======================================================"
echo "[install-system] 安装完成，验证中..."
echo "======================================================"
sleep 2
echo "=== 命令位置 ==="
which ravendrel rvdl 2>/dev/null || echo "  （若未找到，请把 $BIN_LINK_DIR 加入 PATH）"
echo "=== daemon 状态 ==="
systemctl --user is-active ravendrel-runtime.service 2>&1 || true
echo "=== 端口 50052 ==="
(ss -ltnp 2>/dev/null | grep 50052 || netstat -ltnp 2>/dev/null | grep 50052 || echo "  （需安装 ss/netstat 才能查看）")
echo "=== 自检（PING）==="
printf 'PING\nEXIT\n' | "$INSTALL_ROOT/ravendrel" 2>&1 | head -3
echo ""
echo "常用命令："
echo "  rvdl run <file.rvdl>            运行 rvdl 脚本（自动连接常驻 daemon）"
echo "  rvdl compile x64 -m <file.rvdl> 编译为原生二进制"
echo "  systemctl --user status ravendrel-runtime.service   查看 daemon"
echo "  ./scripts/install-system.sh --status            查看安装状态"
echo "  ./scripts/install-system.sh --uninstall         卸载"