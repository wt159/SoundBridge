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
  实测冷缓存约 24 分钟、缓存命中约 7 分钟（2026-11-07：run 42346594655 热 7m33s；改为只装
  qtbase 后 run 42372880538 热 7m13s，setup 那一步 141s → 97s）；改动 `sdk/3rdparty` 的构建输入（CMakeLists / build_option /
  toolchain / ffmpeg 与 boost 构建脚本）会让缓存失效。Windows 任务用脚本装好的 CMake 3.31.6 绝对路径，
  因为 runner 自带的 CMake 4.x 对 `cmake_minimum_required(3.5)` 直接报错。
- **Qt 只装 qtbase，不需要全量**。项目只在 app 侧 `find_package(Qt5 Widgets)`；`UnitTests.exe`
  与 `TestSdkSuite.exe` 经 `objdump -p` 核对**不链接任何 Qt**（依赖只有 KERNEL32 / SDL2 /
  avcodec / FLAC / vorbis / MinGW 运行时）。`setup_windows_env.ps1` 默认走 `--archives qtbase`，
  需要 `windeployqt`（`package_portable` target）时加 `-FullQt`。脚本用 `bin\qmake.exe` 判断
  「Qt 是否已装」，不能用 `windeployqt.exe`——后者在 qttools 里，精简安装时不存在。
- **ctest 可以并行，前提是「每个测试进程只写自己的文件」**。2026-11-07 之前 `-j4` 的假红全部来自
  这一点：四个 `UnitTests` 进程共用 `./log/unittest_<秒级时间戳>.log`，同一秒启动就撞名，先打开的
  那个轮转/删除时另一个 `open` 直接 EIO（实测 5 轮 1 次，报 `Failed to open file for writing:
  Input/output error`）。现在日志名与临时目录都带进程号（`sdk/test/fixtures/TestProcess.hpp` 的
  `sb_test::processId()`），坏输入 55 例按实测耗时切成四段（24/27/15/28 s）摊到各核，
  `sdk-windows.yml` 用 `ctest -j4`。实测：串行 197.58s → `-j4` 58~76s；`-j4` 连跑 10 轮、
  `-j8` 超订连跑 5 轮全绿。新增用例若往固定名字的共享路径写东西，会把这个稳定性还回去。
- **`if:` 不含状态检查函数时，Actions 会自动套一层隐式 `success()`**。所以「构建成功就回写缓存」这类
  步骤必须显式写 `(!cancelled()) && ...`，否则前面的用例一红它就跟着被跳过（2026-11-07 踩过：
  冷构建 24 分钟做完、缓存没写回，下一轮又白等一次冷构建）。
