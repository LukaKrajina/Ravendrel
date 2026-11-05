# ============================================================================
# 用自签名证书做代码签名
# ============================================================================
param(
    [Parameter(Mandatory=$true)]
    [string[]]$Targets,
    [string]$PfxPath = "$PSScriptRoot\certs\ravendrel-codesign.pfx",
    [string]$Password = "ravendrel"
)

$ErrorActionPreference = "Stop"

# 定位 signtool（Windows SDK 版本各异）
$signtool = Get-ChildItem "C:\Program Files (x86)\Windows Kits\10\bin" `
    -Filter "signtool.exe" -Recurse -ErrorAction SilentlyContinue `
    | Where-Object { $_.FullName -match "x64" } `
    | Sort-Object FullName -Descending `
    | Select-Object -First 1 -ExpandProperty FullName

if (-not $signtool) { throw "signtool.exe not found. Install Windows SDK." }

foreach ($t in $Targets) {
    if (-not (Test-Path $t)) { Write-Warning "skip missing: $t"; continue }
    & $signtool sign /fd SHA256 /f $PfxPath /p $Password $t
    if ($LASTEXITCODE -ne 0) { Write-Warning "sign skipped (exit $LASTEXITCODE): $t" }
    else { Write-Host "[OK] signed: $t" }
}
