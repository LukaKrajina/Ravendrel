#!/usr/bin/env bash
# ============================================================================
# build-apk.sh — 把 .rvdl 项目构建成 Android APK（一键）
#
# 流程：
#   1. 交叉编译 Android 运行时 librvdl_rt.so（QVM 核心，ARM64，无 LLVM JIT）
#   2. `rvdl compile android` 把 .rvdl AOT 编译成 librvdl_main.so
#      （工具链路径经 CLI 随请求转发给 daemon，无需改造 daemon 环境）
#   3. 符号自检 + 收集两个 .so 到 jniLibs/arm64-v8a/
#   4. Gradle 打包 APK，并校验 APK 内含两个 .so
#
# 前置：Android SDK/NDK（sdkmanager / $ANDROID_HOME）、gradle、daemon 运行中、
#       `rvdl` CLI 在 PATH（server/out/cli.js）。
#
# 用法：
#   ./scripts/build-apk.sh <file.rvdl> [--release]
#   rvdl build apk <file.rvdl> [--release]      # 等价的 CLI 命令
# ============================================================================
set -euo pipefail

ANDROID_HOME="${ANDROID_HOME:-$HOME/Android/Sdk}"
NDK_VERSION="${NDK_VERSION:-26.1.10909125}"
NDK="$ANDROID_HOME/ndk/$NDK_VERSION"
NDK_TOOLCHAIN="$NDK/toolchains/llvm/prebuilt/linux-x86_64"

# Gradle 8.5 + AGP 8.2 需 Java 17+；本机 Java 17.0.2 有容器 cgroup 检测 bug
# （apksigner 抛 ExceptionInInitializerError），故用 Java 21。
JAVA_HOME="${JAVA_HOME:-$HOME/.local/share/mise/installs/java/21.0.2}"
if [ ! -x "$JAVA_HOME/bin/java" ]; then
    echo "[build-apk] 警告：$JAVA_HOME 不可用，改用系统 java。"
    unset JAVA_HOME
fi
export JAVA_HOME
export PATH="$JAVA_HOME/bin:$HOME/.local/bin:$PATH"
export ANDROID_HOME

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
APK_DIR="$ROOT/runtime/android/apk"

# 参数解析：<file.rvdl> [--release]
BUILD_TYPE="debug"
QK_FILE=""
for arg in "$@"; do
    case "$arg" in
        --release) BUILD_TYPE="release" ;;
        --debug) BUILD_TYPE="debug" ;;
        -*) ;;
        *) QK_FILE="$arg" ;;
    esac
done
if [ -z "$QK_FILE" ]; then
    echo "用法：$0 <file.rvdl> [--release]"
    exit 1
fi
QK_FILE="$(realpath "$QK_FILE")"
QK_NAME="$(basename "$QK_FILE" .rvdl)"

# ─── 0. 前置检查 ─────────────────────────────────────────
if [ ! -d "$NDK" ]; then
    echo "[build-apk] 错误：未找到 NDK $NDK_VERSION（$NDK）。"
    echo "  安装：sdkmanager \"ndk;$NDK_VERSION\"（或设置 ANDROID_HOME / NDK_VERSION）。"
    exit 1
fi
for tool in cmake ninja gradle rvdl nm; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "[build-apk] 错误：未找到 $tool（请安装并加入 PATH）。"
        exit 1
    fi
done

# ─── 1. 交叉编译 Android 运行时 librvdl_rt.so ─────────────
BUILD_DIR="$ROOT/runtime/android/build-android"
echo "[build-apk] 配置 / 编译 Android 运行时 librvdl_rt.so ..."
cmake -S "$ROOT/runtime/android" -B "$BUILD_DIR" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI=arm64-v8a \
    -DANDROID_PLATFORM=android-21 \
    -DCMAKE_BUILD_TYPE=Release
cmake --build "$BUILD_DIR" --parallel 8
RT_SO="$BUILD_DIR/librvdl_rt.so"
if [ ! -f "$RT_SO" ]; then
    echo "[build-apk] 错误：未生成 $RT_SO"
    exit 1
fi

# ─── 2. AOT 编译 rvdl 代码 → librvdl_main.so ──────────────
# 以下工具链变量由 rvdl CLI 随请求转发给 daemon（无需 daemon 环境预置）。
export RVDL_ANDROID_CLANGXX="$NDK_TOOLCHAIN/bin/aarch64-linux-android21-clang++"
export RVDL_ANDROID_SYSROOT="$NDK_TOOLCHAIN/sysroot"
export RVDL_ANDROID_RT_DIR="$BUILD_DIR"

