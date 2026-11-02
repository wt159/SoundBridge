<#
.SYNOPSIS
    以正确的运行库布局启动 SoundBridge（Windows 开发态）。

.DESCRIPTION
    build/app 里只部署了 Qt 运行时（Qt5*.dll + platforms/），SDL2d/FFmpeg 等第三方
    DLL 仍然躺在 sdk/3rdparty/dist/<dist>/bin 下。直接双击 exe 会因缺 DLL 静默退出，
    既没有弹窗也没有输出，非常难查。本脚本把 dist/bin 与 build/app 一起前置到 PATH
    后再启动。

    用 Start-Process 而不是直接调用 exe：这样播放器不挂在当前 shell 名下，
    关掉终端（或当前 shell 被回收）不会连带把播放器带走。

.PARAMETER BuildDir
    构建目录，默认 <repo>/build。

.PARAMETER ThirdPartyDistName
    第三方运行库目录名，默认 windows_x86_64_gcc_debug，与 CMake toolchain 保持一致。

.PARAMETER Rebuild
    启动前先增量编译 SoundBridge 目标。

.PARAMETER TailLog
    启动后持续跟随最新日志（Ctrl+C 退出，不影响播放器）。

.PARAMETER Wait
    阻塞到播放器退出（适合脚本化冒烟）。

.EXAMPLE
    .\scripts\run_player.ps1
.EXAMPLE
    .\scripts\run_player.ps1 -Rebuild -TailLog
#>
param(
    [string]$BuildDir = "",
    [string]$ThirdPartyDistName = "windows_x86_64_gcc_debug",
    [switch]$Rebuild,
    [switch]$TailLog,
    [switch]$Wait
)

$ErrorActionPreference = "Stop"

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot = Split-Path -Parent $scriptDir

if (-not $BuildDir) {
    $BuildDir = Join-Path $repoRoot "build"
}
$BuildDir = (Resolve-Path $BuildDir).Path

$appDir = Join-Path $BuildDir "app"
$exe = Join-Path $appDir "SoundBridge.exe"
$distBin = Join-Path $repoRoot "sdk\3rdparty\dist\$ThirdPartyDistName\bin"
$logDir = Join-Path $appDir "log"

if (-not (Test-Path $exe)) {
    throw "找不到 $exe，先编译：cmake --build `"$BuildDir`""
}
if (-not (Test-Path $distBin)) {
    throw "找不到第三方运行库目录 $distBin，先构建 sdk/3rdparty（见 scripts\setup_windows_env.ps1）"
}
if (-not (Test-Path (Join-Path $appDir "Qt5Core.dll"))) {
    throw "build\app 里没有 Qt5Core.dll，Qt 运行时没部署过来。先跑 scripts\run_sdk_tests.ps1 -QtDeploy 或手动 windeployqt。"
}

if ($Rebuild) {
    Write-Host "[run_player] 增量编译 SoundBridge ..." -ForegroundColor Cyan
    & cmake --build $BuildDir --target SoundBridge
    if ($LASTEXITCODE -ne 0) {
        throw "编译失败（exit $LASTEXITCODE）"
    }
}

# 关键一步：dist/bin 提供 SDL2d/FFmpeg，build/app 提供 Qt 与 MinGW 运行时。
$env:PATH = "$distBin;$appDir;$env:PATH"

Write-Host "[run_player] exe    : $exe" -ForegroundColor DarkGray
Write-Host "[run_player] rtdeps : $distBin" -ForegroundColor DarkGray

$proc = Start-Process -FilePath $exe -WorkingDirectory $appDir -PassThru
Start-Sleep -Milliseconds 1500

if ($proc.HasExited) {
    throw "播放器启动即退出（exit code $($proc.ExitCode)）。最常见原因是 DLL 不全：确认 `"$distBin`" 与 `"$appDir`" 都在 PATH 里。"
}

Write-Host "[run_player] 已启动 pid=$($proc.Id)" -ForegroundColor Green

$latest = Get-ChildItem -Path $logDir -Filter "soundbridge_*.log" -ErrorAction SilentlyContinue |
    Sort-Object LastWriteTime -Descending |
    Select-Object -First 1
if ($latest) {
    Write-Host "[run_player] 日志   : $($latest.FullName)" -ForegroundColor DarkGray
}

if ($Wait) {
    $proc.WaitForExit()
    Write-Host "[run_player] 已退出，exit code $($proc.ExitCode)" -ForegroundColor Yellow
    exit $proc.ExitCode
}

if ($TailLog) {
    if (-not $latest) {
        throw "还没有日志文件，可能刚启动，稍后重跑 -TailLog"
    }
    Get-Content -Path $latest.FullName -Wait -Tail 30
}
