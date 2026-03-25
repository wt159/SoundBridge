# AGENTS.md

## 项目概览
SoundBridge 是使用 C++ 和 Qt 构建的跨平台音乐播放器，提供模块化 SDK 用于解码、重采样和播放。
目标平台：Windows、Linux、嵌入式 Linux。

## 仓库结构
- `app/` Qt UI 应用（入口：`app/main.cpp`）
- `sdk/` 核心 SDK（播放器、解码、重采样、设备、提取器、日志）
- `sdk/audio/` 音频模块（decode/resample/device/common）
- `sdk/extractor/` 媒体元数据提取
- `sdk/utils/` 共享工具
- `sdk/test/` SDK 测试（`TestSdkSuite`）
- `sdk/3rdparty/` 第三方依赖构建与预构建 `dist/`
- `cmake/` 工具链与构建辅助
- `music/` 示例媒体文件（测试用）

## 技术栈
- 语言：C++11，UI：Qt5 Widgets，构建：CMake 3.2+
- 音频：FFmpeg（avcodec/avformat/avutil）+ FLAC + Ogg/Vorbis，输出：SDL2
- 日志：Boost.Log，文件系统：Boost.Filesystem
- 格式化：clang-format（`.clang-format`，WebKit 风格）
- 静态分析：无 `.clang-tidy`，仅使用 `clang-format`

## 架构与模块依赖
从底层到高层：
```
utils → LogWrapper → AudioResample/AudioDecode/AudioDevice → Extractor → sdk → SoundBridge
```

**构建目标**：`SoundBridge`（Qt App）、`sdk`、`AudioDecode`/`AudioResample`/`AudioDevice`、`Extractor`、`LogWrapper`、`utils`、`TestSdkSuite`、`package_portable`（Windows）

**约束**：禁止层间循环依赖；优先在最低合适层添加功能并向上暴露。

## 构建命令
### 前置条件
1. 构建第三方依赖（输出到 `sdk/3rdparty/dist/<platform>`）
2. 使用 `cmake/toolchain/` 中正确工具链
3. 确保 CMake 可发现 Qt5 和 SDL2

### 构建（选择对应平台）
```bash
mkdir build && cd build
cmake .. --no-warn-unused-cli -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_TOOLCHAIN_FILE=../cmake/toolchain/toolchain.<平台>.cmake \
  -G "<生成器>"
cmake --build .
```
- **Linux x86_64**：`toolchain.linux_x86_64_gcc.cmake`，生成器 `"Unix Makefiles"`
- **Windows MinGW**：`toolchain.windows_x86_64_mingw.cmake`，生成器 `"MinGW Makefiles"`
- **嵌入式 Linux ARM**：`toolchain.linux_arm_gnueabihf_gcc.cmake`，生成器 `"Unix Makefiles"`

### 打包（Windows 便携版）
```bash
cmake --build . --target package_portable
```
输出：`package/SoundBridge_portable_v3`

## 测试
### 测试框架
- **自定义框架**（非 Google Test）
- 断言：`check(condition, message)` 输出 `[PASS]` 或 `[FAIL]`
- 测试类命名：`*Test`（如 `AudioDecodeTest`）

### 运行测试
```bash
# 构建测试
cmake --build . --target TestSdkSuite

# 运行全部
ctest -R sdk_ --output-on-failure

# 运行单个组（core/resample/decode/all/media）
ctest -R sdk_<组名>_tests --output-on-failure
# 或直接运行（在 build/sdk/test 目录下）
./TestSdkSuite <组名>
```

### 媒体冒烟测试
```bash
./TestSdkSuite media [扩展名] [数量限制]
# 示例：./TestSdkSuite media .m4a 5
```

### 环境变量
- `SB_MEDIA_DIR` - 媒体目录（默认：`../../music`）
- `SB_MEDIA_FILTER` - 扩展名过滤（如 `.m4a`）
- `SB_MEDIA_LIMIT` - 文件数量限制

## 代码风格
### 格式化
- `clang-format`（WebKit 风格，4 空格缩进，100 列宽）
- 命令：`clang-format -i <file>`

### 命名约定
| 类型 | 风格 | 示例 |
|------|------|------|
| 类/结构体/枚举 | PascalCase | `MusicPlayer`、`ErrorCode` |
| 函数 | camelCase | `getAudioFormat`、`initCheck` |
| 成员变量 | m_ + camelCase | `m_impl`、`m_player` |
| 常量 | k + PascalCase | `kTag`、`kOkInfo` |
| 宏 | UPPER_SNAKE_CASE | `LOG_INFO`、`CHECK` |
| 命名空间 | lowercase | `sdk`、`sdk_utils` |

### 头文件与指针引用
- 使用 `#pragma once`（非 `#ifndef`）
- 系统头文件：`#include <vector>`，项目头文件：`#include "AudioDecode.h"`
- 指针右对齐：`int *ptr`，引用右对齐：`std::string &str`

### 大括号风格（WebKit）
```cpp
void foo()          class Foo {         if (condition) {
{                   // body              // body
    // body         };                  }
}
```
函数定义换行；类/结构体/控制语句不换行。

## 错误处理
- 状态码：`sdk_utils::status_t`（0=OK，负数=错误），定义在 `sdk/utils/ErrorUtils.h`
- 公共 API：`ErrorCode` 枚举类（`sdk/ErrorCode.hpp`），使用 `FormatError()` 格式化
- 日志宏：`LOG_INFO`/`LOG_ERROR`/`LOG_WARNING`/`LOG_DEBUG`
- 短宏：`LOGI`/`LOGE`/`LOGW`/`LOGD`（需定义 `LOG_TAG`）

## Agent 指南
- 保持 SDK 模块边界；优先在最低合适层添加功能并向上暴露
- 避免编辑 `sdk/3rdparty/`（除非更新依赖源码）
- 修改代码后运行 `clang-format` 格式化
- 运行测试验证：`ctest -R sdk_<group> --output-on-failure`
- 遵循命名约定和错误处理模式
- 使用 `#pragma once` 而非 `#ifndef`
- 不要引入循环依赖
