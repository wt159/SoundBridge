# SoundBridge Music Player

一款跨平台的音乐播放器, 用来学习各类音频文件格式(aac ogg wav aiff mp3 flac m4a asf wma)的解析, 及相应的解码流程。

## 架构图

```shell
+---------------------------------------------+
|                     APP                     |
+---------------------------------------------+
                       |
                       |
+---------------------------------------------+
|                     SDK                     |
+---------------------------------------------+
|           - MusicPlayer                     |
|               - Log                         |
|               - Audio                       |
|               - Extractor                   |
+---------------------------------------------+
```

## 下载

```bash
git clone git@github.com:wt159/SoundBridge.git
```

## 编译

[**注意第一次编译请先编译sdk/3rdparty目录下的依赖库，点击跳转**](sdk/3rdparty/Readme.md)

### Windows MinGW

```bat
mkdir build && cd build
cmake .. --no-warn-unused-cli -DCMAKE_BUILD_TYPE=Debug ^
  -DCMAKE_TOOLCHAIN_FILE=../cmake/toolchain/toolchain.windows_x86_64_mingw.cmake ^
  -G "MinGW Makefiles"
cmake --build .
```

### Linux x86_64

```bash
mkdir build && cd build
cmake .. --no-warn-unused-cli -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_TOOLCHAIN_FILE=../cmake/toolchain/toolchain.linux_x86_64_gcc.cmake \
  -G "Unix Makefiles"
cmake --build .
```

### Embedded Linux ARM

```bash
mkdir build && cd build
cmake .. --no-warn-unused-cli -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_TOOLCHAIN_FILE=../cmake/toolchain/toolchain.linux_arm_gnueabihf_gcc.cmake \
  -G "Unix Makefiles"
cmake --build .
```

## 运行

### 开发态运行（构建目录）

完成编译后，可直接在 `build` 目录运行应用：

```bash
# Linux
./app/SoundBridge
```

```bat
:: Windows (cmd / PowerShell)
.\app\SoundBridge.exe
```

Windows 直接运行仅适用于 Qt 与第三方 DLL 已经可被系统搜索到的环境（例如已配置 `PATH`）。
如果出现缺少 DLL，优先使用下方 `package_portable` 流程，或先执行 `windeployqt` 并补齐
`sdk/3rdparty/dist/<windows-dist>/bin` 中的 DLL。

### Portable 运行（推荐）

在仓库根目录先执行打包，再运行打包产物，可减少缺少运行库的问题。

```bash
# Linux
cmake --build build --target package_portable
./package/SoundBridge_portable/run.sh
```

```bat
:: Windows
cmake --build build --target package_portable
start .\package\SoundBridge_portable_v3\SoundBridge.exe
```

### 常见问题

- 启动后无声音或播放失败：确认 `sdk/3rdparty/dist/<platform>` 已完成构建。
- Linux 直接运行失败：优先使用 `run.sh` 启动（会自动设置 `LD_LIBRARY_PATH`）。
- Windows 缺少 Qt/第三方 DLL：重新执行 `package_portable`，确保 `windeployqt` 路径正确。

## 进度

### 状态快照（2026-03）

#### 已完成

- 第三方库模块
- 日志模块
- 音频播放设备模块
- 音频重采样模块
- 用户界面模块
- 音频解码模块
- 音频信息模块

#### 进行中

- ASF 文件解码

#### 待办

- M4A
- AMR

### 维护约定

- 每次发布或里程碑结束后更新“状态快照”标题中的日期。
- 新增能力时优先写入“进行中”，稳定后再移动到“已完成”。
- 长期搁置项保留在“待办”，并在提交信息中关联对应任务说明。

## Windows 打包（Portable）

在完成编译后，可将可执行文件和运行依赖打包到 `package` 目录。

### 使用 CMake 目标（推荐）

以下命令在 `build` 目录执行：

```bat
# 生成后执行
cmake --build . --target package_portable
```

可选参数（首次配置时传入）：

```bat
# 自定义输出目录、windeployqt 路径与 Qt 测试 bin 目录
cmake .. -DSOUNDBRIDGE_PACKAGE_DIR="<package-dir>" ^
         -DSOUNDBRIDGE_WINDEPLOYQT="<path-to-windeployqt>" ^
         -DSOUNDBRIDGE_QT_TEST_BIN_DIR="<path-to-qt-tools-bin>"
```

### 在 `cmd.exe` 中执行

