# AGENTS.md

## 项目概览
SoundBridge 是使用 C++ 和 Qt 构建的跨平台音乐播放器，提供模块化 SDK 用于解码、重采样和播放。
目标平台：Windows、Linux、嵌入式 Linux。

## 快速开始（Agent 30 秒）
```bash
# 1) 配置（以 Linux x86_64 为例）
mkdir build && cd build
cmake .. --no-warn-unused-cli -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_TOOLCHAIN_FILE=../cmake/toolchain/toolchain.linux_x86_64_gcc.cmake \
  -G "Unix Makefiles"

# 2) 构建
cmake --build .

# 3) SDK 核心回归
ctest -R sdk_core_tests --output-on-failure

# 4) 媒体冒烟（可选）
ctest -R sdk_media_smoke --output-on-failure
```

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
- 格式化：clang-format（`.clang-format`，WebKit 风格，4 空格缩进，100 列宽）
- 静态分析：无 `.clang-tidy`，仅使用 `clang-format`

## 架构与模块依赖
从底层到高层：
```
utils → LogWrapper → AudioResample/AudioDecode/AudioDevice → Extractor → sdk → SoundBridge
```

**构建目标**：`SoundBridge`（Qt App）、`sdk`、`AudioDecode`/`AudioResample`/`AudioDevice`、`Extractor`、`LogWrapper`、`utils`、`TestSdkSuite`、`package_portable`（Windows/Linux）

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

### 打包
```bash
cmake --build . --target package_portable
```
- **Windows**：输出 `package/SoundBridge_portable_v3`，使用 `windeployqt` 部署 Qt 依赖
- **Linux**：输出 `package/SoundBridge_portable/`，使用 `ldd` 自动收集 `.so` 库，生成 `run.sh` 启动脚本

## 测试
### 测试框架
- **自定义框架**（非 Google Test）
- 断言：`check(condition, message)` 输出 `[PASS]` 或 `[FAIL]`
- 测试类命名：`*Test`（如 `AudioDecodeTest`）

### 单元测试
```bash
# 构建测试
cmake --build . --target TestSdkSuite

# 运行全部
ctest -R sdk_ --output-on-failure

# 运行单个组
ctest -R sdk_core_tests --output-on-failure
ctest -R sdk_resample_tests --output-on-failure
ctest -R sdk_decode_tests --output-on-failure
ctest -R sdk_all_tests --output-on-failure

# 或直接运行（在 build/sdk/test 目录下）
./TestSdkSuite core
./TestSdkSuite resample
./TestSdkSuite decode
./TestSdkSuite all
```

### 媒体冒烟测试
```bash
./TestSdkSuite media [扩展名] [数量限制]
# 示例：./TestSdkSuite media .m4a 5

# 等效 CTest 用例名（固定）
ctest -R sdk_media_smoke --output-on-failure
```

### 环境变量
- `SB_MEDIA_DIR` - 媒体目录（默认：`../../music`）
- `SB_MEDIA_FILTER` - 扩展名过滤（如 `.m4a`）
- `SB_MEDIA_LIMIT` - 文件数量限制

### 按改动类型最小回归
- **utils / LogWrapper**：`ctest -R sdk_core_tests --output-on-failure`
- **audio/resample**：`ctest -R sdk_resample_tests --output-on-failure`
- **audio/decode**：`ctest -R sdk_decode_tests --output-on-failure`
- **extractor / 媒体格式兼容**：`ctest -R sdk_decode_tests --output-on-failure` + `ctest -R sdk_media_smoke --output-on-failure`
- **跨模块或发布前**：`ctest -R sdk_ --output-on-failure`

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

### 导入顺序
1. 项目头文件（如 `#include "AudioDecode.h"`）
2. 系统头文件（如 `#include <vector>`）
3. 第三方库（如 `#include <libavformat/avformat.h>`）

### 大括号风格（WebKit）
```cpp
void foo()          class Foo {         if (condition) {
{                   // body              // body
    // body         };                  }
}
```
函数定义换行；类/结构体/控制语句不换行。

### 类型使用
- 使用 `size_t` 而非 `off64_t`（跨平台兼容）
- 优先使用 `std::string` 而非 C 风格字符串
- 使用 `std::unique_ptr` 管理所有权，`std::shared_ptr` 共享所有权
- 避免原始指针，除非与 C API 交互

## 错误处理
- 状态码：`sdk_utils::status_t`（0=OK，负数=错误），定义在 `sdk/utils/ErrorUtils.h`
- 公共 API：`ErrorCode` 枚举类（`sdk/ErrorCode.hpp`），使用 `FormatError()` 格式化
- 日志宏：`LOG_INFO`/`LOG_ERROR`/`LOG_WARNING`/`LOG_DEBUG`
- 短宏：`LOGI`/`LOGE`/`LOGW`/`LOGD`（需定义 `LOG_TAG`）

### 错误处理示例
```cpp
#define LOG_TAG "MyModule"

sdk_utils::status_t result = someOperation();
if (result != sdk_utils::OK) {
    LOGE("Operation failed: %d", result);
    return result;
}
```

## 已知编译问题
- `off64_t` 未声明：使用 `size_t` 替代（跨平台兼容，Windows 无 `sys/types.h`）
- GCC 13+ 缺少 `<string>` 头文件：`std::unordered_map<std::string, ...>` 场景需显式包含
- 指针比较 `ptr <= 0`：应改为 `ptr == nullptr` 或 `*ptr == 0`

## 提交规范
- 使用 `fix:`、`feat:`、`chore:`、`docs:` 等前缀
- 示例：`fix: resolve cross-platform compilation issues`
- 示例：`feat: add Linux portable packaging support`
- 示例：`chore: add .cache to .gitignore`

## .gitignore
- `build/` - 构建输出
- `package/` - 打包输出
- `.cache/` - clangd 索引缓存
- `sdk/3rdparty/dist/` - 第三方依赖预构建
- `sdk/3rdparty/ffmpeg-4.4.4/` - FFmpeg 源码

## Agent 指南
### Do
- 保持 SDK 模块边界；优先在最低合适层添加功能并向上暴露
- 修改代码后运行 `clang-format -i <file>`
- 按改动类型执行最小回归，至少运行对应 `sdk_*` 测试
- 遵循命名约定、错误处理模式与 `#pragma once` 规则
- 跨平台相关修改在 Windows 与 Linux 都验证编译
- 若 `AGENTS.md` 与 `README.md` 冲突，以 `AGENTS.md` 为执行准，并在合适时同步 README

### Don't
- 不要引入层间循环依赖
- 不要编辑 `sdk/3rdparty/`（除非明确是依赖源码升级）
- 不要引入不必要头文件，保持依赖最小化
- 不要破坏现有 PIMPL 封装边界（如 `MusicPlayer`）
