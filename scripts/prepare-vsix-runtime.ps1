# ============================================================================
# prepare-vsix-runtime.ps1 —— 把 runtime 构建产物复制到扩展 bin/，
# 使其随 vsix 打包，安装扩展后即可开箱运行/编译/构建 rvdl 代码。
#
# 前置：先用 build.bat 构建 runtime（runtime.exe + rvdl_rt.dll）。
# 用法：./scripts/prepare-vsix-runtime.ps1 [-BuildDir runtime/build]
# ============================================================================
param(
    [string]$BuildDir = "runtime/build"
)

$ErrorActionPreference = "Continue"
$extBin = Join-Path $PSScriptRoot "..\bin"
New-Item -ItemType Directory -Force -Path $extBin | Out-Null

if (-not (Test-Path $BuildDir)) {
    Write-Warning "[prepare-vsix-runtime] build dir not found: $BuildDir"
    Write-Host "Run build.bat first, then re-run this script."
    exit 1
}

# 1. 核心可执行
$exe = Join-Path $BuildDir "runtime.exe"
if (Test-Path $exe) {
    Copy-Item $exe $extBin -Force
    Write-Host "[prepare-vsix-runtime] runtime.exe -> bin/"
} else {
    Write-Warning "[prepare-vsix-runtime] runtime.exe not found"
}

# 2. 核心共享库 + 依赖 DLL
$dlls = Get-ChildItem $BuildDir -Filter "*.dll" -ErrorAction SilentlyContinue
$copied = 0
foreach ($dll in $dlls) {
    Copy-Item $dll.FullName $extBin -Force
    $copied++
}
Write-Host "[prepare-vsix-runtime] copied $copied DLL(s) -> bin/"

# 3. 列出 bin/ 目录
Write-Host ""
Write-Host "[prepare-vsix-runtime] bin/ contents:"
Get-ChildItem $extBin | ForEach-Object { Write-Host ("  " + $_.Name) }

Write-Host ""
Write-Host "[prepare-vsix-runtime] done. Run 'npx vsce package' to bundle bin/ into the vsix."
