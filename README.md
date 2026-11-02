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

## 准备环境（Windows）

新机器从零拉起构建环境，按下面的依赖清单准备。项目只支持 **GCC/MinGW**（`cmake/toolchain/`
下没有 MSVC toolchain，编译参数也是 GCC 风格），不能用 Visual Studio 的 cl.exe。

| 组件 | 版本 | 说明 |
|------|------|------|
| Qt | 5.14.2 `mingw73_64` | 需同时安装 `Tools/mingw730_64`（含 `g++`、`mingw32-make`、`windeployqt`） |
| CMake | **3.x**（实测 3.31.6） | 不能装 4.x：根 `CMakeLists.txt` 声明 `cmake_minimum_required(VERSION 3.2)`，CMake 4 已移除对 <3.5 的兼容 |
| Git for Windows | 任意 | 构建 FFmpeg 需要其中的 `bash`、`sed`、`pr` 等 |
| Python 3.9+ | 任意 | 仅在用脚本安装 Qt 时需要（`aqtinstall`） |

Qt 的安装位置通过环境变量 `QT_ROOT_PATH` 指定，`cmake/toolchain/toolchain.windows_x86_64_mingw.cmake`
会优先读取它，未设置时回退到 `F:/Qt/Qt5.14.2`。因此换机器只需改这个环境变量，不必改工具链文件：

```bat
setx QT_ROOT_PATH D:\Qt
```

### 一键脚本（推荐）

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\setup_windows_env.ps1 -QtRoot D:\Qt
```

脚本是幂等的，会依次完成：安装 CMake（免安装 zip）→ 安装 Qt 与 MinGW（走清华镜像）→
设置 `QT_ROOT_PATH` 与 `PATH` → 编译并安装 `sdk/3rdparty` → 配置根工程。
首次执行编译第三方库约需 40~70 分钟。只想重装某一环时可加 `-SkipQt` / `-SkipThirdParty`。

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

### 一条命令启动（Windows 开发态）

`build/app` 里只有 Qt 运行时，SDL2d/FFmpeg 仍在 `sdk/3rdparty/dist/<dist>/bin` 下，
所以直接双击 `SoundBridge.exe` 会因缺 DLL **静默退出**（没有弹窗，也没有输出）。
`scripts/run_player.ps1` 负责把这层 PATH 拼好：

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\run_player.ps1

# 常用参数
#   -Rebuild   启动前增量编译 SoundBridge
#   -TailLog   跟随最新日志（Ctrl+C 只退出 tail，不影响播放器）
#   -Wait      阻塞到播放器退出，适合脚本化冒烟
```

脚本用 `Start-Process` 起进程，播放器不挂在当前终端名下。

### 调试界面（截图 / 量坐标 / 点击）

`scripts/ui.py` 只用 Python 标准库（ctypes 直连 Win32），不依赖 Pillow / pyautogui。
本机安全策略禁止 `Add-Type`，PowerShell 侧的 UIAutomation 不可用，这个脚本是替代品：

```bash
python scripts/ui.py shot  --window SoundBridge --out shot.png   # 截图，被遮挡也能截
python scripts/ui.py map   --window SoundBridge --step 10        # 亮度字符图，用来量控件坐标
python scripts/ui.py where --window SoundBridge                  # 窗口矩形 / 客户区原点
python scripts/ui.py click --window SoundBridge --client 220,649 --dry-run
```

`--client X,Y` 是**客户区坐标**（不含标题栏与边框），与 Qt 控件 `geometry()` 同一套；
工具自己加客户区原点做换算，窗口被拖动也不受影响。`--dry-run` 只打印命中窗口，不动作。
不知坐标时先跑 `map`，按字符图读像素位置 —— 注意别拿缩放的预览图量，会偏。

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
- ASF 文件解码
- M4A 文件解码
- AMR 文件解码

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

### UnitTests（Doctest）

```bash
# 构建 UnitTests
cmake --build . --target UnitTests

# 运行所有单元测试
ctest -R sdk_unit_tests --output-on-failure
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
:: (在 build 目录执行)
.\sdk\test\TestSdkSuite.exe media .m4a
```

### 按改动类型最小回归

- `utils / LogWrapper`：`ctest -R sdk_core_tests --output-on-failure`
- `audio/resample`：`ctest -R sdk_resample_tests --output-on-failure`
- `audio/decode`：`ctest -R sdk_decode_tests --output-on-failure`
- `audio/device`：`ctest -R sdk_unit_tests --output-on-failure`
- `extractor / 媒体格式兼容`：`ctest -R sdk_decode_tests --output-on-failure` + `ctest -R sdk_media_smoke --output-on-failure`
- `跨模块或发布前`：`ctest -R sdk_ --output-on-failure`
