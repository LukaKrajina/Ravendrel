#Requires -Version 5.1
<#
.SYNOPSIS
    配置并构建 Ravendrel 运行时（rvdl_rt 等）。

.DESCRIPTION
    自动定位 VS / CMake / Ninja / clang / CUDA / Kokkos / zlib / zstd，
    以 Release + C++20 配置 runtime/ 并并行构建。

    本脚本固化了本机踩过的坑（详见各 NOTE 注释）：
      * LLVM 必须用 clang+llvm- 发行版（含 lib/cmake/llvm/LLVMConfig.cmake），
        工具链版 LLVM-*.exe 缺 CMake 开发文件。
      * 除 CXX 外还必须指定 C 编译器与 RC（llvm-rc），否则 project() 失败。
      * Kokkos 导出的 CUDAToolkit_ROOT 未加引号，空格路径会被 SET() 切坏
        -> 已由 build-kokkos-windows.ps1 打补丁；此处再用 8.3 短路径兜底。
      * LLVMSupport 的链接接口依赖 ZLIB::ZLIB 与 zstd::libzstd_static，
        两者必须先装好并加入 CMAKE_PREFIX_PATH。
      * clang++ 在 Windows 上 OpenMP 需要显式给出 flags / lib 名。

.PARAMETER Target
    要构建的目标，默认 rvdl_rt。

.PARAMETER Jobs
    并行作业数，0 表示全部逻辑核心。

.PARAMETER ConfigureOnly
    只配置不构建。

.EXAMPLE
    .\scripts\build-runtime-windows.ps1
    .\scripts\build-runtime-windows.ps1 -Target runtime -ConfigureOnly
#>
[CmdletBinding()]
param(
    [string]$Target = 'rvdl_rt',
    [int]   $Jobs   = 0,
    [switch]$ConfigureOnly
)

$ErrorActionPreference = 'Stop'

function Find-First {
    param([string[]]$Candidates)
    foreach ($c in $Candidates) { if ($c -and (Test-Path $c)) { return $c } }
    return $null
}

# ---------------------------------------------------------------------------
# 定位工具链
# ---------------------------------------------------------------------------
Write-Host '[ravendrel-build] Locating toolchain ...'

$VsRoot = 'C:\Program Files\Microsoft Visual Studio'
$VsAppDir = $null
if (Test-Path $VsRoot) {
    foreach ($v in (Get-ChildItem $VsRoot -Directory -ErrorAction SilentlyContinue)) {
        $ed = Get-ChildItem $v.FullName -Directory -ErrorAction SilentlyContinue |
              Select-Object -First 1
        if ($ed) { $VsAppDir = $ed.FullName; break }
    }
}
if (-not $VsAppDir) { Write-Host '[ravendrel-build] ERROR: Visual Studio not found.' -ForegroundColor Red; exit 1 }

