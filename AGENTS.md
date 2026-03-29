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
```

## 仓库结构
```
app/              Qt UI 应用（入口：app/main.cpp）
sdk/
  audio/          decode / resample / device / common
  extractor/      媒体元数据提取
  utils/          共享工具（ErrorUtils、FileSearch 等）
  log/            LogWrapper（Boost.Log 封装）
  test/           TestSdkSuite（自定义测试框架，单文件 TestSdkSuite.cpp）
  cosmos/         通用 C++ 工具组件（见下方说明）
  3rdparty/       第三方依赖源码 + dist/<platform>
cmake/            工具链与构建辅助
music/            示例媒体文件（测试用）
```

## 技术栈
- 语言：C++11，UI：Qt5 Widgets，构建：CMake 3.2+
- 音频：FFmpeg（avcodec/avformat/avutil）+ FLAC + Ogg/Vorbis，输出：SDL2（Linux 后端为 PulseAudio）
- 日志：Boost.Log，文件系统：Boost.Filesystem
- 格式化：clang-format（WebKit 风格，4 空格缩进，100 列宽）

## 架构与模块依赖
```
utils → LogWrapper → AudioResample/AudioDecode/AudioDevice → Extractor → sdk → SoundBridge
```
**约束**：禁止层间循环依赖；优先在最低合适层添加功能并向上暴露。

**构建目标**：`SoundBridge`、`sdk`、`AudioDecode`、`AudioResample`、`AudioDevice`、`Extractor`、`LogWrapper`、`utils`、`TestSdkSuite`、`package_portable`

## 构建命令
```bash
mkdir build && cd build
cmake .. --no-warn-unused-cli -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_TOOLCHAIN_FILE=../cmake/toolchain/toolchain.<平台>.cmake \
  -G "<生成器>"
cmake --build .
```
| 平台 | 工具链文件 | 生成器 |
|------|-----------|--------|
| Linux x86_64 | `toolchain.linux_x86_64_gcc.cmake` | `Unix Makefiles` |
| Windows MinGW | `toolchain.windows_x86_64_mingw.cmake` | `MinGW Makefiles` |
| 嵌入式 Linux ARM | `toolchain.linux_arm_gnueabihf_gcc.cmake` | `Unix Makefiles` |

### 打包
```bash
cmake --build . --target package_portable
```
- **Windows**：`package/SoundBridge_portable_v3/`，使用 `windeployqt`
- **Linux**：`package/SoundBridge_portable/`，使用 `ldd` 收集 `.so`，生成 `run.sh`

## 测试
### 测试框架
- **自定义框架**（非 Google Test），单文件：`sdk/test/TestSdkSuite.cpp`
- 断言：`check(condition, message)` 输出 `[PASS]` 或 `[FAIL]`
- 测试类命名：`*Test`（如 `AudioDecodeTest`）

### 运行测试
```bash
# CTest（推荐，在 build/ 目录）
ctest -R sdk_core_tests --output-on-failure   # utils / LogWrapper
ctest -R sdk_resample_tests --output-on-failure # AudioResample
ctest -R sdk_decode_tests --output-on-failure   # AudioDecode
ctest -R sdk_player_tests --output-on-failure   # MusicPlayer
ctest -R sdk_extractor_spec_tests --output-on-failure  # Extractor
ctest -R sdk_ --output-on-failure               # 所有 sdk 测试

# 直接运行：./TestSdkSuite [core|resample|decode|player|extractor|all|media [扩展名] [数量]]
```

### 按改动类型最小回归
| 改动范围 | 最小回归命令 |
|---------|-------------|
| utils / LogWrapper | `sdk_core_tests` |
| audio/resample | `sdk_resample_tests` |
| audio/decode | `sdk_decode_tests` |
| MusicPlayer / sdk | `sdk_player_tests` |
| extractor / 媒体格式 | `sdk_extractor_spec_tests` + `sdk_media_smoke` |
| 跨模块或发布前 | `ctest -R sdk_` |

### 测试环境变量
- `SB_MEDIA_DIR` — 媒体目录（默认：`../../music`）
- `SB_MEDIA_FILTER` — 扩展名过滤（如 `.m4a`）
- `SB_MEDIA_LIMIT` — 文件数量限制

> **Windows 注意**：若 Qt 安装路径非默认，需在 cmake 时设置
> `-DSOUNDBRIDGE_QT_TEST_BIN_DIR=<Qt bin 路径>`，或使用 `scripts/run_sdk_tests.ps1`。

## sdk/cosmos 工具组件
头文件位于 `sdk/cosmos/`，直接 `#include` 使用：`Optional<T>`、`Lazy<T>`、`NonCopyable`、`ScopeGuard`、`SyncQueue<T>`、`WorkQueue`、`ThreadPool`、`Timer`、`Variant<...>`、`Range<T>`、`SharedptrUtil`

