<#
.SYNOPSIS
    SoundBridge Windows 开发环境一键准备。

.DESCRIPTION
    按顺序完成：安装 CMake -> 安装 aqtinstall -> 安装 Qt 5.14.2 与 MinGW 7.3.0
    -> 编译并安装 sdk/3rdparty -> 配置根工程。
    脚本是幂等的：已存在的组件会被跳过，可反复执行。

.PARAMETER QtRoot
    Qt 安装根目录，最终路径为 <QtRoot>/<QtVersion>/<QtMingwDir>，
    同时会写入环境变量 QT_ROOT_PATH（cmake/toolchain 优先读取它）。

.PARAMETER ToolsRoot
    免安装工具目录，用于存放 CMake 与 aqt 包装脚本。

.EXAMPLE
    .\scripts\setup_windows_env.ps1 -QtRoot D:\Qt -Jobs 4

.NOTES
    需要：Windows 10/11、Python 3.9+、Git for Windows（提供 bash）、网络可访问 Qt 镜像。
#>
param(
    [string]$QtRoot = "D:\Qt",
    [string]$ToolsRoot = "D:\tools",
    [string]$CmakeVersion = "3.31.6",
    [string]$QtVersion = "5.14.2",
    [string]$QtMingwDir = "mingw73_64",
    [string]$QtToolsMingwDir = "mingw730_64",
    [string]$BuildType = "Debug",
    [int]$Jobs = 4,
    [string]$QtMirror = "https://mirrors.tuna.tsinghua.edu.cn/qt",
    [string]$PyPIIndex = "https://pypi.tuna.tsinghua.edu.cn/simple",
    [switch]$SkipQt,
    [switch]$SkipThirdParty
)

$ErrorActionPreference = "Stop"

$RepoRoot = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$ThirdPartyDir = Join-Path $RepoRoot "sdk\3rdparty"
$DistName = "windows_x86_64_gcc_$($BuildType.ToLower())"
$DistDir = Join-Path $ThirdPartyDir "dist\$DistName"

function Write-Step([string]$Message) {
    Write-Host ""
    Write-Host "=== $Message ===" -ForegroundColor Cyan
}

function Add-ToUserPath([string]$Dir) {
    if (-not (Test-Path -LiteralPath $Dir)) { return }
    $userPath = [Environment]::GetEnvironmentVariable("Path", "User")
    $parts = @()
    if ($userPath) { $parts = $userPath -split ";" | Where-Object { $_ } }
    if ($parts -notcontains $Dir) {
        [Environment]::SetEnvironmentVariable("Path", (($parts + $Dir) -join ";"), "User")
        Write-Host "  已加入用户 PATH: $Dir"
    }
}

# ---------------------------------------------------------------------------
# 1. CMake
#    必须使用 3.x：项目 cmake_minimum_required(VERSION 3.2)，
#    CMake 4.x 已移除对 <3.5 的兼容，会直接报错。
# ---------------------------------------------------------------------------
Write-Step "检查 CMake"
$cmakeBin = Join-Path $ToolsRoot "cmake-$CmakeVersion-windows-x86_64\bin"
$cmakeExe = Join-Path $cmakeBin "cmake.exe"
if (-not (Test-Path -LiteralPath $cmakeExe)) {
    $zip = Join-Path $ToolsRoot "cmake-$CmakeVersion.zip"
    New-Item -ItemType Directory -Force -Path $ToolsRoot | Out-Null
    $url = "https://github.com/Kitware/CMake/releases/download/v$CmakeVersion/cmake-$CmakeVersion-windows-x86_64.zip"
    Write-Host "  下载 $url"
    Invoke-WebRequest -Uri $url -OutFile $zip -UseBasicParsing
    Expand-Archive -Path $zip -DestinationPath $ToolsRoot -Force
    Remove-Item $zip -Force
}
Add-ToUserPath $cmakeBin
$env:Path = "$cmakeBin;$env:Path"
Write-Host "  cmake: $(& $cmakeExe --version | Select-Object -First 1)"

# ---------------------------------------------------------------------------
# 2. Qt + MinGW
# ---------------------------------------------------------------------------
$qtMingwPath = Join-Path $QtRoot "$QtVersion\$QtMingwDir"
$qtToolsBin = Join-Path $QtRoot "Tools\$QtToolsMingwDir\bin"

