#!/usr/bin/env bash
# Fox backend/embed_spv.sh - 把编译好的 SPIR-V 烘焙进 C++ 头文件
#
# 修改 shaders.vert / shaders.frag 后运行：
#   bash backend/embed_spv.sh
#
# Windows 上的孪生脚本 embed_spv.ps1 做的事完全一样；这个脚本是为了在
# Linux/macOS/CI 上不装 PowerShell 也能重新生成着色器。
#
# ⚠ CRLF。头文件是生成后入库的文件，PowerShell 孪生脚本用 Windows 换行
#   写它，所以这里也一样。若这里用 LF，在不同平台上每次重新生成都会让
#   `git diff` 显示"整个文件都变了"，把真正的着色器改动淹没掉。
#
# .spv 文件本身也保留在仓库里：它们是编译器输出，头文件由它们派生——
# 这样头文件和嵌入的二进制永远不会对不上上次编译的结果。
set -euo pipefail

dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
glslc="${GLSLC:-glslc}"
compile=1
if [ "${1:-}" = "--no-compile" ]; then
    compile=0
fi

if [ "$compile" = 1 ]; then
    echo "compiling shaders.vert..."
    "$glslc" -fshader-stage=vert "$dir/shaders.vert" -o "$dir/shaders.vert.spv"
    echo "compiling shaders.frag..."
    "$glslc" -fshader-stage=frag "$dir/shaders.frag" -o "$dir/shaders.frag.spv"
fi

out="$dir/shaders_spv.h"

# 以 uint32_t 字输出：VkShaderModuleCreateInfo::pCode 是 uint32_t*，字节数组
# 需要非对齐的 reinterpret_cast。`od -tx4` 按主机字节序读文件，与 Vulkan
# loader 在 Fox 支持的所有平台上期望的顺序一致（magic word 0x07230203 在
# 小端机器上直接正确；大端主机的字节本来就该交换）。
emit_array() {
    local src="$1" spv="$2" size_macro="$3" name="$4"
    local bytes
    bytes=$(stat -c %s "$spv")
    echo "// $src ($bytes bytes)"
    echo "static const uint32_t $size_macro = $bytes;"
    echo "static const uint32_t $name[] = {"
    od -An -v -tx4 "$spv" | tr -s ' ' '\n' | grep -v '^$' | awk '
        { printf("%s%s", ((NR - 1) % 6 == 0) ? "    0x" : ", 0x", $0);
          if (NR % 6 == 0) printf(",\n") }
        END { if (NR % 6 != 0) printf(",\n") }'
    echo "};"
    echo ""
}

{
    echo "// Fox backend/shaders_spv.h - GENERATED, do not edit by hand."
    echo "//"
    echo "// Regenerate with: powershell -File backend/embed_spv.ps1"
    echo "//             or: bash backend/embed_spv.sh"
    echo "// Source: backend/shaders.vert + shaders.frag"
    echo ""
    echo "#pragma once"
    echo "#include <cstdint>"
    echo ""
    emit_array "shaders.vert" "$dir/shaders.vert.spv" "FOX_SPV_VERT_SIZE" "fox_spv_vert"
    emit_array "shaders.frag" "$dir/shaders.frag.spv" "FOX_SPV_FRAG_SIZE" "fox_spv_frag"
} | awk '{ printf "%s\r\n", $0 }' > "$out"

echo "wrote $out"
