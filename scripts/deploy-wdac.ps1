# ============================================================================
# 部署 WDAC 补充策略（需管理员）
# ============================================================================
$ErrorActionPreference = "Continue"
$log = "D:\BH_Project\ravendrel-vscode\scripts\wdac\deploy-log.txt" # 日志文件路径（当前硬编码）
$cipPath = "D:\BH_Project\ravendrel-vscode\scripts\wdac\supplemental.cip" # 补充策略文件路径（当前硬编码）

"=== deploy start $(Get-Date) ===" | Out-File $log

try {
    $isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
    "isAdmin: $isAdmin" | Out-File $log -Append

    if (-not $isAdmin) {
        "NOT ADMIN" | Out-File $log -Append
        exit 1
    }

    if (-not (Test-Path $cipPath)) {
        "cip not found: $cipPath" | Out-File $log -Append
        exit 1
    }

    $guid = [guid]::NewGuid().ToString().ToUpper()
    $destDir = "C:\Windows\System32\CodeIntegrity\CiPolicies\Active"
    $dest = Join-Path $destDir "{$guid}.cip"

    Copy-Item $cipPath $dest -Force
    "deployed to: $dest" | Out-File $log -Append
    "SUCCESS" | Out-File $log -Append
}
catch {
    "ERROR: $_" | Out-File $log -Append
}
