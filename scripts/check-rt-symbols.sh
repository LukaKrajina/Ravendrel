#!/usr/bin/env bash
# ============================================================================
# check-rt-symbols.sh — 校验 librvdl_rt.so 导出了 RuntimeApi.h 声明的全部 C ABI 符号。
#
# 用途：CI / 发布流程防止出现「构建成功但 ABI 残缺」的运行时库（历史上曾因
# 目标名 / 输出目录错位而打包过陈旧或错误的 .so）。期望符号表直接从
# runtime/include/qhal/RuntimeApi.h 提取，头文件即契约，无需手工维护清单。
#
# 用法：
#   ./scripts/check-rt-symbols.sh [path/to/librvdl_rt.so]
#   不传路径时在仓库常见构建目录下自动查找。
# ============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
HEADER="$ROOT/runtime/include/qhal/RuntimeApi.h"

SO="${1:-}"
if [ -z "$SO" ]; then
    SO="$(find "$ROOT" -maxdepth 4 -name 'librvdl_rt.so' -type f 2>/dev/null | head -1)"
fi
if [ -z "$SO" ] || [ ! -f "$SO" ]; then
    echo "[check-rt-symbols] 错误：未找到 librvdl_rt.so（请先构建 rvdl_rt 目标，或显式传入路径）。"
    exit 1
fi

if [ ! -f "$HEADER" ]; then
    echo "[check-rt-symbols] 错误：未找到 $HEADER"
    exit 1
fi

NM="${NM:-nm}"
if ! command -v "$NM" >/dev/null 2>&1; then
    echo "[check-rt-symbols] 错误：未找到 nm（binutils）。"
    exit 1
fi

# 期望符号：RVDL_RT_API 声明行中紧邻 '(' 的 rvdl_* 标识符。
EXPECTED="$(awk '/RVDL_RT_API/ { if (match($0, /rvdl_[a-z0-9_]+\(/)) print substr($0, RSTART, RLENGTH-1) }' "$HEADER" | sort -u)"
COUNT="$(printf '%s\n' "$EXPECTED" | wc -l | tr -d ' ')"
if [ "$COUNT" -lt 25 ]; then
    echo "[check-rt-symbols] 错误：从 RuntimeApi.h 仅提取到 $COUNT 个符号——头文件格式可能已变化，请检查本脚本的提取规则。"
    exit 1
fi

# 落盘成文件再 grep：避免 pipefail 下 `printf | grep -q` 因 grep 提前退出
# 触发 SIGPIPE(141) 造成「符号明明存在却被判缺失」的假阴性。
DEFINED_LIST="$(mktemp)"
trap 'rm -f "$DEFINED_LIST"' EXIT
"$NM" -D --defined-only "$SO" 2>/dev/null | awk '{print $NF}' | sort -u > "$DEFINED_LIST"

MISSING=0
while IFS= read -r sym; do
    [ -n "$sym" ] || continue
    if ! grep -qx "$sym" "$DEFINED_LIST"; then
        echo "[check-rt-symbols] 缺少导出符号：$sym"
        MISSING=1
    fi
done <<< "$EXPECTED"

if [ "$MISSING" -ne 0 ]; then
    echo "[check-rt-symbols] 失败：$SO 未导出全部 C ABI 符号（期望 $COUNT 个）。"
    exit 1
fi

echo "[check-rt-symbols] OK：$SO 导出了全部 $COUNT 个 C ABI 符号。"
