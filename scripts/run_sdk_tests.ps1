param(
    [string]$BuildDir = "build",
    [string]$BuildType = "Debug"
)

$ErrorActionPreference = "Stop"

$Root = Resolve-Path (Join-Path $PSScriptRoot "..")
$BuildPath = Join-Path $Root $BuildDir

if (!(Test-Path $BuildPath)) {
    New-Item -ItemType Directory $BuildPath | Out-Null
}

cmake -S $Root -B $BuildPath --no-warn-unused-cli -DCMAKE_BUILD_TYPE:STRING=$BuildType -DCMAKE_TOOLCHAIN_FILE="$Root/cmake/toolchain/toolchain.windows_x86_64_mingw.cmake" -G "MinGW Makefiles" | Out-Host
cmake --build $BuildPath --target TestSdkSuite | Out-Host
ctest --test-dir $BuildPath -R "sdk_.*_tests" --output-on-failure | Out-Host
