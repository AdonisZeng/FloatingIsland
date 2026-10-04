# 离线编译 HLSL -> 头文件字节码数组（由 src/*.cpp 直接 include）
# 修改 src/shaders/*.hlsl 后重跑本脚本。
# 生成的头依赖：无；运行时不再需要 D3DCOMPILER_47.dll。

$ErrorActionPreference = "Stop"

$fxcCandidates = @(
    "C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64\fxc.exe",
    "C:\Program Files (x86)\Windows Kits\10\bin\10.0.22621.0\x64\fxc.exe"
)
$fxc = $fxcCandidates | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $fxc) { throw "未找到 fxc.exe，请安装 Windows SDK。" }

$srcDir = Join-Path $PSScriptRoot "..\src\shaders"
$outDir = Join-Path $srcDir "generated"
New-Item -ItemType Directory -Force -Path $outDir | Out-Null

$jobs = @(
    @{ In = "fullscreen_vs.hlsl"; Entry = "VS"; Target = "vs_5_0"; Out = "fullscreen_vs.h"; Var = "g_fullscreen_vs" },
    @{ In = "island_ps.hlsl";     Entry = "PS"; Target = "ps_5_0"; Out = "island_ps.h";     Var = "g_island_ps" }
)

foreach ($j in $jobs) {
    $in  = Join-Path $srcDir $j.In
    $out = Join-Path $outDir $j.Out
    & $fxc /nologo /T $j.Target /E $j.Entry /Fh $out /Vn $j.Var $in
    if ($LASTEXITCODE -ne 0) { throw "编译失败: $($j.In)" }
    Write-Host "OK  $($j.Out)"
}
