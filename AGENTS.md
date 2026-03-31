# PROJECT KNOWLEDGE BASE

**Generated:** 2026-03-31
**Commit:** 00f89318d
**Branch:** master

## OVERVIEW
SoundBridge 是 C++11 + Qt5 Widgets 的跨平台音乐播放器；核心工作集中在 `sdk/` 的解码、重采样、设备输出、元数据提取，以及 `app/` 的桌面 UI。

## STRUCTURE
```text
./
├── app/                  # Qt Widgets 前端；看 app/AGENTS.md
├── sdk/                  # 核心库：audio / extractor / log / utils / test
│   ├── audio/            # decode / device / resample；看 sdk/audio/AGENTS.md
│   ├── extractor/        # 按格式拆分的提取器族；看 sdk/extractor/AGENTS.md
│   ├── test/             # TestSdkSuite + doctest；看 sdk/test/AGENTS.md
│   ├── cosmos/           # header-only 通用组件
│   └── 3rdparty/         # 供应商代码；默认不改
├── cmake/                # toolchain / portable packaging
├── doc/                  # 设计说明；不是运行真相源
└── music/                # 回归样本媒体
```

## AGENTS HIERARCHY
- `sdk/audio/AGENTS.md`：音频链路、SDL/FFmpeg/swr 约束、模块级回归入口。
- `app/AGENTS.md`：UI 入口、状态流、Qt 约束。
- `sdk/test/AGENTS.md`：测试入口、分组、fixture/冒烟规则。
- `sdk/extractor/AGENTS.md`：提取器工厂、格式子目录、FFmpeg/数据源陷阱。

## WHERE TO LOOK
| 任务 | 位置 | 备注 |
|------|------|------|
| 应用入口 | `app/main.cpp` | QApplication、字体与主窗体启动 |
| 主界面交互 | `app/mainwindow.cpp` | 按钮/列表/错误提示都在这里 |
| UI 与 SDK 桥接 | `app/playercontroller.cpp` | `PlayerCallbacks` → `PlayerViewState` |
| SDK 聚合入口 | `sdk/MusicPlayer.cpp`, `sdk/player_impl.cpp` | 遗留接口 + 新公开 API |
| 音频链路 | `sdk/audio/{decode,resample,device}/` | 解码、重采样、输出分层 |
| 提取器工厂 | `sdk/extractor/ExtractorFactory.cpp` | 扩展名注册 + magic sniff |
| 公共错误/状态 | `sdk/utils/ErrorUtils.h`, `sdk/ErrorCode.hpp` | 内部 `status_t` vs 对外 `ErrorCode` |
| 构建入口 | `CMakeLists.txt`, `sdk/CMakeLists.txt`, `app/CMakeLists.txt` | 根负责 toolchain / package_portable |
| 测试总入口 | `sdk/test/CMakeLists.txt`, `sdk/test/TestSdkSuite.cpp` | CTest 分组都从这里注册 |

## CODE MAP
| 符号/组件 | 位置 | 角色 |
|-----------|------|------|
| `MainWindow` | `app/mainwindow.*` | Qt 视图层；直接连按钮与列表控件 |
| `PlayerController` | `app/playercontroller.*` | 适配 `soundbridge::Player` 与 Qt signal |
| `soundbridge::Player` | `sdk/include/soundbridge/player.h`, `sdk/player_impl.cpp` | 新 API 面向 app |
| `sdk::MusicPlayer` | `sdk/MusicPlayer.*` | 遗留接口；测试仍大量覆盖 |
| `AudioStreamDecoder` | `sdk/audio/decode/AudioStreamDecoder.*` | 解码线程 + 惰性重采样 |
| `AudioDevice` | `sdk/audio/device/AudioDevice.*` | SDL 输出设备 |
| `ExtractorFactory` | `sdk/extractor/ExtractorFactory.*` | 提取器注册、sniff、实例化 |
| `TestSdkSuite` | `sdk/test/TestSdkSuite.cpp` | 自定义回归/媒体冒烟入口 |
| `UnitTests` | `sdk/test/unit/*.cpp` | doctest 单元测试 |

## CONVENTIONS
- 依赖方向固定：`utils → LogWrapper → Audio* → Extractor → sdk → app`；新功能优先放在最低可用层。
- 构建前必须先准备 `sdk/3rdparty/dist/<platform>`；根 `CMakeLists.txt` 会直接 `FATAL_ERROR`。
- 根文档只放跨仓库规则；进入 `app/`、`sdk/test/`、`sdk/extractor/` 时优先读取该子目录 AGENTS。
- 代码风格继续沿用现状：WebKit 风格、4 空格、指针/引用右靠、`#pragma once`、导入顺序“项目 → 系统 → 第三方”。
- 错误处理分两层：内部返回 `sdk_utils::status_t`，对外 API 走 `soundbridge::ErrorCode`/`sdk::ErrorCode`。

## ANTI-PATTERNS (THIS PROJECT)
- 不要改 `sdk/3rdparty/` 来“修业务问题”；先在自有代码层面适配。
- 不要跨层新增依赖或把 UI 逻辑塞回 `sdk/`。
- 不要在业务代码扩散 `#ifdef _WIN32`；编码/路径转换集中到边界工具，例如 `sdk/utils/Utf8Path.h`。
- 不要继续使用 `off64_t` 作为新增公开接口类型；Windows 兼容性差，优先 `size_t` / `uint64_t`。

## COMMANDS
```bash
# 配置（Linux x86_64）
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain/toolchain.linux_x86_64_gcc.cmake -G "Unix Makefiles"

# 构建
cmake --build build

# 最常用回归
ctest --test-dir build -R sdk_core_tests --output-on-failure
ctest --test-dir build -R sdk_decode_tests --output-on-failure
ctest --test-dir build -R sdk_unit_tests --output-on-failure
ctest --test-dir build -R sdk_ --output-on-failure

# 打包
cmake --build build --target package_portable
```

## NOTES
- `doc/` 与 `README.md` 提供背景，但真实构建/测试入口以当前 `CMakeLists.txt` 为准。
- `sdk/cosmos/` 是 header-only 工具区；规则不够特殊，暂不单独挂子 AGENTS。
- `music/` 里包含真实媒体样本；涉及 extractor / decode 的改动，优先补 `sdk_media_smoke` 或 `sdk_extractor_spec_tests`。
- 用户口头说“提交一版”时，默认含义是：`git add` + `git commit` + `git push`；除非用户显式缩小范围，否则按这三个步骤执行。