- **判定 CI 成败只认 `gh run view <id> --json status,conclusion,attempt`**。`gh run watch <id> --exit-status | tail` 未加 `set -o pipefail` 时退出码取 `tail` 的 0，失败 run 会被看成成功（2026-11-08 踩过一次假绿）。纯 `doc/` / `docs/` / `AGENTS.md` 提交不触发 CI：两个 workflow 的 `push` 与 `pull_request` 都带 `paths` 过滤（`CMakeLists.txt` / `cmake/**` / `app/**` / `scripts/**` / `sdk/**` / workflow 自身），这类提交不出现在 run 列表里属预期，别误判成「CI 挂了没跑」。
- **判 flake 四条证据**：① 同一代码在 PR run 已绿；② 失败目标不在本次改动文件；③ 该目标常态耗时吻合；④ `gh run rerun <id> --failed` 全绿。Linux 侧有低概率单目标 flake（2026-11-08：`sdk_badinput_rejection_tests` 的 `getMusicCount()==1` 20s 等待超时；PR #111 合并后 `sdk_interaction_tests` 的 `seek to middle moves position forward` 还出现过 `SIGSEGV` 进程崩溃、14 断言全过的新形态）。**别为此放宽超时或改动 BadInput 55 例基线**，只做一次 rerun 取证。当 ② 不成立时用替代取证：比对 merge commit 与 PR 分支头的 tree（相同即逐字节同代码）、用 doctest suite 过滤器确认失败用例与新增用例零交集、确认生产包装层默认关闭时行为等价。日志里 grep 到的 `sigsegv` 多数是 g3log 示例 `main_sigsegv.cpp` 的编译输出，别当成历史崩溃。
- **异步事件的会话起点一律按事件内容定位，不要用固定快照下标猜**（2026-11-09 `afterMonotonic` 基线假红，[#115](https://github.com/wt159/SoundBridge/issues/115) / [#116](https://github.com/wt159/SoundBridge/issues/116)）：`sdk/test/unit/test_player_integration.cpp` 的 `position callbacks stay monotonic and a legal seek still rewinds` 曾用 `beforeCount+1` 当「seek 后的新会话起点」，但 `Player::seek` 经 `m_workQueue.asyncRunTask` 异步生效，`first` 快照到 seek 生效之间旧会话仍会追加 position；只要追加 ≥1 个值，合法回退 100 就落在 `beforeCount+k`，被 `afterMonotonic` 误判为「新会话内回退」→ `sdk_interaction_tests` 低概率假红（11-09 PR #114 稳定性对照两组各 9/10、11-08 C1/C5 master 9/10 同源）。正解是在序列中定位实际回退点 `rewindAt`（首个 `< before`），只检查其后的新会话单调；真回退仍会被抓到、断言条数与所有数值不变，**这不是放宽判据**。修判据前必须先抓到失败序列实证（临时诊断只写 stdout、取证后零残留），再做「注入真回退必红 / 还原绿」的变异验证与同轮数 `-j4` 对照；备选「生产 `resetFloor` 二次上报」若成立须另开新票，不在测试票里改生产代码。**本 effort 只解决基线假红**；`42488656322` 的 SIGSEGV 根因仍未定位（无复现、无栈，CI=Linux / 本机=Windows 不可外推），不随本票修复。#115 / #116 已按全套闸门放行，PR [#117](https://github.com/wt159/SoundBridge/pull/117) 合并为 `b482b900e`；实测修复版 10/10 全绿 0 假红、master 同轮数 9/10 且第 5 轮自然复现同一假红。
- **新增测试专用钩子（哪怕默认关闭）也要走稳定性闸门**：release/acquire 内存序、只在控制线程读写、绝不落解码线程 / 消费主循环 / 判据主循环；放行前必须做 master 与分支**同轮数** `ctest -j4` 对照（2026-11-08 C1/C5：分支 10/10 vs master 9/10 才放行），单目标串行绿不算证据。落在解码线程或判据主循环的额外写会把 `-j4` 假红率从 0 抬到约 30%（O1 对照实验）。（2026-11-09 反例：#112 / #113 / PR #114 的 `stop()` 清槽 + 两个测试钩子，虽通过变异验证、两轴 review 与 PR 阶段 CI 双绿，但分支两组 `-j4` 各 9/10、master 同轮数 10/10，按「假红率高于基线即撤回」的闸门**全部撤回、PR 关闭、改动未进 master**；取证与 resolution 见 [PR #114](https://github.com/wt159/SoundBridge/pull/114)。CI 双绿不能替代同轮数对照；失败用例与改动零交集也不是事后放宽闸门的理由。）
- **`SIGSEGV` flake 的「反复出现」门槛（方向②闸门）**：截至 2026-11-08，全历史 50 个 `sdk-coverage` run 里 `test case CRASHED: SIGSEGV` **仅 1 次**（push run `42488656322` attempt 1，`sdk_interaction_tests` / `test_player_integration.cpp:726` 的 `seek to middle moves position forward`），`rerun --failed` 后 attempt 2 全绿；其余 6 个历史 `failure` 分别是编译错误、`skip not allowed in CI`、断言失败、第三方 boost 拷贝、`Build coverage target` 等待超时，形态都不同。**满足任一才算「反复出现」、才另开票重审**：① 不同 `headSha` 上同形态 SIGSEGV ≥2 次且 `rerun --failed` 不能稳定消解；② 本机 `ctest -j4` 同轮数对照中该用例可稳定复现（≥2/10）；③ 拿到崩溃栈 / ASan 报告指向具体竞态。单次偶发 + rerun 全绿 → 维持观测，不改判据 / TIMEOUT / 坏输入 55 例基线。取证：`gh run list --workflow sdk-coverage.yml --limit 50 --json databaseId,conclusion,attempt` 列候选，再对每个 `failure` run 用 `gh run view <id> --log-failed | grep -Ei "CRASHED|SIGSEGV"`；**run 已是 attempt 2 成功时 `--log-failed` 看不到 attempt 1**，须用 `gh api repos/<owner>/<repo>/actions/runs/<id>/attempts/1/jobs` 补查（注意 `gh api` 不吃 `-R`）。 （2026-11-09 补：门槛②已实测 —— 本机 Windows / MinGW730 跑 10 轮 `ctest -j4` 全套 14/14 全绿（57–60s/轮，`sdk_interaction_tests` 13.58–14.21s）、**0 次 SIGSEGV**；另用 `UnitTests -ts="MusicPlayer Interactive Paths" -tc="seek to middle moves position forward"` 单用例连跑 100 次，**0 崩溃 0 非零退出**（127s）。**平台局限**：CI 那次 SIGSEGV 发生在 Linux ubuntu-24.04，本地实验是 Windows，不能等价外推；三条门槛至 11-09 均未命中，维持观测，不另开票、不改生产代码。）**2026-11-09 一次性破例留痕**：用户明确指令「强制重审」，主动打破本条门槛、不等第二次出现，开 [#112](https://github.com/wt159/SoundBridge/issues/112) / [#113](https://github.com/wt159/SoundBridge/issues/113) 并出 PR #114；该 effort 事前写死边界「无复现、无崩溃栈、CI 在 Linux / 本机在 Windows，交付状态契约加固 + 判据 + 稳定性对照，**不是修复 SIGSEGV**」，且因上述稳定性闸门未过而**全部撤回，终态仍是不改生产代码**。三条门槛本身不删、不放宽，本次破例不构成后续自动启动 effort 的先例；SIGSEGV 根因仍未定位，H1 在 Windows 侧 240 次窗口命中 0 崩溃已证伪、H2 未改、H3/H4 未验。取证与反悔条件见 `doc/player-state-contract.md` §10.4 与 `doc/streaming-playback-spec.md` §12.6。（2026-11-09 第十八轮补充：合并 PR #117 后的 push run `42607915236` attempt 1 又在 `sdk_interaction_tests` / `test_player_integration.cpp:726` 报**同形态 SIGSEGV**（14 断言全过、6.26s、headSha `b482b90`），是不同 `headSha` 上的第 **2** 次（首次 `8b36430`）；同 run 另有 `sdk_unit_tests` / `MusicPlayer Error Handling / play without track triggers error callback:256` 的 `CHECK(error_count > 0)` 断言失败（新形态）。`gh run rerun 42607915236 --failed` → attempt 2 `success`，与首次一样被 rerun **稳定消解**，故条件 ① 的「`rerun --failed` 不能稳定消解」**仍未满足**；② 本机 0/10、0/100；③ 仍无崩溃栈 / ASan。**三条门槛仍未命中，维持观测，不另开票、不改生产代码**，别把「第 2 次出现」直接读成门槛命中。本次「失败目标不在改动文件」不成立（同文件），按登记的替代取证：tree 对比 `209782555` vs `b482b900e` 为空、doctest suite 过滤器确认失败用例属 `MusicPlayer Error Handling` / `MusicPlayer Interactive Paths` 而与改动的 `PublicPlayer Interactive Paths` 零交集、PR 阶段同代码 run `42603640630` 已绿（常态 `sdk_unit_tests` 16.01s / `sdk_interaction_tests` 12.26s）、rerun 全绿。run 已是 attempt 2 时 `--log-failed` 看不到 attempt 1，须用 `gh api repos/<owner>/<repo>/actions/runs/<id>/attempts/1/jobs` 补查。）
- **真触发时 seek 竞态的最小重审范围**（2026-11-08 只读勘察；日志无崩溃栈，以下均未证实为根因）：① `AudioStreamDecoder::seekToMs()`（`AudioStreamDecoder.cpp:219-233`）只写 `m_seekRequestMs` 单槽，native RA 定位在解码线程 `exchange(-1)` 后执行；`cancelRequested()` 把「槽内有值」当取消（`:457`），seek 期间刻意不置 `m_abortDecode`（`:226-231`，#59/#60/#65 已知时序坑），而 stop 靠 `m_stopRequest + m_abortDecode` 中断读回调 —— **「seek 槽未消费完就 stop」是最可疑的叠加窗口**。② 析构期已有 issue #70 的 `SDL_CloseAudioDevice` 双释放防护（`MusicPlayer.cpp:153-157`：`m_workQueue.shutdown()` 必须先于 `_releaseAudio()`）；`stop()` 是异步队列任务（`:219`），`_stop()` 已走过一次 `_releaseAudio()`（`:369-370`），析构再走一次，重复路径幂等性无专门判据覆盖。③ **已排除**：`m_seekBaseMs` / `m_seekDiscardBytes` 虽非 atomic，但只在解码线程内读写（seek 分支 `:538/:614/:677/:975` 与 EOS 汇总 `:345/:353/:366`）；消费线程经 `MusicPlayer.cpp:555` 调的 `positionMs()`（`:430-441`）只读 atomic `m_segmentBaseBytes` + `m_ring->readPos()`，**不构成数据竞争**，别重复怀疑这条。
- **Windows 上非 ASCII 文件名必须按宽字符打开**：`sdk/extractor/FileSource.cpp` 用 `_wfopen`
  + `sdk_utils::toWideString`；`toNativeString` 是 UTF-8 → ANSI(`CP_ACP`) 的有损转换，
  在 en-US runner 上中文变 `?`，只保留给确实需要 ANSI 的老 API。不要再用它打开文件。
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
