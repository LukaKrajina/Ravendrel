// Fox backend/embed_spv.ps1 - bake compiled SPIR-V into a C++ header
//
// Run after editing shaders.vert / shaders.frag:
//   powershell -File backend/embed_spv.ps1
//
// Compiles the GLSL with glslc (from the Vulkan SDK) and writes
// backend/shaders_spv.h as byte arrays. The build then has NO dependency on a
// shader compiler being installed - which matters because the project ships to
// Linux/macOS builders and Android NDK cross-compiles, where glslc is not
// necessarily present. Shaders change rarely; the header changes with them.

param(
    [string]$Glslc = "glslc"
)

$ErrorActionPreference = "Stop"
$dir = Split-Path -Parent $MyInvocation.MyCommand.Path

$targets = @(
    @{ src = "shaders.vert"; out = "shaders.vert.spv"; name = "fox_spv_vert"; size = "FOX_SPV_VERT_SIZE"; stage = "vert" },
    @{ src = "shaders.frag"; out = "shaders.frag.spv"; name = "fox_spv_frag"; size = "FOX_SPV_FRAG_SIZE"; stage = "frag" }
)

foreach ($t in $targets) {
    Write-Host "compiling $($t.src)..."
    & $Glslc "-fshader-stage=$($t.stage)" (Join-Path $dir $t.src) -o (Join-Path $dir $t.out)
    if ($LASTEXITCODE -ne 0) { throw "glslc failed on $($t.src)" }
}

$sb = New-Object System.Text.StringBuilder
[void]$sb.AppendLine("// Fox backend/shaders_spv.h - GENERATED, do not edit by hand.")
[void]$sb.AppendLine("//")
[void]$sb.AppendLine("// Regenerate with: powershell -File backend/embed_spv.ps1")
[void]$sb.AppendLine("//             or: bash backend/embed_spv.sh")
[void]$sb.AppendLine("// Source: backend/shaders.vert + shaders.frag")
[void]$sb.AppendLine("")
[void]$sb.AppendLine("#pragma once")
[void]$sb.AppendLine("#include <cstdint>")
[void]$sb.AppendLine("")

foreach ($t in $targets) {
    $bytes = [System.IO.File]::ReadAllBytes((Join-Path $dir $t.out))
    # 以 uint32_t 字输出：VkShaderModuleCreateInfo::pCode 是 uint32_t*，
    # 字节数组需要非对齐的 reinterpret_cast。
    $words = @()
    for ($i = 0; $i -lt $bytes.Length; $i += 4) {
            $w = [System.BitConverter]::ToUInt32($bytes, $i)
            $words += ("0x{0:x8}" -f $w)
    }
    [void]$sb.AppendLine("// $($t.src) ($($bytes.Length) bytes)")
    [void]$sb.AppendLine("static const uint32_t $($t.size) = $($bytes.Length);")
    [void]$sb.AppendLine("static const uint32_t $($t.name)[] = {")
    for ($i = 0; $i -lt $words.Count; $i += 6) {
            $chunk = $words[$i..([Math]::Min($i + 5, $words.Count - 1))]
            [void]$sb.AppendLine("    " + ($chunk -join ", ") + ",")
    }
    [void]$sb.AppendLine("};")
    [void]$sb.AppendLine("")
}

$headerPath = Join-Path $dir "shaders_spv.h"
[System.IO.File]::WriteAllText($headerPath, $sb.ToString(), (New-Object System.Text.UTF8Encoding($false)))
Write-Host "wrote $headerPath"