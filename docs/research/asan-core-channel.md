# 取证通道：ASan 构建开关与 core dump 取栈（WSL/Linux）

> 取证时点：**2026-11-24**，commit **7cd1b0661**（`master`）+ 本 PR 的 `ENABLE_ASAN` 开关。
> 取证票：GitHub issue [#298](https://github.com/wt159/SoundBridge/issues/298)（ASan / core 取证通道）。
> 环境：WSL2 Ubuntu 24.04，6 vCPU / 7.8 GB，gcc 13、cmake 3.28、gdb 15.1；3rdparty 用既有预编译 `sdk/3rdparty/dist/linux_x86_64_gcc_debug`。
> 本文是**通道说明书 + 首轮实测快照**，不是判据，也不是 CI 变更：通道**不进 CI**、不做门禁、默认关。

---

## 1. 开关与边界

| 项 | 内容 |
|---|---|
| 开关 | `-DENABLE_ASAN=ON`（根 `CMakeLists.txt`，默认 `OFF`） |
| 生效范围 | 只追加编译/链接参数：`-fsanitize=address -fno-omit-frame-pointer`；FATAL_ERROR 拦非 GCC/Clang |
| 默认关的证明 | 同一 `CMakeLists.txt` 下默认配置重建 `build/sdk/CMakeFiles/sdk.dir/flags.make` 中 `fsanitize=address` 出现 **0 次**，`UnitTests` 正常重链 |
| 构建目录 | `build-asan/`（已进 `.gitignore`），与普通 `build/`、`build-cov/` 互不污染 |
| 不改的东西 | CI 两个 workflow、`sdk/test/CMakeLists.txt` 的目标清单、`sdk/3rdparty/**` |
| 不覆盖 | TSan（H4 的 SDL 句柄双关需 open/close 计数或 TSan，见 [#299](https://github.com/wt159/SoundBridge/issues/299)）、MSan、Windows/MinGW |

**已知盲区**：`sdk/3rdparty/dist` 下的 ffmpeg / SDL2 / FLAC / vorbis 是预编译产物、未插桩。堆越界/UAF 只要触及 ASan 的 malloc 拦截器仍会被抓到，但发生在这些库**内部**的栈上越界不会被发现。

---

## 2. 配置与构建（实测）

```bash
cd ~/soundbridge
cmake -S . -B build-asan \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain/toolchain.linux_x86_64_gcc.cmake \
  -DENABLE_ASAN=ON -G "Unix Makefiles"
cmake --build build-asan -j6
```

- 配置 4.0 s，全量构建 **2 m 15 s**（6 并发，冷目录）。
- 产物：`build-asan/sdk/test/UnitTests`（76 MB）、`TestSdkSuite`（21 MB）。
- 自检：`ldd build-asan/sdk/test/UnitTests | grep libasan` → `libasan.so.8`；`nm -D … | grep -c __asan_report` → **12**。两者同时成立才能说明「不是只加了编译参数、运行时真的带 ASan」。

## 3. 运行与判读

```bash
SDL_AUDIODRIVER=dummy \
ASAN_OPTIONS=detect_leaks=1:halt_on_error=0 \
ctest --test-dir build-asan -R '^sdk_' -j2 --output-on-failure
```

- **`-j2` 而非 `-j4`**：ASan 影子内存 + 4 个 `UnitTests` 进程（坏输入组各带 71 MB 素材）在 7.8 GB 上会挤，串行代价又太高；`-j2` 实测 70 s 跑完，是这台机器的稳妥档。
- **退出码语义**：ASan 报错默认 `abort_on_error=0` 但仍以非零退出；`halt_on_error=0` 让同一进程把后续错误也报完，便于首轮「一次性列全」。
- **泄漏**：Linux 上 LSan 默认随 ASan 启用（`detect_leaks=1`）。首轮 15 个目标**零泄漏报告**，说明现有测试的清理路径干净；若日后出现泄漏，先分清是生产路径还是测试夹具未 join。

### 正对照（防止「零报错」其实是假阴性）

同工具链编一个必然越界的 4 行程序：`int *p = new int[4]; p[4] = 7;`

```
==2648==ERROR: AddressSanitizer: heap-buffer-overflow on address 0x502000000020
WRITE of size 4 at 0x502000000020 thread T0
    #0 0x… in main /tmp/asan_ctl/overflow.cpp:5
```

退出码 1、栈精确到源码行。**先跑正对照、再看项目结果**，这是这条通道的使用纪律。

## 4. core dump 取栈（WSL 专用坑）

WSL 默认 `kernel.core_pattern` 是管道式：

```
|/wsl-capture-crash %t %E %p %s
```

它把 core 交给 WSL 的崩溃采集器，**工作目录里不会落 core 文件**，且默认 `ulimit -c` 为 0。要拿 core 必须先覆盖（需 root，WSL 可用 `wsl.exe -u root`，无需 sudo 密码）：

```bash
mkdir -p /tmp/sbcore && chmod 777 /tmp/sbcore
ORIG=$(cat /proc/sys/kernel/core_pattern)          # 先记原值
echo '/tmp/sbcore/core.%e.%p' > /proc/sys/kernel/core_pattern
bash -c 'ulimit -c unlimited; cd /tmp/sbcore; ./your_crashing_binary'
gdb -batch -ex 'thread apply all bt' ./your_crashing_binary /tmp/sbcore/core.your_crashing_binary.<pid>
printf '%s' "$ORIG" > /proc/sys/kernel/core_pattern  # 用完原样恢复
```

**演练（2026-11-24 实测）**：`abort()` 小程序 → 落 `core.abort_demo.1462`（311,296 字节）→ gdb 打出 `#0 __pthread_kill_implementation … #2 __GI___pthread_kill` 完整栈；`core_pattern` 已恢复为 `|/wsl-capture-crash %t %E %p %s`。

注意：core 文件属主是跑崩溃进程的用户，root 覆盖 `core_pattern` 后普通用户仍可正常落盘，只要目标目录可写。

## 5. 首轮实测结果（2026-11-24，`7cd1b0661` + 本 PR）

| 目标 | 结果 | 耗时 |
|---|---|---|
| `sdk_all_tests` / `sdk_extractor_spec_tests` / `sdk_media_smoke` | Passed | 2.05 / 0.22 / 6.14 s |
| `sdk_unit_tests` | Passed | 59.63 s |
| `sdk_badinput_*`（4 组） | Passed | 8.26 / 7.64 / 4.58 / 6.75 s |
| `sdk_streaming_tests` | Passed | 14.72 s |
| `sdk_interaction_tests` | Passed | 19.96 s |
| `sdk_app_ui_tests` | Passed | 4.08 s |
| **合计** | **15/15 passed，0 failed** | **70.11 s（`-j2`）** |

**ASan 报错清单：0 条**（含 heap-use-after-free / heap-buffer-overflow / stack-overflow / 全局越界 / 泄漏）。结合正对照与 `__asan_report` 符号数，判为**真阴性**而非通道失效。

对 [#295](https://github.com/wt159/SoundBridge/issues/295) 三假设的含义：
- H3（extractor 悬空）/ H4（重复 `_releaseAudio`）已由 [#299](https://github.com/wt159/SoundBridge/issues/299) 结构论证消除，本轮 ASan 全组零报错与之**不矛盾**；但「全组通过」不等于「这两条路径被覆盖」，覆盖口径仍以 #299 的不变量与锚点为准。
- 原始 SIGSEGV（run `42488656322`，Linux CI）在 ASan 下未复现；它在 [#142](https://github.com/wt159/SoundBridge/issues/142) 后本就未再出现，本轮不构成新的证据强度。

## 6. 复现清单

1. `git checkout <commit>`，确认 `sdk/3rdparty/dist/linux_x86_64_gcc_debug` 存在（缺则先跑 3rdparty 源码构建）。
2. 按 §2 配置构建 `build-asan`。
3. 按 §3 跑 `sdk_` 全组；先跑 §3 的正对照。
4. 需要 core 时按 §4 覆盖 `core_pattern`，用完恢复。
5. 取证产物（日志、core、报告）不入库；`build-asan/` 已在忽略列表。
