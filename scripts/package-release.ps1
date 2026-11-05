# ============================================================================
# package-release.ps1 —— 组装 Windows 发布包并打成 zip。
#
# 产物：
#   release/ravendrel-0.6.0-windows-x64/  (runtime.exe + rvdl_rt.dll + 依赖)
#   release/ravendrel-0.6.0-windows-x64.zip
# ============================================================================
$ErrorActionPreference = "Stop"

$root = Split-Path $PSScriptRoot -Parent
$version = "0.6.6"
$relDir = Join-Path $root "release\ravendrel-$version-windows-x64"
$buildDir = Join-Path $root "runtime\build"

New-Item -ItemType Directory -Force -Path $relDir | Out-Null

if (-not (Test-Path $buildDir)) {
    Write-Warning "runtime/build not found. Run build.bat first."
    exit 1
}

# 核心产物
Copy-Item (Join-Path $buildDir "runtime.exe") $relDir -Force -ErrorAction SilentlyContinue
Copy-Item (Join-Path $buildDir "rvdl_rt.dll") $relDir -Force -ErrorAction SilentlyContinue

# 依赖 DLL
Copy-Item "C:\Program Files\LLVM\bin\libomp.dll" $relDir -Force -ErrorAction SilentlyContinue
Copy-Item "C:\Libraries\zlib\bin\zlib.dll" $relDir -Force -ErrorAction SilentlyContinue

Write-Host "=== release contents ==="
Get-ChildItem $relDir | ForEach-Object { Write-Host ("  " + $_.Name) }

# 打包 zip
$zip = Join-Path $root "release\ravendrel-$version-windows-x64.zip"
if (Test-Path $zip) { Remove-Item $zip -Force }
Compress-Archive -Path (Join-Path $relDir "*") -DestinationPath $zip
Write-Host ""
Write-Host "=== packaged ==="
Write-Host "  $zip"