$CMake = Find-First @(
    (Join-Path $VsAppDir 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'),
    'C:\Program Files\CMake\bin\cmake.exe'
)
$NinjaDir = Find-First @((Join-Path $VsAppDir 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe'))
if (-not $NinjaDir) { Write-Host '[ravendrel-build] ERROR: ninja not found.' -ForegroundColor Red; exit 1 }
$env:PATH = (Split-Path $NinjaDir -Parent) + ';' + $env:PATH
Write-Host "  CMake  : $CMake"
Write-Host "  Ninja  : $NinjaDir"

# LLVM 根目录：优先 LLVM_ROOT，其次常见安装位置
$LlvmRoot = $env:LLVM_ROOT
if (-not $LlvmRoot) {
    # LLVM-md 为 /MD 构建的 LLVM（与 build.bat / GLFW / Qt6 的 CRT 对齐）
    $LlvmRoot = Find-First @('C:\Libraries\LLVM-md', 'C:\Libraries\LLVM', 'C:\Program Files\LLVM')
}
if (-not (Test-Path (Join-Path $LlvmRoot 'lib\cmake\llvm\LLVMConfig.cmake'))) {
    Write-Host "[ravendrel-build] ERROR: $LlvmRoot lacks lib/cmake/llvm/LLVMConfig.cmake." -ForegroundColor Red
    Write-Host '  Install the clang+llvm- distribution (not the LLVM-*.exe toolchain installer).' -ForegroundColor Yellow
    exit 1
}
Write-Host "  LLVM   : $LlvmRoot"

$ClangC   = Join-Path $LlvmRoot 'bin\clang.exe'
$ClangCxx = Join-Path $LlvmRoot 'bin\clang++.exe'
$LlvmRc   = Join-Path $LlvmRoot 'bin\llvm-rc.exe'

# ---------------------------------------------------------------------------
# 依赖前缀（zlib / zstd —— LLVMSupport 的链接接口需要）
# ---------------------------------------------------------------------------
$Prefix = @()
foreach ($d in @('C:\Libraries\ZLIB', 'C:\Libraries\ZSTD')) {
    if (Test-Path $d) { $Prefix += $d }
    else { Write-Host "  (missing) $d - LLVMSupport needs it" -ForegroundColor Yellow }
}
if ($Prefix.Count -gt 0) {
    $env:CMAKE_PREFIX_PATH = ($Prefix -join ';')
    Write-Host "  CMAKE_PREFIX_PATH = $($Prefix -join ';')"
}

# ---------------------------------------------------------------------------
# CUDA：8.3 短路径，规避 Kokkos 导出时空格被吞的问题
# ---------------------------------------------------------------------------
$CudaCandidates = @($env:CUDA_PATH)
$ToolkitRoot = 'C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA'
if (Test-Path $ToolkitRoot) {
    $CudaCandidates += (Get-ChildItem $ToolkitRoot -Directory -ErrorAction SilentlyContinue |
                        Where-Object { $_.Name -match '^v\d' } |
                        Sort-Object { [version]($_.Name.TrimStart('v')) } -Descending |
                        ForEach-Object { $_.FullName })
}
$CudaRoot = Find-First $CudaCandidates
$CudaFlag = $null
if ($CudaRoot) {
    try {
        $Fso = New-Object -ComObject Scripting.FileSystemObject
        $CudaFlag = ($Fso.GetFolder($CudaRoot).ShortPath) -replace '\\', '/'
    } catch { $CudaFlag = ($CudaRoot -replace '\\', '/') }
    Write-Host "  CUDA   : $CudaFlag"
}

# ---------------------------------------------------------------------------
# DIA SDK
#      LLVMDebugInfoPDB 的链接接口含 DIASDK::Diaguids；CMake 默认探测不到时
#      会报 "MSVC_DIA_SDK_DIR not set, and could not be inferred"，进而
#      LLVMExports 生成失败。路径含空格，但下面用 & $CMake @Args 原生调用，
#      PowerShell 会正确传递。
# ---------------------------------------------------------------------------
$DiaSdk = Find-First @(
    (Join-Path $VsAppDir 'DIA SDK'),
    'C:\Program Files\Microsoft Visual Studio\18\Professional\DIA SDK'
)
if ($DiaSdk) { Write-Host "  DIA SDK: $DiaSdk" }
else { Write-Host '  (missing) DIA SDK - LLVMDebugInfoPDB needs DIASDK::Diaguids' -ForegroundColor Yellow }

# ---------------------------------------------------------------------------
# 组装并运行配置
# ---------------------------------------------------------------------------
$Root     = Split-Path $PSScriptRoot -Parent
$SourceDir = (Join-Path $Root 'runtime') -replace '\\', '/'
$BuildDir  = (Join-Path $Root 'runtime\build') -replace '\\', '/'

if (Test-Path (Join-Path $Root 'runtime\build')) {
    Remove-Item (Join-Path $Root 'runtime\build') -Recurse -Force
}

$Args = @(
    '-G', 'Ninja'
    '-S', $SourceDir
    '-B', $BuildDir
    '-DCMAKE_BUILD_TYPE=Release'
    "-DCMAKE_C_COMPILER=$($ClangC   -replace '\\','/')"
    "-DCMAKE_CXX_COMPILER=$($ClangCxx -replace '\\','/')"
    "-DCMAKE_RC_COMPILER=$($LlvmRc    -replace '\\','/')"
    "-DLLVM_DIR=$((Join-Path $LlvmRoot 'lib/cmake/llvm') -replace '\\','/')"
    '-DOpenMP_CXX_FLAGS=-fopenmp'
    '-DOpenMP_CXX_LIB_NAMES=libomp'
    "-DOpenMP_libomp_LIBRARY=$((Join-Path $LlvmRoot 'lib/libomp.lib') -replace '\\','/')"
    '-DCMAKE_CXX_STANDARD=20'
    # CRT 必须与 LLVM 构建时一致，否则链接期报
    # "/failifmismatch: mismatch detected for 'RuntimeLibrary'"。
    # 本机统一走 /MD：LLVM(C:\Libraries\LLVM-md)、vendor/GLFW(lib-vc2026)、
    # Qt6(msvc2022_64) 均为 MD；Kokkos 亦以 MultiThreadedDLL +
    # -fms-runtime-lib=dll 构建（见 build-kokkos-windows.ps1）。
    '-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreadedDLL'
    # CMAKE_MSVC_RUNTIME_LIBRARY 只在策略 CMP0091 为 NEW 时才生效；若项目的
    # cmake_minimum_required 较旧，该策略默认是 OLD，变量会被静默忽略
    # （表现仍为 -D_DLL / --dependent-lib=msvcrt）。这里强制置为 NEW。
    '-DCMAKE_POLICY_DEFAULT_CMP0091=NEW'
)
if ($CudaFlag) {
    # Kokkos 会往编译命令里注入 `-x cuda --cuda-gpu-arch=...`。用 clang 作 CUDA
    # 编译器时，clang **不会**自动探测 CUDA_PATH，必须显式给 --cuda-path，
    # 否则报 "cannot find libdevice for sm_86" / "cannot find CUDA installation"。
    # 用 8.3 短路径是因为该标志是原始编译器参数，路径中的空格会被当分隔符。
    # Kokkos 的 Kokkos_MathematicalFunctions.hpp 会 #include <cuda/std/cmath>，
    # 该头文件来自 libcu++ (CCCL)，位于 <CUDA>/include/cccl。用 8.3 短路径时
    # FindCUDAToolkit 只探测到 <CUDA>/include 而漏掉 cccl，故显式补上。
    $Args += "-DCMAKE_CXX_FLAGS=--cuda-path=$CudaFlag -I$CudaFlag/include/cccl"
    $Args += "-DCUDAToolkit_ROOT=$CudaFlag"
}
if ($DiaSdk) { $Args += "-DMSVC_DIA_SDK_DIR=$($DiaSdk -replace '\\','/')" }
# -fPIC 是 Linux 写法；clang 以 x86_64-pc-windows-msvc 为目标时会拒绝该选项。
# Windows 的 DLL 本身就可重定位，不需要 PIC。
$Args += '-DCMAKE_POSITION_INDEPENDENT_CODE=OFF'

Write-Host '[ravendrel-build] Configuring ...'
& $CMake @Args
if ($LASTEXITCODE -ne 0) { Write-Host '[ravendrel-build] ERROR: configure failed.' -ForegroundColor Red; exit $LASTEXITCODE }

if (-not (Test-Path (Join-Path $Root 'runtime\build\build.ninja'))) {
    Write-Host '[ravendrel-build] ERROR: build.ninja was not generated.' -ForegroundColor Red
    exit 1
}
Write-Host '[ravendrel-build] Configure OK.' -ForegroundColor Green

if ($ConfigureOnly) { exit 0 }

# ---------------------------------------------------------------------------
# 并行构建
# ---------------------------------------------------------------------------
if ($Jobs -le 0) { $Jobs = [Environment]::ProcessorCount }
Write-Host "[ravendrel-build] Building target '$Target' with $Jobs jobs ..."
& $CMake --build $BuildDir --config Release --target $Target --parallel $Jobs
if ($LASTEXITCODE -ne 0) { Write-Host '[ravendrel-build] ERROR: build failed.' -ForegroundColor Red; exit $LASTEXITCODE }

Write-Host '[ravendrel-build] Done.' -ForegroundColor Green