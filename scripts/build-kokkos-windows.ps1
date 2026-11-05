#Requires -Version 5.1
<#
.SYNOPSIS
    为 Ravendrel 构建并安装 Kokkos（CUDA + OpenMP 多线程）。

.DESCRIPTION
    本脚本完成以下工作：
      1. 自动定位 Visual Studio / MSVC / CMake / Ninja / CUDA
      2. 导入 MSVC 编译环境（vcvarsall x64）
      3. 以 Release + C++20 + CUDA + OpenMP 配置 Kokkos
      4. 多核并行构建并安装到 $InstallPrefix
      5. 把安装前缀、Node.js、LLVM 写入用户 PATH

    架构（GPU）会自动探测：脚本读取 nvidia-smi 的计算能力，
    映射到对应的 Kokkos_ARCH_* 选项；无 NVIDIA GPU 时自动关闭 CUDA。

.PARAMETER KokkosSource
    Kokkos 源码目录。

.PARAMETER InstallPrefix
    安装前缀。默认 C:\Libraries\Kokkos（与 README 的 C:/Libraries/kokkos 一致，
    且非管理员可写。C:\Program Files 在标准用户下不可写）。

.PARAMETER BuildType
    构建类型，默认 Release。

.PARAMETER Jobs
    并行编译作业数，0 表示使用全部逻辑核心。

.PARAMETER SkipInstall
    只构建不安装（用于分阶段执行长任务）。

.EXAMPLE
    .\scripts\build-kokkos-windows.ps1
    .\scripts\build-kokkos-windows.ps1 -SkipInstall