echo "[build-apk] AOT 编译 $(basename "$QK_FILE") → librvdl_main.so ..."
cd "$ROOT"
rvdl compile android -m "$QK_FILE"
# CLI 默认把产物写到源文件同目录（<源目录>/<name>.so）。
MAIN_SO="$(dirname "$QK_FILE")/$QK_NAME.so"
if [ ! -f "$MAIN_SO" ]; then
    echo "[build-apk] 错误：未找到 AOT 产物 $MAIN_SO"
    echo "  请确认 daemon 可完成 Android 交叉链接（NDK clang++ 存在）。"
    exit 1
fi

# ─── 3. 符号自检 + 收集 .so ──────────────────────────────
JNILIBS="$APK_DIR/app/src/main/jniLibs/arm64-v8a"
rm -rf "$JNILIBS"   # 清理旧产物（历史遗留 libquark_* 等）
mkdir -p "$JNILIBS"
cp -f "$RT_SO" "$JNILIBS/librvdl_rt.so"
cp -f "$MAIN_SO" "$JNILIBS/librvdl_main.so"

check_symbols() {
    local so="$1"; shift
    local missing=0
    for sym in "$@"; do
        if ! nm -D --defined-only "$so" 2>/dev/null | awk '{print $NF}' | grep -qx "$sym"; then
            echo "[build-apk] 错误：$(basename "$so") 缺少导出符号：$sym"
            missing=1
        fi
    done
    return "$missing"
}

# JNI 入口必须由 librvdl_rt.so 导出（Java 侧 loadLibrary 后直接调用）。
check_symbols "$JNILIBS/librvdl_rt.so" \
    Java_com_ravendrel_RavendrelRuntime_nativeInit \
    Java_com_ravendrel_RavendrelRuntime_nativeShutdown \
    Java_com_ravendrel_RavendrelRuntime_nativeBellMeasure \
    Java_com_ravendrel_RavendrelRuntime_nativeRavendrelMain \
    || { echo "[build-apk] 符号自检失败：librvdl_rt.so 不是有效的 Android 运行时。"; exit 1; }

# AOT 主库必须导出入口函数（普通脚本 rvdl_main；@layer 脚本 rvdl_topology_entry）。
if ! nm -D --defined-only "$JNILIBS/librvdl_main.so" 2>/dev/null | awk '{print $NF}' | grep -qxE "rvdl_main|rvdl_topology_entry"; then
    echo "[build-apk] 错误：librvdl_main.so 未导出 rvdl_main / rvdl_topology_entry。"
    exit 1
fi

echo "[build-apk] jniLibs/arm64-v8a/:"
ls -lh "$JNILIBS" | sed 's/^/  /'

# ─── 4. Gradle 打包 ──────────────────────────────────────
GRADLE_TASK="assembleDebug"
if [ "$BUILD_TYPE" = "release" ]; then
    GRADLE_TASK="assembleRelease"
fi
echo "[build-apk] Gradle $GRADLE_TASK ..."
cd "$APK_DIR"
gradle "$GRADLE_TASK" --no-daemon

APK_OUT="$APK_DIR/app/build/outputs/apk/$BUILD_TYPE/app-$BUILD_TYPE.apk"
if [ ! -f "$APK_OUT" ]; then
    echo "[build-apk] 错误：未生成 APK（$BUILD_TYPE）"
    exit 1
fi

# ─── 5. APK 内容校验：两个 .so 必须入包 ──────────────────
if command -v unzip >/dev/null 2>&1; then
    APK_LIST="$(unzip -l "$APK_OUT")"
    for lib in "lib/arm64-v8a/librvdl_rt.so" "lib/arm64-v8a/librvdl_main.so"; do
        if ! printf '%s' "$APK_LIST" | grep -q "$lib"; then
            echo "[build-apk] 错误：APK 缺少 $lib"
            exit 1
        fi
    done
    echo "[build-apk] APK 内容校验通过（librvdl_rt.so + librvdl_main.so）。"
fi

echo ""
echo "[build-apk] 完成：$APK_OUT"
ls -lh "$APK_OUT"
