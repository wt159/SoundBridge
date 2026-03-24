# AGENTS.md

## 项目概览
SoundBridge 是一个使用 C++ 和 Qt 构建的跨平台本地音乐播放器。
它提供模块化 SDK，用于解码、重采样和播放，并包含一个链接 SDK 的 Qt UI 应用。
项目目标平台为 Windows、Linux 和嵌入式 Linux。

## 仓库结构
- `app/` Qt UI 应用（入口：`app/main.cpp`，UI：`app/mainwindow.cpp`）
- `sdk/` 核心 SDK（播放器、解码、重采样、设备、提取器、日志）
- `sdk/audio/` 音频流水线模块（解码/设备/重采样/通用）
- `sdk/log/boost_log/` 日志封装（Boost.Log）
- `sdk/extractor/` 媒体元数据/提取
- `sdk/utils/` 共享工具
- `sdk/test/` SDK 测试（`TestSdkSuite`）
- `sdk/3rdparty/` 第三方构建脚本与预构建 `dist/`
- `cmake/` 工具链与构建辅助
- `scripts/` 辅助脚本（Windows 测试运行器）
- `music/` 示例媒体文件（测试使用）
- `package/` 便携打包输出
- `build/` 本地构建输出（生成目录）

## 技术栈
- 语言：C++17
- UI：Qt5 Widgets
- 构建：CMake 3.2+
- 音频解码：FFmpeg（avcodec/avformat/avutil）+ FLAC + Ogg/Vorbis
- 音频输出：SDL2
- 日志：Boost.Log
- 文件系统：Boost.Filesystem
- 格式化：clang-format（配置：`.clang-format`，基于 WebKit 风格）

## 架构（SDK）
分层由 CMake 链接依赖约束：
- `sdk/` 公共 API：`MusicPlayer`、`MusicPlayList`、`LogApi`、`Metrics`、`ErrorCode`
- `sdk/audio/decode/` 解码（FFmpeg、FLAC、Ogg/Vorbis）
- `sdk/audio/resample/` 重采样（swresample）
- `sdk/audio/device/` 播放设备（SDL2）
- `sdk/extractor/` 元数据与提取
- `sdk/log/boost_log/` 日志后端
- `sdk/utils/` 共享工具

**约束**：禁止在层间引入循环依赖；优先在最低合适层添加新功能并向上暴露。

## 构建前置条件
1. 先构建第三方依赖（输出到 `sdk/3rdparty/dist/<platform>`）。
2. 使用 `cmake/toolchain/` 中正确的工具链进行配置。
3. 确保 CMake 可发现 Qt5 和 SDL2。

依赖构建步骤见 `sdk/3rdparty/Readme.md`。

## 构建命令
### Windows（MinGW）
```bash
mkdir build && cd build
cmake .. --no-warn-unused-cli -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_TOOLCHAIN_FILE=../cmake/toolchain/toolchain.windows_x86_64_mingw.cmake \
  -G "MinGW Makefiles"
cmake --build .
```

### Linux（x86_64）
```bash
mkdir build && cd build
cmake .. --no-warn-unused-cli -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_TOOLCHAIN_FILE=../cmake/toolchain/toolchain.linux_x86_64_gcc.cmake \
  -G "Unix Makefiles"
cmake --build .
```

### 嵌入式 Linux（arm-linux-gnueabihf）
```bash
mkdir build && cd build
cmake .. --no-warn-unused-cli -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_TOOLCHAIN_FILE=../cmake/toolchain/toolchain.linux_arm_gnueabihf_gcc.cmake \
  -G "Unix Makefiles"
cmake --build .
```

## 打包（Windows 便携版）
```bash
cmake --build . --target package_portable
```
便携输出默认位于 `package/SoundBridge_portable_v3`。

## 测试
### 运行全部 SDK 测试
```bash
cmake --build . --target TestSdkSuite
ctest -R sdk_ --output-on-failure
```

### 运行单个测试组
可用组：`core`、`resample`、`decode`、`all`、`media`
```bash
# 通过 ctest
ctest -R sdk_core_tests --output-on-failure
ctest -R sdk_resample_tests --output-on-failure
ctest -R sdk_decode_tests --output-on-failure
ctest -R sdk_all_tests --output-on-failure
ctest -R sdk_media_smoke --output-on-failure

# 直接运行测试二进制（需在 build/sdk/test 目录下）
./TestSdkSuite core
./TestSdkSuite resample
./TestSdkSuite decode
./TestSdkSuite media
./TestSdkSuite all
```

### 媒体冒烟测试
```bash
ctest -R sdk_media_smoke --output-on-failure
# 或直接运行（支持过滤扩展名和限制数量）
./TestSdkSuite media .m4a
./TestSdkSuite media .m4a 5
```

### Windows 测试辅助脚本
```powershell
.\scripts\run_sdk_tests.ps1
.\scripts\run_sdk_tests.ps1 -QtDeploy "path\to\windeployqt.exe"
```

## 代码风格
### 格式化
- 使用 `clang-format`（配置在 `.clang-format`）
- 风格：基于 WebKit，缩进 4 空格，行宽 100 列
- 格式化命令：`clang-format -i <file>`

### 命名约定
| 类型 | 风格 | 示例 |
|------|------|------|
| 类/结构体 | PascalCase | `MusicPlayer`、`ErrorCodeInfo` |
| 枚举类 | PascalCase | `ErrorCode`、`MusicPlayerState` |
| 枚举值 | PascalCase | `PlayingState`、`FileOpenFailed` |
| 函数 | camelCase | `getAudioFormat`、`initCheck` |
| 成员变量 | m_ 前缀 + camelCase | `m_impl`、`m_player` |
| 常量 | k 前缀 + PascalCase | `kTag`、`kOkInfo` |
| 宏 | UPPER_SNAKE_CASE | `LOG_INFO`、`CHECK` |
| 命名空间 | lowercase | `sdk`、`sdk_utils` |

### 头文件与指针引用
- 使用 `#pragma once`（非 `#ifndef` 守卫）
- 系统头文件：`#include <vector>`，项目头文件：`#include "AudioDecode.h"`
- 指针右对齐：`int *ptr`、`const char *name`
- 引用右对齐：`std::string &str`

### 大括号风格（WebKit）
- 函数定义：换行；类/结构体/控制语句：不换行
```cpp
void foo()
{
    // body
}

class Foo {
    // body
};

if (condition) {
    // body
}
```

## 错误处理
### 状态码
- SDK 使用 `sdk_utils::status_t`（0 = OK，负数 = 错误）
- 定义在 `sdk/utils/ErrorUtils.h`

### 错误码与日志
- 公共 API 使用 `ErrorCode` 枚举类（定义在 `sdk/ErrorCode.hpp`）
- 使用 `FormatError()` 格式化错误信息
- 使用 `LogWrapper` 宏：`LOG_INFO`、`LOG_ERROR`、`LOG_WARNING`、`LOG_DEBUG`
- 短宏：`LOGI`、`LOGE`、`LOGW`、`LOGD`（需定义 `LOG_TAG`）

## Agent 指南
- 保持 SDK 模块边界完整；优先在最低合适层添加新功能并向上暴露。
- 避免编辑 `sdk/3rdparty/` 下的内容，除非更新依赖源码。
- Windows 工具链路径应通过 CMake 缓存变量保持可配置。
- 修改代码后运行 `clang-format` 格式化。
- 运行测试验证修改：`ctest -R sdk_<group> --output-on-failure`。
- 遵循命名约定和错误处理模式。
- 使用 `#pragma once` 而非 `#ifndef` 守卫。
- 不要引入循环依赖。
