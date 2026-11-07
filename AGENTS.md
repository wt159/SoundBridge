# PROJECT KNOWLEDGE BASE

**Generated:** 2026-04-03
**Commit:** aa517ed07
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
- **跨平台改动必须双端验证**：凡是改到平台条件编译（`#ifdef _WIN32` / `__unix__`）、POSIX 头（`dirent.h` / `sys/stat.h` / `unistd.h`）、文件或目录/进程 API，本地 Windows 编译与测试全绿**不算完成**；要逐项确认非 `_WIN32` 分支用到的每个符号都有对应头文件与实现，并等 Linux 侧 `sdk-coverage` 工作流编译通过（用 `gh run list` / `gh run watch` 核对，不能只看本地 ctest）。

## ANTI-PATTERNS (THIS PROJECT)
- 不要改 `sdk/3rdparty/` 来“修业务问题”；先在自有代码层面适配。
- 不要跨层新增依赖或把 UI 逻辑塞回 `sdk/`。
- 不要在业务代码扩散 `#ifdef _WIN32`；编码/路径转换集中到边界工具，例如 `sdk/utils/Utf8Path.h`。
- 不要继续使用 `off64_t` 作为新增公开接口类型；Windows 兼容性差，优先 `size_t` / `uint64_t`。
- 不要把 POSIX 头拆到三分支的 `#else` 里：Linux 命中 `#elif defined(__unix__)` 时会漏掉 `dirent.h` / `sys/stat.h`，`mkdir` / `opendir` / `readdir` / `closedir` 全部未声明；Windows/POSIX 二分即可，三个头统一放进非 `_WIN32` 分支（2026-11-04 CI 编译红灯，修复见 `d5c261ed1`）。

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
ctest --test-dir build -R sdk_player_tests --output-on-failure
ctest --test-dir build -R sdk_ --output-on-failure

# 覆盖率报告（Linux only，35% 门禁）
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain/toolchain.linux_x86_64_gcc.cmake -DENABLE_COVERAGE=ON -G "Unix Makefiles"
cmake --build build --target coverage
# HTML 报告：build/coverage/html/index.html
# Summary：build/coverage/summary.txt

# 打包
cmake --build build --target package_portable
```

## NOTES
- `doc/` 与 `README.md` 提供背景，但真实构建/测试入口以当前 `CMakeLists.txt` 为准。
- **CI 有两个工作流**：`sdk-coverage`（Linux x86_64，含覆盖率门禁）与 `sdk-windows`（windows-latest + MinGW）。
  两者的 `push` 都只对 `master` 生效，分支验证走 `pull_request`；需要单独验证某个分支用
  `gh workflow run <name> --ref <branch>`。Windows 侧的 3rdparty 走 `sdk/3rdparty/dist` 缓存，
  缓存命中约 10~15 分钟，冷缓存 40~90 分钟；改动 `sdk/3rdparty` 的构建输入（CMakeLists / build_option /
  toolchain / ffmpeg 与 boost 构建脚本）会让缓存失效。Windows 任务用脚本装好的 CMake 3.31.6 绝对路径，
  因为 runner 自带的 CMake 4.x 对 `cmake_minimum_required(3.5)` 直接报错。
- `sdk/cosmos/` 是 header-only 工具区；规则不够特殊，暂不单独挂子 AGENTS。
- `music/` 里包含真实媒体样本；涉及 extractor / decode 的改动，优先补 `sdk_media_smoke` 或 `sdk_extractor_spec_tests`。
- 用户口头说“提交一版”时，默认含义是：`git add` + `git commit` + `git push`；除非用户显式缩小范围，否则按这三个步骤执行。

## CODE SEARCH DISCIPLINE

Choose by the question, not by habit. CodeGraph is indexed at the repo root (`codegraph.json`); keep its scope in sync with this section.

| Question | Tool | Reason |
|---|---|---|
| Where does a literal / error string / local variable / comment appear? | Built-in Grep, **always scoped with `path`** to `app/`, `sdk/{audio,extractor,test,utils,cosmos,log,include}/`, `sdk/*.cpp`, `doc/` | CodeGraph is a symbol index: it does not see strings, locals or comments |
| Who calls X? What does changing X break? Which tests are affected? | CodeGraph CLI (`callers` / `callees` / `impact` / `affected`) or the `codegraph_explore` MCP tool | Pre-computed call graph; Grep returns lines, not relationships |
| Find a code *shape* (all `#ifdef _WIN32` blocks, all `make_unique<Foo>`) | ast-grep (install on demand) or scoped Grep | Structural match needs no index |
| Compiler-accurate definition / references under Qt macros and templates | clangd + `compile_commands.json` (Serena) | tree-sitter does not run the compiler; `#ifdef` and macro expansion are approximate |

Hard rules:
- **Never Grep from the repo root unscoped.** `sdk/3rdparty` has 84,717 tracked files (boost 74,095). Local A/B: one unscoped `git grep` = 1,831 ms vs 120 ms scoped (≈15×); a 10-query batch took 2m45s.
- `codegraph.json` is the single source of truth for index scope (`exclude` / `deprioritize` / `extensions`). `.gitignore` **cannot** exclude `sdk/3rdparty/` because those files are already tracked. Add any new first-party source directory there.
- `codegraph query` is symbol-level FTS5, not full-text. A/B: `measuredMs` → 1 symbol from CodeGraph vs 63 lines from scoped Grep. An empty CodeGraph result is NOT proof of absence; fall back to Grep.
- The index auto-syncs ~1 s after edits. Do not run `codegraph serve --mcp` by hand and do not commit `.codegraph/` (gitignored). After editing `codegraph.json`, run `codegraph index --force`.
- `git diff --name-only | codegraph affected --stdin --quiet` is a valid pre-test filter for this repo (verified: `ExtractorFactory.cpp` → 10 test files).

## Agent skills

### Issue tracker

Issues and specs live in GitHub Issues on `wt159/SoundBridge`, driven by the `gh` CLI. See `docs/agents/issue-tracker.md`.

### Triage labels

Default five-role vocabulary: `needs-triage`, `needs-info`, `ready-for-agent`, `ready-for-human`, `wontfix`. See `docs/agents/triage-labels.md`.

### Domain docs

Single-context: `GLOSSARY.md` + `docs/adr/` at the repo root, covering both `app/` and `sdk/`. See `docs/agents/domain.md`.