```bat
set "SB_ROOT=%CD%"
set "SB_PKG=%SB_ROOT%\package\SoundBridge_portable_v3"
set "SB_QT_DEPLOY=<path-to-windeployqt>"
set "SB_3RDPARTY_BIN=%SB_ROOT%\sdk\3rdparty\dist\<windows-dist>\bin"
powershell -NoProfile -Command "$pkg=$env:SB_PKG; if (Test-Path $pkg) { Remove-Item -Recurse -Force $pkg }; New-Item -ItemType Directory -Path $pkg | Out-Null; Copy-Item (Join-Path $env:SB_ROOT 'build\app\SoundBridge.exe') $pkg; & $env:SB_QT_DEPLOY --force --compiler-runtime \"$pkg\SoundBridge.exe\"; Copy-Item (Join-Path $env:SB_3RDPARTY_BIN '*.dll') $pkg -Force; if (Test-Path (Join-Path $env:SB_ROOT 'music')) { Copy-Item (Join-Path $env:SB_ROOT 'music') \"$pkg\music\" -Recurse -Force }"
```

### 在 `PowerShell` 中执行

```powershell
$root = (Resolve-Path .).Path
$pkg = Join-Path $root 'package\SoundBridge_portable_v3'
$qtDeploy = '<path-to-windeployqt>'
$thirdPartyBin = Join-Path $root 'sdk\3rdparty\dist\<windows-dist>\bin'
if (Test-Path $pkg) { Remove-Item -Recurse -Force $pkg }
New-Item -ItemType Directory -Path $pkg | Out-Null
Copy-Item (Join-Path $root 'build\app\SoundBridge.exe') $pkg
& $qtDeploy --force --compiler-runtime "$pkg\SoundBridge.exe"
Copy-Item (Join-Path $thirdPartyBin '*.dll') $pkg -Force
if (Test-Path (Join-Path $root 'music')) { Copy-Item (Join-Path $root 'music') "$pkg\music" -Recurse -Force }
```

## Linux 打包（Portable）

在完成编译后，可将可执行文件和依赖库打包到 `package` 目录。

### 使用 CMake 目标（推荐）

以下命令在 `build` 目录执行：

```bash
# 生成后执行
cmake --build . --target package_portable
```

打包输出位于 `package/SoundBridge_portable`，包含：
- `SoundBridge` - 主程序
- `run.sh` - 启动脚本（自动设置 `LD_LIBRARY_PATH`）
- `lib/` - 所有依赖的 `.so` 库
- `music/` - 示例音乐（可选）

### 使用方式

```bash
# 运行启动脚本
./package/SoundBridge_portable/run.sh

# 或直接运行（需手动设置库路径）
LD_LIBRARY_PATH=./package/SoundBridge_portable/lib ./package/SoundBridge_portable/SoundBridge
```

## SDK 测试（Windows）

测试依赖样本文件和 DLL，建议先使用一键脚本准备运行目录，再执行 CTest。

注意：当前仓库中的 `sdk/test/CMakeLists.txt` 通过
`SOUNDBRIDGE_QT_TEST_BIN_DIR` 缓存变量控制 Qt 测试运行目录。若本机 Qt 路径与默认配置不同，请在首次
`cmake ..` 时显式传入该变量，或优先使用 `scripts/run_sdk_tests.ps1`。

### PowerShell 一键执行

```powershell
.\scripts\run_sdk_tests.ps1
```

### 自定义 windeployqt 路径（可选）

```powershell
.\scripts\run_sdk_tests.ps1 -QtRootPath '<qt-root>' -QtVersion '<qt-version>' -QtMingwDir '<qt-mingw-dir>'
```

### CTest 分组（与 AGENTS.md 一致）

以下命令在 `build` 目录执行：

```bat
cmake --build . --target TestSdkSuite

ctest -R sdk_core_tests --output-on-failure
ctest -R sdk_resample_tests --output-on-failure
ctest -R sdk_decode_tests --output-on-failure
ctest -R sdk_all_tests --output-on-failure
```

### 轻量解封装/解码冒烟测试

针对 `music` 目录的媒体文件进行快速验证（无需整编 APP）：
以下命令在 `build` 目录执行：

```bat
cmake --build . --target TestSdkSuite
ctest -R sdk_media_smoke --output-on-failure
```

如需过滤扩展名或限制数量，可直接运行测试程序：

```bat
E:\flushbonad\github\SoundBridge\build\sdk\test\TestSdkSuite.exe media .m4a
```

### 按改动类型最小回归

- `utils / LogWrapper`：`ctest -R sdk_core_tests --output-on-failure`
- `audio/resample`：`ctest -R sdk_resample_tests --output-on-failure`
- `audio/decode`：`ctest -R sdk_decode_tests --output-on-failure`
- `extractor / 媒体格式兼容`：`ctest -R sdk_decode_tests --output-on-failure` + `ctest -R sdk_media_smoke --output-on-failure`
- `跨模块或发布前`：`ctest -R sdk_ --output-on-failure`
