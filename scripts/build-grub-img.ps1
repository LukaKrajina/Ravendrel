#Requires -Version 5.1
<#
.SYNOPSIS
    用 grub-mkstandalone 生成可被 QEMU -kernel 直启的独立 GRUB 镜像（Multiboot1）

    依赖：预编译 GRUB2 for Windows（a1ive/grub，解压到 $env:TEMP\grub2\grub）。
#>
[CmdletBinding()]
param(
    [string]$GrubCfg = 'd:/Project/Ravendrel/runtime/qcos/grub_standalone.cfg',
    [string]$Kernel  = 'd:/Project/Ravendrel/runtime/build/qcos_kernel.elf',
    [string]$OutImg  = (Join-Path $env:TEMP 'grub.img')
)

$GrubDir = Join-Path $env:TEMP 'grub2\grub'
if (-not (Test-Path (Join-Path $GrubDir 'grub-mkstandalone.exe'))) {
    Write-Host "grub-mkstandalone.exe not found under $GrubDir" -ForegroundColor Red
    exit 1
}

Remove-Item $OutImg -Force -EA SilentlyContinue
Push-Location $GrubDir
$null = & (Join-Path $GrubDir 'grub-mkstandalone.exe') -O i386-multiboot -d i386-multiboot -o $OutImg `
    ("boot/grub/grub.cfg=" + $GrubCfg) ("boot/qcos_kernel.elf=" + $Kernel) 2>&1
Pop-Location
if (-not (Test-Path $OutImg)) { Write-Host 'grub-mkstandalone failed.' -ForegroundColor Red; exit 1 }

$bytes = [System.IO.File]::ReadAllBytes($OutImg)
$magic = [BitConverter]::GetBytes([UInt32]0x1BADB002)
$off = -1
for ($i = 0; $i -lt [Math]::Min($bytes.Length - 3, 8192); $i++) {
    if ($bytes[$i] -eq $magic[0] -and $bytes[$i+1] -eq $magic[1] -and $bytes[$i+2] -eq $magic[2] -and $bytes[$i+3] -eq $magic[3]) { $off = $i; break }
}
if ($off -lt 0) { Write-Host 'multiboot magic not found.' -ForegroundColor Red; exit 1 }

$nf = [BitConverter]::GetBytes([UInt32]0x2)
$bytes[$off+4] = $nf[0]; $bytes[$off+5] = $nf[1]; $bytes[$off+6] = $nf[2]; $bytes[$off+7] = $nf[3]
$checksum = [UInt32]((0x100000000 - ([UInt64]0x1BADB002 + [UInt64]0x2)) % 0x100000000)
$ns = [BitConverter]::GetBytes($checksum)
$bytes[$off+8] = $ns[0]; $bytes[$off+9] = $ns[1]; $bytes[$off+10] = $ns[2]; $bytes[$off+11] = $ns[3]
[System.IO.File]::WriteAllBytes($OutImg, $bytes)

Write-Host "GRUB image written: $OutImg" -ForegroundColor Green