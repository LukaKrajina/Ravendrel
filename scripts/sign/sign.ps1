# ============================================================================
# OpenMasons 代码签名脚本（Windows / signtool）
#
# 版权与归属：Copyright (c) 2026 OpenMasons. All rights reserved.
#
# 首次运行会自动生成一张 OpenMasons 自签名代码签名证书：
#   Subject : CN=OpenMasons Code Signing, O=OpenMasons, C=US
#   PFX     : scripts/certs/openmasons-codesign.pfx（密码见 -Password）
#   CER     : scripts/certs/openmasons-codesign.cer
#
# ⚠ 自签名证书只证明「文件由 OpenMasons 构建」，不是受信任 CA 证书。
#   Windows Smart App Control（SAC）/ WDAC 默认仍会拒绝未受信任的签名，
#   Event ID 3077/3118 会记录 "did not meet the Enterprise signing level
#   requirement"。要真正放行，需要管理员把该证书加入 WDAC 补充策略
#   （scripts/build/wdac/）或关闭 SAC。签名失败不会中断构建，只打印警告。
# ============================================================================
param(
    [Parameter(Mandatory=$true)]
    [string[]]$Targets,
    [string]$PfxPath = "",
    [string]$CerPath = "",
    [string]$Password = "OpenMasons!2026",
    [string]$Subject = "CN=OpenMasons Code Signing, O=OpenMasons, C=US"
)

$ErrorActionPreference = "Stop"
$copyright = "Copyright (c) 2026 OpenMasons. All rights reserved."

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
if (-not $ScriptDir) { $ScriptDir = (Get-Location).Path }
if (-not $PfxPath) { $PfxPath = Join-Path (Split-Path -Parent $ScriptDir) "certs\openmasons-codesign.pfx" }
if (-not $CerPath) { $CerPath = Join-Path (Split-Path -Parent $ScriptDir) "certs\openmasons-codesign.cer" }

$targetList = @()
foreach ($t in $Targets) {
    foreach ($part in ($t -split ",")) {
        $trimmed = $part.Trim()
        if ($trimmed) { $targetList += $trimmed }
    }
}

function Find-SignTool {
    $roots = @(
        "C:\Program Files (x86)\Windows Kits\10\bin",
        "C:\Program Files\Windows Kits\10\bin"
    )
    foreach ($root in $roots) {
        if (-not (Test-Path $root)) { continue }
        $tool = Get-ChildItem $root -Filter "signtool.exe" -Recurse -ErrorAction SilentlyContinue |
            Where-Object { $_.FullName -match "x64" } |
            Sort-Object FullName -Descending |
            Select-Object -First 1 -ExpandProperty FullName
        if ($tool) { return $tool }
    }
    return $null
}

function Ensure-OpenMasonsCertificate {
    if (Test-Path $PfxPath) { return }

    $cert = Get-ChildItem Cert:\CurrentUser\My -ErrorAction SilentlyContinue |
        Where-Object { $_.Subject -like "*OpenMasons*" -and $_.HasPrivateKey } |
        Sort-Object NotAfter -Descending |
        Select-Object -First 1

    if (-not $cert) {
        Write-Host "[cert] generating OpenMasons self-signed code-signing certificate"
        $cert = New-SelfSignedCertificate `
            -Type CodeSigningCert `
            -Subject $Subject `
            -KeyUsage DigitalSignature `
            -KeyAlgorithm RSA `
            -KeyLength 3072 `
            -HashAlgorithm SHA256 `
            -CertStoreLocation "Cert:\CurrentUser\My" `
            -NotAfter (Get-Date).AddYears(10)
    }
    else {
        Write-Host "[cert] reusing OpenMasons certificate $($cert.Thumbprint)"
    }

    $certDir = Split-Path -Parent $PfxPath
    if ($certDir) { New-Item -ItemType Directory -Force -Path $certDir | Out-Null }

    $secure = ConvertTo-SecureString -String $Password -Force -AsPlainText
    Export-PfxCertificate -Cert $cert -FilePath $PfxPath -Password $secure | Out-Null
    Export-Certificate -Cert $cert -FilePath $CerPath | Out-Null

    Write-Host "[cert] PFX: $PfxPath"
    Write-Host "[cert] CER: $CerPath"
    Write-Host "[cert] $copyright"
}

$signtool = Find-SignTool
if (-not $signtool) {
    Write-Warning "signtool.exe not found; install the Windows SDK. Signing skipped."
    exit 0
}

Ensure-OpenMasonsCertificate

$failed = 0
foreach ($t in $targetList) {
    if (-not (Test-Path $t)) {
        Write-Warning "skip missing: $t"
        continue
    }

    & $signtool sign /fd SHA256 /f $PfxPath /p $Password $t
    if ($LASTEXITCODE -ne 0) {
        Write-Warning "sign failed (exit $LASTEXITCODE): $t"
        $failed++
        continue
    }

    $sig = Get-AuthenticodeSignature -FilePath $t
    Write-Host "[OK] signed: $t ($($sig.Status))"
}

if ($failed -gt 0) {
    Write-Warning "$failed target(s) could not be signed. $copyright"
    Write-Warning "SAC/WDAC may still block unsigned or untrusted binaries (Event 3077/3118)."
}

exit 0