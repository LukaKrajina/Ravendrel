#Requires -Version 5.1
<#
.SYNOPSIS
    用 RVDL 编写 QCOS 内核：RVDL 源码 → LLVM IR → freestanding object → 链接引导 stub → ELF

.DESCRIPTION
    链路：
      RVDL 源码 (rvdl_main)
        -> node server/out/cli.js ir 生成 LLVM IR（UTF-8 无 BOM）
        -> clang++ --target=x86_64-unknown-elf 编译为 freestanding 对象
        -> 链接 boot_x86_64.S（Multiboot2 引导 + 长模式）与 rvdl_shim.cpp（kernel_main 桥接）
        -> 可引导 ELF

    产物可用 QEMU（需 GRUB 引导 Multiboot2）启动：
        qemu-system-x86_64 -cdrom qcos.iso -display none -serial stdio

    依赖：Node.js、LLVM（LLVM_ROOT 或 C:/Libraries/LLVM）。
    前置：先跑 npm/tsc 编译出 server/out/cli.js。

.EXAMPLE
    .\scripts\build-qcos-kernel.ps1
    .\scripts\build-qcos-kernel.ps1 -Source examples\my_kernel.rvdl
#>
[CmdletBinding()]
param(
    [string]$Source = 'examples/qcos_kernel.rvdl',
    [string]$OutElf  = 'runtime/build/qcos_kernel.elf'
)

$ErrorActionPreference = 'Stop'
$Root = Split-Path $PSScriptRoot -Parent

# ---- 定位工具链 ----
$LlvmRoot = $env:LLVM_ROOT
if (-not $LlvmRoot) { $LlvmRoot = 'C:\Libraries\LLVM' }
if (-not (Test-Path "$LlvmRoot\bin\clang++.exe")) { Write-Host "LLVM not found: $LlvmRoot" -ForegroundColor Red; exit 1 }

$Node = (Get-Command node -EA SilentlyContinue).Source
if (-not $Node) { $Node = 'C:\Program Files\nodejs\node.exe' }

$Clang  = Join-Path $LlvmRoot 'bin\clang.exe'
$ClangX = Join-Path $LlvmRoot 'bin\clang++.exe'
$LdLld  = Join-Path $LlvmRoot 'bin\ld.lld.exe'

$Src    = (Join-Path $Root $Source) -replace '\\', '/'
$BootS  = (Join-Path $Root 'runtime/qcos/boot_x86_64.S') -replace '\\', '/'
$Shim   = (Join-Path $Root 'runtime/qcos/rvdl_shim.cpp') -replace '\\', '/'
$Linker = (Join-Path $Root 'runtime/qcos/linker_x86_64.ld') -replace '\\', '/'
$Inc    = (Join-Path $Root 'runtime/include') -replace '\\', '/'
$Out    = (Join-Path $Root $OutElf) -replace '\\', '/'

$Work = Join-Path $env:TEMP 'qcos-kernel-build'
New-Item -ItemType Directory -Path $Work -Force | Out-Null
$Ir = Join-Path $Work 'kernel.ll'

# 注意：不启用 -mno-sse。x86_64 SysV ABI 用 xmm 寄存器传递/返回 double，
# 量子服务（QMS）需要 SSE；boot_x86_64.S 已在进入长模式后使能CR4.OSFXSR/OSXMMEXCPT 并 fninit。
$FF = @('-ffreestanding','-fno-exceptions','-fno-rtti','-fno-stack-protector','-mno-red-zone')

# ---- RVDL -> LLVM IR（UTF-8 无 BOM；PowerShell 的 > 重定向会写 UTF-16）----
Write-Host '[qcos-kernel] Generating LLVM IR ...'
$irLines = & $Node (Join-Path $Root 'server/out/cli.js') ir $Src 2>$null
[System.IO.File]::WriteAllText($Ir, ($irLines -join "`n"), (New-Object System.Text.UTF8Encoding($false)))

# ---- freestanding 编译 boot / shim / kernel ----
Write-Host '[qcos-kernel] Compiling (freestanding x86_64-unknown-elf) ...'
& $Clang  --target=x86_64-unknown-elf -ffreestanding -c $BootS -o (Join-Path $Work 'boot.o')
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& $ClangX --target=x86_64-unknown-elf @FF -I $Inc -c $Shim -o (Join-Path $Work 'shim.o')
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& $ClangX --target=x86_64-unknown-elf @FF -c $Ir -o (Join-Path $Work 'kernel.o')
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

# ---- 链接 ----
Write-Host '[qcos-kernel] Linking ...'
& $LdLld -m elf_x86_64 -T $Linker (Join-Path $Work 'boot.o') (Join-Path $Work 'shim.o') (Join-Path $Work 'kernel.o') -o $Out
if ($LASTEXITCODE -ne 0) { Write-Host '[qcos-kernel] link failed.' -ForegroundColor Red; exit $LASTEXITCODE }

Write-Host "[qcos-kernel] ELF written: $Out" -ForegroundColor Green