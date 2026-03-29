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
- 音频：FFmpeg（avcodec/avformat/avutil）+ FLAC + Ogg/Vorbis，输出：PulseAudio（Linux）/ SDL2
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
# 构建测试二进制
cmake --build . --target TestSdkSuite

# CTest 运行（推荐，在 build/ 目录下）
ctest -R sdk_core_tests      --output-on-failure   # utils / LogWrapper
ctest -R sdk_resample_tests  --output-on-failure   # AudioResample
ctest -R sdk_decode_tests    --output-on-failure   # AudioDecode
ctest -R sdk_player_tests    --output-on-failure   # MusicPlayer
ctest -R sdk_extractor_spec_tests --output-on-failure  # Extractor
ctest -R sdk_all_tests       --output-on-failure   # 全部单元测试
ctest -R sdk_media_smoke     --output-on-failure   # 媒体冒烟
ctest -R sdk_                --output-on-failure   # 所有 sdk 测试

# 直接运行单个分组（在 build/sdk/test/ 目录下）
./TestSdkSuite core
./TestSdkSuite resample
./TestSdkSuite decode
./TestSdkSuite player
./TestSdkSuite extractor
./TestSdkSuite all
./TestSdkSuite media [扩展名] [数量限制]
# 示例：./TestSdkSuite media .m4a 5
```

> **没有「单个测试用例」粒度的运行方式**。最小粒度是分组（group），如 `core`、`decode` 等。
> 需要隔离单个用例时，在 `TestSdkSuite.cpp` 中临时注释掉其他用例后重新构建。

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
头文件位于 `sdk/cosmos/`，已通过 CMake include path 暴露，直接 `#include` 即可。

| 组件 | 说明 |
|------|------|
| `Optional<T>` | 类似 std::optional（C++11 兼容），用 `isInit()` 判断是否有值 |
| `Lazy<T>` | 懒加载包装，首次调用 `Value()` 时执行工厂函数；`IsValueCreated()` 查询状态 |
| `NonCopyable` | 禁止拷贝的基类，SDK 内部类广泛继承 |
| `ScopeGuard` | RAII 作用域退出回调 |
| `SyncQueue<T>` | 线程安全队列 |
| `WorkQueue` | 单线程任务队列 |
| `ThreadPool` | 简单线程池 |
| `Timer` | 定时器工具 |
| `Variant<...>` | 类型安全联合体（类似 std::variant） |
| `Range<T>` | 范围迭代辅助 |
| `SharedptrUtil` | shared_ptr 辅助函数 |

### Lazy<T> 使用示例
```cpp
#include "Lazy.hpp"
#include "Optional.hpp"

// 声明
Optional<Lazy<std::shared_ptr<AudioResample>>> m_lazyResample;

// 注册工厂（捕获 this 可在触发时读取最新状态）
auto factory = [this]() -> std::shared_ptr<AudioResample> {
    AudioSpec in = m_srcSpec;   // 触发时读取，非构造时
    in.samples   = 1024;
    auto r = std::make_shared<AudioResample>(in, m_devSpec);
    return (r && r->initCheck() == sdk_utils::OK) ? r : nullptr;
};
m_lazyResample.emplace(factory);

// 触发初始化
auto r = m_lazyResample->Value();

// 重置（切换曲目时）
m_lazyResample = Optional<Lazy<std::shared_ptr<AudioResample>>>();
```

## 代码风格
### 格式化（.clang-format — WebKit）
- 命令：`clang-format -i <file>`（修改代码后必须运行）
- 缩进：4 空格，列宽 100，禁用 Tab
- 指针/引用右对齐：`int *ptr`、`std::string &str`（`PointerAlignment: Right`）
- 连续赋值对齐：`AlignConsecutiveAssignments: true`
- 连续宏定义对齐：`AlignConsecutiveMacros: true`
- 函数定义大括号换行，类/控制语句大括号不换行
- `short if` 不允许单行：`AllowShortIfStatementsOnASingleLine: Never`

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
- 使用 `#pragma once`（禁用 `#ifndef` 守卫）
- **导入顺序**：① 项目头文件 → ② 系统头文件 → ③ 第三方库
  ```cpp
  #include "AudioDecode.h"          // 1. 项目
  #include <string>                  // 2. 系统
  #include <libavformat/avformat.h>  // 3. 第三方
  ```

### 类型使用
- 用 `size_t` 而非 `off64_t`（Windows 无 `sys/types.h`）
- 优先 `std::string` 而非 C 字符串
- 用 `std::unique_ptr` 管理所有权，`std::shared_ptr` 共享所有权
- 避免裸指针，除非与 C API 交互
- `AudioFormat` 枚举默认值为 `AudioFormatUnknown(-1)`；压缩格式（WMA 等）解码前 format 未知，需懒初始化处理

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
- Windows 平台避免使用 POSIX 专属类型和头文件
- `Lazy<T>::Value()` 内部调用 `Optional::isInit()`（小写 i），不是 `IsInit()`
- `Optional::destroy()` 是私有方法，重置请用赋值空对象：`m_lazyX = Optional<Lazy<T>>()`
- `AudioResample` 继承 `NonCopyable`，不可直接放入 `Lazy<AudioResample>`，须用 `Lazy<std::shared_ptr<AudioResample>>`
- `boost::filesystem::path::generic_string()` 在 Windows 上返回 ANSI 本地编码（非 UTF-8），传给 `QString::fromUtf8()` 会乱码

## 跨平台编码原则：转换接口代替平台分支

**优先在边界做编码转换，不要散播 `#ifdef _WIN32` 到业务代码中。**

错误做法：用平台分支改写业务逻辑（`std::ifstream` → `FILE*`，`seekg` → `fseek`……）：
```cpp
// ❌ 不要这样做
#ifdef _WIN32
    FILE *m_file = _wfopen(wpath, L"rb");
    fread(data, 1, size, m_file);
#else
    std::ifstream m_file(path, std::ios::binary);
    m_file.read(data, size);
#endif
```

正确做法：保持标准接口不变，只在调用前做一次编码转换：
```cpp
// ✅ 转换接口代替平台分支
// sdk/utils/Utf8Path.h
inline std::string toNativeString(const char *utf8) {
#ifdef _WIN32
    // UTF-8 → wstring → ANSI（Windows fopen 期望 ANSI 编码）
    int wsize = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0);
    std::wstring ws(wsize - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, &ws[0], wsize);
    int asize = WideCharToMultiByte(CP_ACP, 0, ws.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string ansi(asize - 1, '\0');
    WideCharToMultiByte(CP_ACP, 0, ws.c_str(), -1, &ansi[0], asize, nullptr, nullptr);
    return ansi;
#else
    return utf8 ? std::string(utf8) : std::string();
#endif
}

// FileSource.cpp — 业务代码无平台分支
FileSource::FileSource(const char *filename)
    : m_file(sdk_utils::toNativeString(filename), std::ios::binary)  // 唯一的变化
    , m_offset(0)
    , m_length(-1)
{ }
```

**原则总结**：
1. **接口不变**：`std::ifstream`、`std::string` 等标准接口保持原样
2. **边界转换**：在 I/O 边界（文件打开、Qt 显示）做编码转换，业务逻辑无感知
3. **工具集中**：转换函数集中在 `sdk/utils/Utf8Path.h`，不散播到业务文件
4. **全链路 UTF-8**：存储层统一 UTF-8，仅在平台边界按需转码