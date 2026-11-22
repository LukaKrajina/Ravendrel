#!/usr/bin/env bash
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