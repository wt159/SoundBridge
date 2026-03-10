param(
    [string]$QtDeploy = "F:\Qt\Qt5.14.2\5.14.2\mingw73_64\bin\windeployqt.exe"
)

$ErrorActionPreference = "Stop"

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$root = Split-Path -Parent $scriptDir

$pkg = Join-Path $root "package\SoundBridge_test_runtime"
$testExe = Join-Path $root "build\sdk\test\TestSdkSuite.exe"
$thirdPartyDlls = Join-Path $root "sdk\3rdparty\dist\windows_x86_64_gcc_debug\bin\*.dll"

if (-not (Test-Path $testExe)) {
    Write-Error "TestSdkSuite.exe not found: $testExe"
}

if (-not (Test-Path $QtDeploy)) {
    Write-Error "windeployqt not found: $QtDeploy"
}

if (Test-Path $pkg) {
    Remove-Item -Recurse -Force $pkg
}
New-Item -ItemType Directory -Path $pkg | Out-Null

Copy-Item $testExe $pkg -Force
& $QtDeploy --force --compiler-runtime "$pkg\TestSdkSuite.exe" | Out-Null
Copy-Item $thirdPartyDlls $pkg -Force

Copy-Item (Join-Path $root "sdk\test\1-44100_s16le_2.pcm") $pkg -Force
Copy-Item (Join-Path $root "sdk\test\6-48000_fltp_1.aac") $pkg -Force
Copy-Item (Join-Path $root "sdk\test\music.wav") $pkg -Force

Push-Location $pkg
$tests = @("core","resample","decode","all")
$failed = $false
foreach ($t in $tests) {
    Write-Output "==> TestSdkSuite.exe $t"
    & .\TestSdkSuite.exe $t
    $code = $LASTEXITCODE
    Write-Output "exitcode=$code"
    Write-Output ""
    if ($code -ne 0) { $failed = $true }
}
Pop-Location

if ($failed) {
    exit 1
}
exit 0