if (-not $SkipQt) {
    Write-Step "检查 Qt $QtVersion ($QtMingwDir) 与 MinGW"
    $needQt = -not (Test-Path -LiteralPath (Join-Path $qtMingwPath "bin\windeployqt.exe"))
    $needTool = -not (Test-Path -LiteralPath (Join-Path $qtToolsBin "g++.exe"))

    if ($needQt -or $needTool) {
        Write-Step "安装 aqtinstall"
        python -m pip install -q aqtinstall -i $PyPIIndex
        if ($LASTEXITCODE -ne 0) { throw "aqtinstall 安装失败，请检查 Python 与网络" }

        # aqt 的默认 UA 会被部分镜像返回错误页，导致校验和不匹配；这里注入 curl 风格 UA。
        $wrapper = Join-Path $ToolsRoot "aqt_wrapper.py"
        @'
"""强制 aqtinstall 使用 curl 风格 UA，避免镜像对 python-requests 返回错误页。"""
import sys

import requests

_UA = "curl/8.9.1"
_orig_request = requests.sessions.Session.request


def _patched_request(self, method, url, **kwargs):
    headers = dict(kwargs.get("headers") or {})
    headers.setdefault("User-Agent", _UA)
    kwargs["headers"] = headers
    return _orig_request(self, method, url, **kwargs)


requests.sessions.Session.request = _patched_request

from aqt.__main__ import main  # noqa: E402

if __name__ == "__main__":
    sys.exit(main())
'@ | Set-Content -LiteralPath $wrapper -Encoding UTF8

        if ($needQt) {
            Write-Step "安装 Qt $QtVersion"
            python $wrapper install-qt windows desktop $QtVersion win64_mingw73 -O $QtRoot -b $QtMirror
            if ($LASTEXITCODE -ne 0) { throw "Qt 安装失败" }
        }
        if ($needTool) {
            Write-Step "安装 MinGW 7.3.0 工具链"
            python $wrapper install-tool windows desktop tools_mingw qt.tools.win64_mingw730 -O $QtRoot -b $QtMirror
            if ($LASTEXITCODE -ne 0) { throw "MinGW 工具链安装失败" }
        }
    } else {
        Write-Host "  已存在，跳过"
    }
}

# ---------------------------------------------------------------------------
# 3. 环境变量
# ---------------------------------------------------------------------------
Write-Step "设置环境变量"
[Environment]::SetEnvironmentVariable("QT_ROOT_PATH", $QtRoot, "User")
$env:QT_ROOT_PATH = $QtRoot
Add-ToUserPath $qtToolsBin
Add-ToUserPath (Join-Path $qtMingwPath "bin")
$env:Path = "$qtToolsBin;$(Join-Path $qtMingwPath 'bin');$env:Path"
Write-Host "  QT_ROOT_PATH = $QtRoot"

# ---------------------------------------------------------------------------
# 4. 第三方库
# ---------------------------------------------------------------------------
if (-not $SkipThirdParty) {
    Write-Step "编译第三方库（Boost / FFmpeg / SDL2 / FLAC / Ogg / Vorbis / zlib / g3log / g3sinks）"
    Write-Host "  首次执行约 40~70 分钟，请耐心等待"
    $tpBuild = Join-Path $ThirdPartyDir "build"
    New-Item -ItemType Directory -Force -Path $tpBuild | Out-Null
    Push-Location $tpBuild
    try {
        # -DVERSION 用于跳过 g3log 中依赖 git 的版本号推导（该处嵌套引号在 Windows 上不可靠）
        cmake .. -G "MinGW Makefiles" `
            -DCMAKE_BUILD_TYPE=$BuildType `
            -DCMAKE_TOOLCHAIN_FILE="$RepoRoot\cmake\toolchain\toolchain.windows_x86_64_mingw.cmake" `
            -DVERSION=2.1.0 -UBUILD_OPTION_G3LOG -UBoost_USE_STATIC_LIBS
        if ($LASTEXITCODE -ne 0) { throw "第三方库配置失败" }

        cmake --build . -j $Jobs
        if ($LASTEXITCODE -ne 0) { throw "第三方库编译失败" }

        cmake --install .
        if ($LASTEXITCODE -ne 0) { throw "第三方库安装失败" }
    } finally {
        Pop-Location
    }

    # Boost 的动态库被 b2 装到了 lib/，运行期和打包都期望它们在 bin/
    $boostDlls = Get-ChildItem -Path (Join-Path $DistDir "lib") -Filter "*.dll" -ErrorAction SilentlyContinue
    if ($boostDlls) {
        $binDir = Join-Path $DistDir "bin"
        New-Item -ItemType Directory -Force -Path $binDir | Out-Null
        $boostDlls | Copy-Item -Destination $binDir -Force
        Write-Host "  已把 $($boostDlls.Count) 个 Boost DLL 复制到 bin/"
    }
}

# ---------------------------------------------------------------------------
# 5. 根工程
# ---------------------------------------------------------------------------
Write-Step "配置根工程"
$buildDir = Join-Path $RepoRoot "build"
New-Item -ItemType Directory -Force -Path $buildDir | Out-Null
Push-Location $buildDir
try {
    cmake .. -G "MinGW Makefiles" `
        -DCMAKE_BUILD_TYPE=$BuildType `
        -DCMAKE_TOOLCHAIN_FILE="$RepoRoot\cmake\toolchain\toolchain.windows_x86_64_mingw.cmake"
    if ($LASTEXITCODE -ne 0) { throw "根工程配置失败" }
} finally {
    Pop-Location
}

Write-Step "完成"
Write-Host "  编译主程序:  cmake --build build --target SoundBridge"
Write-Host "  运行:        .\build\app\SoundBridge.exe"
Write-Host "  便携打包:    cmake --build build --target package_portable"
Write-Host "  提示: 新开终端后 PATH 才会生效"
