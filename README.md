# SoundBridge_MusicPlayer

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
|           - MusicPlyer                      |
|               - Log                         |
|               - Audio                       |
|               - Extractor                   |
+---------------------------------------------+
```

## 下载

```shell
git clone git@github.com:wt159/SoundBridge.git
```

## 编译

[**注意第一次编译请先编译sdk/3rdparty目录下的依赖库，点击跳转**](sdk/3rdparty/Readme.md)

### windows

#### soundBridge

```shell
# 打开cmd
cd SoundBridge
# 新建build目录
mkdir build && cd build
# 修改 toolchain.windows_x86_64_mingw.cmake编译工具链
cmake .. --no-warn-unused-cli -DCMAKE_BUILD_TYPE:STRING=Debug -DCMAKE_TOOLCHAIN_FILE=%cd%\..\cmake\toolchain\toolchain.windows_x86_64_mingw.cmake -G "MinGW Makefiles"
# 编译
cmake --build .
```

### Linux

#### soundBridge

```shell
# 打开bash
cd SoundBridge
# 新建build目录
mkdir build && cd build
# 指定编译工具链
cmake .. --no-warn-unused-cli -DCMAKE_BUILD_TYPE:STRING=Debug -DCMAKE_TOOLCHAIN_FILE=../cmake/toolchain/toolchain.linux_x86_64_gcc.cmake -G "Unix Makefiles"
# 编译
cmake --build .
```

### Embedded_Linux

```shell
# 打开bash
cd SoundBridge
# 新建build目录
mkdir build && cd build
# 指定编译工具链
cmake .. --no-warn-unused-cli -DCMAKE_BUILD_TYPE:STRING=Debug -DCMAKE_TOOLCHAIN_FILE=../cmake/toolchain/toolchain.linux_arm_gnueabihf_gcc.cmake -G "Unix Makefiles"
# 编译
cmake --build .
```

## 运行

待完成

## 进度

### 已完成

* 第三方库模块
* 日志log模块
* 音频播放设备模块
* 音频重采样模块
* 用户界面模块
* 音频解码模块
* 音频信息模块

### 进行中

* asf文件解码

### 待办

* m4a
* amr

## Windows 打包（Portable）

在完成编译后，可将可执行文件和运行依赖打包到 `package` 目录。

### 使用 CMake 目标（推荐）

```shell
# 生成后执行
cmake --build . --target package_portable
```

可选参数（首次配置时传入）：

```shell
# 自定义输出目录与 windeployqt 路径
cmake .. -DSOUNDBRIDGE_PACKAGE_DIR="E:/flushbonad/github/SoundBridge/package/SoundBridge_portable_v3" ^
         -DSOUNDBRIDGE_WINDEPLOYQT="F:/Qt/Qt5.14.2/5.14.2/mingw73_64/bin/windeployqt.exe"
```

### 在 `cmd.exe` 中执行

```bat
powershell -NoProfile -Command "$pkg='E:\flushbonad\github\SoundBridge\package\SoundBridge_portable_v3'; if (Test-Path $pkg) { Remove-Item -Recurse -Force $pkg }; New-Item -ItemType Directory -Path $pkg | Out-Null; Copy-Item 'E:\flushbonad\github\SoundBridge\build\app\SoundBridge.exe' $pkg; & 'F:\Qt\Qt5.14.2\5.14.2\mingw73_64\bin\windeployqt.exe' --force --compiler-runtime \"$pkg\SoundBridge.exe\"; Copy-Item 'E:\flushbonad\github\SoundBridge\sdk\3rdparty\dist\windows_x86_64_gcc_debug\bin\*.dll' $pkg -Force; if (Test-Path 'E:\flushbonad\github\SoundBridge\music') { Copy-Item 'E:\flushbonad\github\SoundBridge\music' \"$pkg\music\" -Recurse -Force }"
```

### 在 `PowerShell` 中执行

```powershell
$pkg='E:\flushbonad\github\SoundBridge\package\SoundBridge_portable_v3'
if (Test-Path $pkg) { Remove-Item -Recurse -Force $pkg }
New-Item -ItemType Directory -Path $pkg | Out-Null
Copy-Item 'E:\flushbonad\github\SoundBridge\build\app\SoundBridge.exe' $pkg
& 'F:\Qt\Qt5.14.2\5.14.2\mingw73_64\bin\windeployqt.exe' --force --compiler-runtime "$pkg\SoundBridge.exe"
Copy-Item 'E:\flushbonad\github\SoundBridge\sdk\3rdparty\dist\windows_x86_64_gcc_debug\bin\*.dll' $pkg -Force
if (Test-Path 'E:\flushbonad\github\SoundBridge\music') { Copy-Item 'E:\flushbonad\github\SoundBridge\music' "$pkg\music" -Recurse -Force }
```

## Linux 打包（Portable）

在完成编译后，可将可执行文件和依赖库打包到 `package` 目录。

### 使用 CMake 目标（推荐）

```shell
# 生成后执行
cmake --build . --target package_portable
```

打包输出位于 `package/SoundBridge_portable`，包含：
- `SoundBridge` - 主程序
- `run.sh` - 启动脚本（自动设置 `LD_LIBRARY_PATH`）
- `lib/` - 所有依赖的 `.so` 库
- `music/` - 示例音乐（可选）

### 使用方式

```shell
# 运行启动脚本
./package/SoundBridge_portable/run.sh

# 或直接运行（需手动设置库路径）
LD_LIBRARY_PATH=./package/SoundBridge_portable/lib ./package/SoundBridge_portable/SoundBridge
```

## SDK 测试（Windows）

测试依赖样本文件和 DLL，请使用一键脚本准备运行目录并执行测试。

### PowerShell 一键执行

```powershell
.\scripts\run_sdk_tests.ps1
```

### 自定义 windeployqt 路径（可选）

```powershell
.\scripts\run_sdk_tests.ps1 -QtDeploy "F:\Qt\Qt5.14.2\5.14.2\mingw73_64\bin\windeployqt.exe"
```

### 轻量解封装/解码冒烟测试

针对 `music` 目录的媒体文件进行快速验证（无需整编 APP）：

```bat
cmake --build . --target TestSdkSuite
ctest -R sdk_media_smoke --output-on-failure
```

#### 过滤扩展名（ctest 3.25+）

```bat
ctest -R sdk_media_smoke --output-on-failure --test-action-env=SB_MEDIA_FILTER=.m4a
```

如果环境变量方式不生效，可用命令行直跑：

```bat
E:\flushbonad\github\SoundBridge\build\sdk\test\TestSdkSuite.exe media .m4a
```

#### 限制测试数量

```bat
ctest -R sdk_media_smoke --output-on-failure --test-action-env=SB_MEDIA_LIMIT=5
```
