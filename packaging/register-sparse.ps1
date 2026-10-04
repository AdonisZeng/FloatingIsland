# 注册 sparse MSIX，为 FloatingIsland.exe 赋予 package identity。
# 前提：开启开发者模式（设置 > 系统 > 开发者选项），未签名包走 -Register 通道。
# 用法：powershell -ExecutionPolicy Bypass -File packaging\register-sparse.ps1
#       可选参数 -ExeDir 指定 exe 目录（默认 ..\x64\Release）

param([string]$ExeDir = "")

$ErrorActionPreference = "Stop"
$manifest = Join-Path $PSScriptRoot "AppxManifest.xml"

if (-not $ExeDir) {
    $ExeDir = Join-Path $PSScriptRoot "..\x64\Release"
}
$ExeDir = (Resolve-Path $ExeDir).Path

if (-not (Test-Path (Join-Path $ExeDir "FloatingIsland.exe"))) {
    throw "FloatingIsland.exe 不存在于 $ExeDir ，请先构建 Release。"
}

$dev = Get-ItemProperty "HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\AppModelUnlock" `
        -ErrorAction SilentlyContinue
if (-not $dev -or -not $dev.AllowDevelopmentWithoutDevLicense) {
    Write-Warning "开发者模式未开启。未签名包注册可能失败，请先开启（设置 > 系统 > 开发者选项）。"
}

# 若旧版本已注册，先移除，保证 -ExternalLocation 指向当前输出目录
Get-AppxPackage -Name "FloatingIsland" -ErrorAction SilentlyContinue |
    Remove-AppxPackage -ErrorAction SilentlyContinue

Add-AppxPackage -Register $manifest -ExternalLocation $ExeDir

$pkg = Get-AppxPackage -Name "FloatingIsland" -ErrorAction SilentlyContinue
if (-not $pkg) { throw "注册后未找到包，请检查上方错误输出。" }

Write-Host ""
Write-Host "注册成功"
Write-Host "  包全名   : $($pkg.PackageFullName)"
Write-Host "  包 Family: $($pkg.PackageFamilyName)"
Write-Host ""
Write-Host "重要：必须从开始菜单启动（搜 FloatingIsland），直接双击 exe 没有包身份。"
Write-Host "命令行启动："
Write-Host ("  explorer.exe 'shell:AppsFolder\{0}!App'" -f $pkg.PackageFamilyName)
