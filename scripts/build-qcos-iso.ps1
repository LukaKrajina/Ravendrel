#Requires -Version 5.1
<#
.SYNOPSIS
    生成可引导的 QCOS ISO（需 grub-mkrescue + xorriso）。

.DESCRIPTION
    先把内核 ELF 编译链接出来（见 build-runtime-windows.ps1 / 手动 lld 链接），
    再用 GRUB 的 Multiboot2 引导把它打成 ISO，供
        qemu-system-x86_64 -cdrom qcos.iso
    启动。

    依赖：GRUB 2（grub-mkrescue）与 xorriso。Windows 下推荐 MSYS2：
        pacman -S grub xorriso mtools

.EXAMPLE
    .\scripts\build-qcos-iso.ps1
#>
[CmdletBinding()]
param(
    [string]$KernelElf = 'd:/Project/Ravendrel/runtime/build/qcos.elf',
    [string]$GrubCfg   = 'd:/Project/Ravendrel/runtime/qcos/grub.cfg',
    [string]$OutIso    = 'd:/Project/Ravendrel/runtime/build/qcos.iso'
)

$ErrorActionPreference = 'Stop'

if (-not (Test-Path $KernelElf)) { Write-Host "kernel ELF not found: $KernelElf" -ForegroundColor Red; exit 1 }
if (-not (Test-Path $GrubCfg))   { Write-Host "grub.cfg not found: $GrubCfg" -ForegroundColor Red; exit 1 }

$grub = Get-Command grub-mkrescue -EA SilentlyContinue
if (-not $grub) { Write-Host 'grub-mkrescue not found. Install GRUB 2 (MSYS2: pacman -S grub xorriso mtools).' -ForegroundColor Red; exit 1 }

$stage = Join-Path $env:TEMP 'qcos-iso'
Remove-Item $stage -Recurse -Force -EA SilentlyContinue
New-Item -ItemType Directory -Path (Join-Path $stage 'boot\grub') -Force | Out-Null
Copy-Item $KernelElf (Join-Path $stage 'boot\qcos.elf') -Force
Copy-Item $GrubCfg   (Join-Path $stage 'boot\grub\grub.cfg') -Force

& $grub.Source -o $OutIso $stage
if ($LASTEXITCODE -ne 0) { Write-Host 'grub-mkrescue failed.' -ForegroundColor Red; exit $LASTEXITCODE }

Write-Host "ISO written: $OutIso" -ForegroundColor Green
Write-Host "Boot with: qemu-system-x86_64 -cdrom $OutIso -display none -serial stdio" -ForegroundColor Cyan