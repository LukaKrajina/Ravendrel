# ============================================================================
# 更新 WDAC 补充策略（去掉 Audit Mode 后）并刷新需要管理员权限运行
# ============================================================================
$ErrorActionPreference = "Continue"
$cip = "D:\BH_Project\ravendrel-vscode\scripts\wdac\supplemental.cip"
$log = "D:\BH_Project\ravendrel-vscode\scripts\wdac\fix-log.txt"

"=== fix start $(Get-Date) ===" | Out-File $log

try {
    $isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
    "isAdmin: $isAdmin" | Out-File $log -Append
    if (-not $isAdmin) {
        "NOT ADMIN — re-run as administrator" | Out-File $log -Append
        exit 1
    }

    if (-not (Test-Path $cip)) {
        "cip not found: $cip" | Out-File $log -Append
        exit 1
    }

    # 1. 更新策略（CiTool 用 cip 内嵌 PolicyID 定位并替换）
    $out = & CiTool.exe --update-policy $cip 2>&1
    "CiTool --update-policy exit=$LASTEXITCODE" | Out-File $log -Append
    "$out" | Out-File $log -Append

    # 2. 刷新策略
    $out2 = & CiTool.exe --refresh 2>&1
    "CiTool --refresh exit=$LASTEXITCODE" | Out-File $log -Append
    "$out2" | Out-File $log -Append

    "SUCCESS" | Out-File $log -Append
}
catch {
    "ERROR: $_" | Out-File $log -Append
}