#>
[CmdletBinding()]
param(
    [string]$KokkosSource  = (Join-Path $env:USERPROFILE 'Desktop\kokkos'),
    # 与 build.bat / runtime 的 Kokkos_DIR 约定保持一致
    [string]$InstallPrefix = 'C:\Libraries\kokkos',
    [string]$BuildType     = 'Release',
    [int]   $Jobs          = 0,
    # CUDA 工具包根目录；留空则自动探测（PATH 上的 nvcc → 默认安装目录最新版本）
    [string]$CudaRoot      = '',
    # 一并构建 Kokkos 自带测试（默认关闭，-WithTests 打开）
    [switch]$WithTests,
    [switch]$SkipInstall
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

# ---------------------------------------------------------------------------
# 工具函数
# ---------------------------------------------------------------------------
function Write-Step { param([string]$Message)
    Write-Host "[kokkos-build] $Message" -ForegroundColor Cyan
}
function Write-Fail { param([string]$Message)
    Write-Host "[kokkos-build] ERROR: $Message" -ForegroundColor Red
}

function Find-First {
    param([string[]]$Candidates)
    foreach ($c in $Candidates) {
        if ($c -and (Test-Path $c)) { return $c }
    }
    return $null
}

<#
    StrictMode 下对 $null 取属性会抛错，命令未找到时必须显式判空。
#>
function Get-CommandPath {
    param([string]$Name)
    $cmd = Get-Command $Name -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    return $null
}

# ---------------------------------------------------------------------------
# 定位工具链
# ---------------------------------------------------------------------------
Write-Step 'Locating toolchain ...'

# 优先用官方 vswhere 定位；失败则扫描目录（跳过没有版本子目录的空壳目录）
$VsAppDir = $null
$Vswhere = Find-First @(
    'C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe'
)
if ($Vswhere) {
    $candidate = & $Vswhere -latest -products * `
                    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
                    -property installationPath 2>$null |
                 Select-Object -First 1
    if ($candidate -and (Test-Path $candidate)) { $VsAppDir = $candidate.Trim() }
}

if (-not $VsAppDir) {
    $VsRoot = 'C:\Program Files\Microsoft Visual Studio'
    if (Test-Path $VsRoot) {
        foreach ($v in (Get-ChildItem $VsRoot -Directory -ErrorAction SilentlyContinue |
                        Sort-Object { [int]($_.Name -replace '\D','') } -Descending)) {
            $edition = Get-ChildItem $v.FullName -Directory -ErrorAction SilentlyContinue |
                       Sort-Object Name -Descending | Select-Object -First 1
            if ($edition) { $VsAppDir = $edition.FullName; break }
        }
    }
}

if (-not $VsAppDir) { Write-Fail 'Visual Studio not found.'; exit 1 }
Write-Host "  VS           : $VsAppDir"

$VcvarsAll = Find-First @(Join-Path $VsAppDir 'VC\Auxiliary\Build\vcvarsall.bat')
if (-not $VcvarsAll) { Write-Fail 'vcvarsall.bat not found.'; exit 1 }

$CMake = Find-First @(
    (Join-Path $VsAppDir 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'),
    'C:\Program Files\CMake\bin\cmake.exe'
)
if (-not $CMake) { Write-Fail 'cmake.exe not found.'; exit 1 }
Write-Host "  CMake        : $CMake"

$Ninja = Find-First @(
    (Join-Path $VsAppDir 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe')
)
if (-not $Ninja) { Write-Fail 'ninja.exe not found.'; exit 1 }
Write-Host "  Ninja        : $Ninja"

# Kokkos 的 CUDA 校验只接受 NVIDIA(nvcc_wrapper) 或 Clang 作为 CXX 编译器，
# MSVC 不在其列（见 cmake/kokkos_test_cxx_std.cmake）。
# 本机 LLVM 的 clang++ 默认目标为 x86_64-pc-windows-msvc（MSVC ABI 兼容），
# 且实测可用 --cuda-path 直接编译 CUDA，故 CUDA 构建走 clang++。
$ClangCxx = Find-First @(
    'C:\Program Files\LLVM\bin\clang++.exe',
    (Get-CommandPath 'clang++.exe')
)
if ($ClangCxx) { Write-Host "  clang++      : $ClangCxx" }

# ---------------------------------------------------------------------------
# 导入 MSVC x64 编译环境
# ---------------------------------------------------------------------------
Write-Step 'Importing MSVC x64 environment ...'
$envDump = & cmd.exe /c "`"$VcvarsAll`" x64 >nul 2>&1 && set"
if ($LASTEXITCODE -ne 0) { Write-Fail 'vcvarsall.bat failed.'; exit 1 }
foreach ($line in $envDump) {
    if ($line -match '^([^=]+)=(.*)$') {
        [Environment]::SetEnvironmentVariable($Matches[1], $Matches[2], 'Process')
    }
}

$ClPath = Get-CommandPath 'cl.exe'
if (-not $ClPath) { Write-Fail 'cl.exe not on PATH after vcvarsall.'; exit 1 }
Write-Host "  cl.exe       : $ClPath"

# ---------------------------------------------------------------------------
# 探测 GPU 与 CUDA
# ---------------------------------------------------------------------------
Write-Step 'Detecting GPU / CUDA ...'
$EnableCuda   = $false
$KokkosArch   = $null
$CudaRoot     = $null
$ComputeCap   = $null

$NvidiaSmi = Get-CommandPath 'nvidia-smi.exe'
if ($NvidiaSmi) {
    $ComputeCap = (& $NvidiaSmi --query-gpu=compute_cap --format=csv,noheader 2>$null |
                    Select-Object -First 1).Trim()
}

# CUDA 工具包定位顺序：显式 -CudaRoot > PATH 上的 nvcc > 默认安装目录下的最新版本
if (-not $CudaRoot) {
    $Nvcc = Get-CommandPath 'nvcc.exe'
    if ($Nvcc) { $CudaRoot = Split-Path (Split-Path $Nvcc -Parent) -Parent }
}
if (-not $CudaRoot) {
    $ToolkitRoot = 'C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA'
    if (Test-Path $ToolkitRoot) {
        $LatestCuda = Get-ChildItem $ToolkitRoot -Directory -ErrorAction SilentlyContinue |
                      Where-Object { $_.Name -match '^v\d' } |
                      Sort-Object { [version]($_.Name.TrimStart('v')) } -Descending |
                      Select-Object -First 1
        if ($LatestCuda -and (Test-Path (Join-Path $LatestCuda.FullName 'bin\nvcc.exe'))) {
            $CudaRoot = $LatestCuda.FullName
        }
    }
}

if ($ComputeCap -and $CudaRoot) {
    $EnableCuda = $true
    Write-Host "  GPU          : compute capability $ComputeCap"
    Write-Host "  CUDA         : $CudaRoot"

    # --cuda-path 是**原始编译器标志**，路径中的空格会被当成参数分隔符，
    # 因此取 8.3 短名（CUDAToolkit_ROOT 由 CMake 处理，不受此限）。
    try {
        $Fso = New-Object -ComObject Scripting.FileSystemObject
        $CudaRootFlag = $Fso.GetFolder($CudaRoot).ShortPath
    } catch {
        $CudaRootFlag = $CudaRoot
    }
    if ($CudaRootFlag -ne $CudaRoot) {
        Write-Host "  CUDA (8.3)   : $CudaRootFlag"
    }
    <#
        既无空格（8.3 短名）又无反斜杠（正斜杠）的形态：
        Kokkos 会把 Kokkos_CUDA_DIR 写进生成的 CMake 代码，反斜杠会被当成
        转义符（"Invalid character escape '\P'"），故必须正斜杠。
        clang 的 --cuda-path 同样接受这种写法。
    #>
    $CudaRootFlagForward = $CudaRootFlag -replace '\\', '/'
    if ($CudaRootFlagForward -ne $CudaRootFlag) {
        Write-Host "  CUDA (flag)  : $CudaRootFlagForward"
    }

    <#
        Kokkos 会把 CUDA 路径写入生成的 CMake 配置代码。反斜杠在那里会被
        当成转义符（例如 "C:\Program Files\..." 触发
        "Invalid character escape '\P'"），因此 CUDAToolkit_ROOT 必须用正斜杠。
        编译器标志 --cuda-path 不是 CMake 代码，继续用无空格的 8.3 短名。
    #>
    $CudaRootForward = $CudaRoot -replace '\\', '/'
}

<#
    compute capability -> Kokkos_ARCH_*
    仅列出 Ampere 及之后的主流架构；未命中时回落到常见映射表。
#>
# 值为 Kokkos_ARCH_<X> 中的 <X> 部分
$ArchMap = @{
    '8.0'  = 'AMPERE80'
    '8.6'  = 'AMPERE86'
    '8.9'  = 'ADA89'
    '9.0'  = 'HOPPER90'
    '10.0' = 'BLACKWELL100'
    '12.0' = 'BLACKWELL120'
}
if ($EnableCuda) {
    if (-not $ArchMap.ContainsKey($ComputeCap)) {
        Write-Fail "Unmapped compute capability '$ComputeCap'. Please add it to `$ArchMap."
        exit 1
    }
    $KokkosArch = $ArchMap[$ComputeCap]
    Write-Host "  Kokkos arch  : Kokkos_ARCH_$KokkosArch"
} else {
    Write-Host '  CUDA         : not available, building CPU-only Kokkos' -ForegroundColor Yellow
}

# ---------------------------------------------------------------------------
# 组装 CMake 选项
# ---------------------------------------------------------------------------
if ($Jobs -le 0) { $Jobs = [Environment]::ProcessorCount }

$BuildDir = Join-Path $KokkosSource 'build-ravendrel'
$TestsFlag = if ($WithTests) { 'ON' } else { 'OFF' }

$CmakeArgs = @(
    '-G', 'Ninja'
    '-S', $KokkosSource
    '-B', $BuildDir
    "-DCMAKE_BUILD_TYPE=$BuildType"
    "-DCMAKE_INSTALL_PREFIX=$InstallPrefix"
    '-DCMAKE_CXX_STANDARD=20'
    <#
        CRT 运行时 = /MD(MultiThreadedDLL)：与 build.bat 的
        -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreadedDLL、以及 Qt6 msvc2022_64 /
        OpenSSL 等官方预编译依赖完全一致。

        注意：clang++ 走 GNU 前端（CMAKE_CXX_COMPILER_FRONTEND_VARIANT=GNU），
        CMake 对它不置 MSVC 变量，因此 CMAKE_MSVC_RUNTIME_LIBRARY 不会自动生效；
        且该编译器默认链接**静态** CRT（实测 lld-link 收到 -defaultlib:libcmt）。
        必须同时显式追加 -fms-runtime-lib=dll（见下方 CMAKE_CXX_FLAGS），
        目标文件才会带 /DEFAULTLIB:msvcrt.lib，真正做到 MD。
    #>
    '-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreadedDLL'
    "-DCMAKE_MAKE_PROGRAM=$Ninja"
    # --- 并行后端全开：Serial + OpenMP（CUDA 见下方条件块）---
    '-DKokkos_ENABLE_SERIAL=ON'
    '-DKokkos_ENABLE_OPENMP=ON'
    # --- CUDA 特性全开 ---
    '-DKokkos_ENABLE_CUDA_CONSTEXPR=ON'
    # --- 组件裁剪：默认跳过测试/示例/基准；-WithTests 可打开测试 ---
    "-DKokkos_ENABLE_TESTS=$TestsFlag"
    '-DKokkos_ENABLE_EXAMPLES=OFF'
    '-DKokkos_ENABLE_BENCHMARKS=OFF'
)

# 依据 LLVM 是否自带新版 offload driver 所需工具，决定 RDC 开关
$RdcFlag = 'OFF'
if ($ClangCxx) {
    $LlvmBin = Split-Path $ClangCxx -Parent
    $HasPackager = Test-Path (Join-Path $LlvmBin 'clang-offload-packager.exe')
    $HasOffloadBin = Test-Path (Join-Path $LlvmBin 'llvm-offload-binary.exe')
    if ($HasPackager -and $HasOffloadBin) {
        $RdcFlag = 'ON'
    } else {
        Write-Host '  Note: LLVM lacks clang-offload-packager / llvm-offload-binary.' -ForegroundColor Yellow
        Write-Host '        Falling back to the legacy offload driver (RDC disabled).' -ForegroundColor Yellow
    }
}

if ($EnableCuda) {
    if (-not $ClangCxx) {
        Write-Fail @(
            'Kokkos requires nvcc_wrapper or Clang for CUDA builds; MSVC is rejected.',
            'No clang++.exe found. Install LLVM, or pass -DisableCuda to build CPU-only.'
        )
        exit 1
    }
    $CmakeArgs += @(
        '-DKokkos_ENABLE_CUDA=ON'
        "-DKokkos_ARCH_$KokkosArch=ON"
        <#
            KokkosConfig.cmake 里生成的是
                SET(CUDAToolkit_ROOT  <value>)
            **没有引号**。若路径含空格，CMake 会把它当成分号列表，
            值被拼接成 "C:/ProgramFiles/NVIDIA..."（空格消失），
            下游 find_package(Kokkos) 就会报 "Could not find nvcc"。
            故这里必须传无空格的 8.3 短名 + 正斜杠。
        #>
        "-DCUDAToolkit_ROOT=$CudaRootFlagForward"
        <#
            rvdl_rt 是 SHARED 库，静态 Kokkos 链入 DLL 时设备代码需要可重定位，
            故默认开启 RDC。但 RDC 会启用 clang 的**新版** offload driver，
            后者强依赖 clang-offload-packager / llvm-offload-binary 两个工具；
            部分 LLVM 发行版并不包含它们，此时必须关闭 RDC 回退到旧 driver
            （仅依赖 clang-offload-bundler），否则每个 .cpp 都会：
              "llvm-offload-binary command failed: program not executable"
        #>
        "-DKokkos_ENABLE_CUDA_RELOCATABLE_DEVICE_CODE=$RdcFlag"
        <#
            Kokkos 会用 Kokkos_CUDA_DIR（优先）或 CUDAToolkit_BIN_DIR 拼出
            --cuda-path=<dir>/.. ；后者含空格时会被当成参数分隔符。
            直接给 8.3 短名最稳。
        #>
        "-DKokkos_CUDA_DIR=$CudaRootFlagForward"
        # --- 编译器：clang++（MSVC ABI）+ 显式 CUDA 路径 ---
        "-DCMAKE_CXX_COMPILER=$ClangCxx"
        '-DCMAKE_CXX_EXTENSIONS=OFF'
        <#
            -fms-runtime-lib=dll : 强制 /MD（原因见上方 CRT 说明）
            -Wno-unknown-cuda-version : 本机 CUDA 13.4 高于 clang 23 支持上限(13.2)，
            默认只是一条警告，显式静默以免刷屏/被当作错误。
        #>
        "-DCMAKE_CXX_FLAGS=--cuda-path=$CudaRootFlagForward -fms-runtime-lib=dll -Wno-unknown-cuda-version"
    )
}

<#
    CMake 会把 CMAKE_CXX_COMPILER 写进 CMakeCache.txt。若上次配置用的是别的
    编译器，必须清掉构建目录重新配置，否则新设置被静默忽略。
#>
$DesiredCompiler = if ($EnableCuda) { $ClangCxx } else { $ClPath }
$CacheFile = Join-Path $BuildDir 'CMakeCache.txt'
if (Test-Path $CacheFile) {
    $Cached = (Select-String -Path $CacheFile -Pattern '^CMAKE_CXX_COMPILER:.*=(.*)$' -ErrorAction SilentlyContinue |
               Select-Object -First 1).Matches.Groups[1].Value
    if ($Cached -and ($Cached -ne $DesiredCompiler)) {
        Write-Host "  Cached compiler differs ('$Cached'), wiping build dir ..." -ForegroundColor Yellow
        Remove-Item $BuildDir -Recurse -Force
    }
}

# ---------------------------------------------------------------------------
# 配置
# ---------------------------------------------------------------------------
Write-Step "Configuring Kokkos (build dir: $BuildDir) ..."
& $CMake @CmakeArgs
if ($LASTEXITCODE -ne 0) { Write-Fail 'CMake configure failed.'; exit $LASTEXITCODE }

# ---------------------------------------------------------------------------
# 并行构建
# ---------------------------------------------------------------------------
Write-Step "Building with $Jobs parallel jobs (this takes a while) ..."
& $CMake --build $BuildDir --config $BuildType --parallel $Jobs
if ($LASTEXITCODE -ne 0) { Write-Fail 'Build failed.'; exit $LASTEXITCODE }

# ---------------------------------------------------------------------------
# 安装
# ---------------------------------------------------------------------------
if (-not $SkipInstall) {
    Write-Step "Installing to $InstallPrefix ..."
    & $CMake --install $BuildDir --config $BuildType
    if ($LASTEXITCODE -ne 0) { Write-Fail 'Install failed.'; exit $LASTEXITCODE }

    # -----------------------------------------------------------------------
    # 写入用户 PATH（Kokkos / Node.js / LLVM）
    # -----------------------------------------------------------------------
    Write-Step 'Updating user PATH ...'
    $ExtraPaths = @(
        (Join-Path $InstallPrefix 'bin'),
        'C:\Program Files\nodejs',
        'C:\Program Files\LLVM\bin'
    )
    $UserPath = [Environment]::GetEnvironmentVariable('Path', 'User')
    $Parts = @()
    if ($UserPath) { $Parts = $UserPath -split ';' | Where-Object { $_ -and $_.Trim() } }
    $Changed = $false
    foreach ($p in $ExtraPaths) {
        if (Test-Path $p) {
            $Norm = $p.TrimEnd('\')
            if (-not ($Parts | Where-Object { $_.TrimEnd('\') -ieq $Norm })) {
                $Parts += $Norm
                $Changed = $true
                Write-Host "  + $Norm"
            }
        } else {
            Write-Host "  (skip missing) $p" -ForegroundColor DarkGray
        }
    }
    if ($Changed) {
        [Environment]::SetEnvironmentVariable('Path', ($Parts -join ';'), 'User')
        Write-Host '  User PATH updated. New shells will pick it up.' -ForegroundColor Green
    } else {
        Write-Host '  User PATH already up to date.' -ForegroundColor DarkGray
    }
}

Write-Step 'Done.'
if ($EnableCuda) {
    Write-Host "  Kokkos_DIR = $InstallPrefix/lib/cmake/Kokkos" -ForegroundColor Green
} else {
    Write-Host "  Kokkos_DIR = $InstallPrefix/lib/cmake/Kokkos" -ForegroundColor Green
}