### Lazy<T> 用法
```cpp
Optional<Lazy<std::shared_ptr<AudioResample>>> m_lazyResample;
m_lazyResample.emplace([this]() { return std::make_shared<AudioResample>(...); });
auto r = m_lazyResample->Value();           // 触发初始化
m_lazyResample = Optional<Lazy<...>>();     // 重置
```

## 代码风格
- 运行 `clang-format -i <file>` 自动格式化（WebKit 风格，4 空格，100 列）
- 指针/引用右对齐：`int *ptr`、`std::string &str`

### 命名约定
| 类型 | 风格 | 示例 |
|------|------|------|
| 类/结构体/枚举 | PascalCase | `MusicPlayer`、`ErrorCode` |
| 函数 | camelCase | `getAudioFormat`、`initCheck` |
| 成员变量 | `m_` + camelCase | `m_impl`、`m_player` |
| 常量 | `k` + PascalCase | `kTag`、`kOkInfo` |
| 宏 | UPPER_SNAKE_CASE | `LOG_INFO`、`CHECK` |
| 命名空间 | lowercase | `sdk`、`sdk_utils` |

### 头文件规范
- 使用 `#pragma once`
- **导入顺序**：项目 → 系统 → 第三方

### 类型使用
- 用 `size_t` 而非 `off64_t`（Windows 无 `sys/types.h`）
- 优先 `std::string` 而非 C 字符串
- 用 `std::unique_ptr` 管理所有权，`std::shared_ptr` 共享所有权
- 避免裸指针，除非与 C API 交互

## 错误处理
- 状态码类型：`sdk_utils::status_t`（`int32_t`，0=OK，负数=错误）
- 定义于 `sdk/utils/ErrorUtils.h`
- 常用常量：`OK`、`NO_MEMORY`、`INVALID_OPERATION`、`BAD_VALUE`、`NAME_NOT_FOUND`、
  `PERMISSION_DENIED`、`NO_INIT`、`ALREADY_EXISTS`、`DEAD_OBJECT`、`TIMED_OUT`
- 公共 API 枚举：`sdk::ErrorCode`（`sdk/ErrorCode.hpp`），使用 `FormatError()` 格式化
- 日志宏：`LOG_INFO`/`LOG_ERROR`/`LOG_WARNING`/`LOG_DEBUG`
- 短宏：`LOGI`/`LOGE`/`LOGW`/`LOGD`（需在文件顶部定义 `LOG_TAG`）

```cpp
#define LOG_TAG "MyModule"

sdk_utils::status_t result = someOperation();
if (result != sdk_utils::OK) {
    LOGE("Operation failed: %d", result);
    return result;
}
```

## 已知陷阱
- `off64_t` 在 Windows 未声明 → 用 `size_t` 替代
- `Lazy<T>::Value()` 调用 `Optional::isInit()`（小写 i）
- `Optional::destroy()` 是私有方法，重置用赋值空对象：`m_lazyX = Optional<Lazy<T>>()`
- `AudioResample` 继承 `NonCopyable`，须用 `Lazy<std::shared_ptr<AudioResample>>`
- `boost::filesystem::path::generic_string()` 在 Windows 返回 ANSI，传给 `QString::fromUtf8()` 会乱码

## 调试原则
- **先验 baseline**：用原始代码运行测试，确认原始行为
- **定位根因**：找到真正的 bug，而非急于修复
- **单变量验证**：每次只改一个变量，逐步验证
- **验证假设**：不要假设某个模式（如栈分配）一定是问题根源，参考同模块正常工作的代码

### FFmpeg Extractor 陷阱
`avformat_close_input()` 会释放 AVFormatContext，**必须先取值再关闭**：
```cpp
int64_t duration = fmt->duration;   // ✅ 先取值
avformat_close_input(&fmt);         // ✅ 再关闭
```

## 跨平台编码原则
**优先在边界做编码转换，不散播 `#ifdef _WIN32` 到业务代码。**

- 接口不变：`std::ifstream`、`std::string` 等保持原样
- 边界转换：在 I/O 边界做编码转换，业务逻辑无感知
- 工具集中：转换函数集中在 `sdk/utils/Utf8Path.h`
- 全链路 UTF-8：存储层统一 UTF-8，仅在平台边界按需转码