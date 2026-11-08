# CI 观测现状与数值基线 · 事实核对（票 #95 / 地图 #94）

> 本文件由 `research` skill 流程产出：逐条回到一手来源（workflow 文本、源码、规格、真实 GitHub Actions job 日志）核对，每个断言附 `文件:行号` 或 CI run id / 日志行号。
> **本票只给事实与证据，不做任何决策、不改代码、不关票、不动其他 issue。** 凡与地图 Notes 现状快照或票面措辞冲突，以实测为准并在文末「更正」段显式列出。

## 基线与方法

- 仓库 `wt159/SoundBridge`，基线提交 `master @ e86e9bd5d`（Merge PR #93），核对前工作区干净。
- 实测 CI（均为 `sdk-coverage` workflow、conclusion `success`，`gh run view` / `gh api` 核实）：
  - run `42442116861`，**attempt 2**，job `128617560798`，createdAt `2026-11-08T03:17:50Z`，headBranch `master`；日志 5309739 B / 24653 行。
  - run `42442016087`，attempt 1，job `128613044475`，createdAt `2026-11-08T03:16:25Z`，headBranch `implement/player-state-contract`；日志 5309760 B / 24656 行。
- 日志下载：`curl -sL -H "Authorization: Bearer $(gh auth token)" -H "Accept: application/vnd.github+json" "https://api.github.com/repos/wt159/SoundBridge/actions/jobs/<job_database_id>/logs" -o "$TEMP/soundbridge-ci-logs/job-<id>.log"`，随后用 Grep 工具**限定 path 到这两个具体日志文件**做检索（未对 `sdk/3rdparty` 做任何全仓搜索）。
- 「出现次数」口径：Grep `count` 模式给出的**日志行数**，含 `-s` 展开的 `SUCCESS: CHECK(...)` 表达式行与 `MESSAGE:` 数值行；`packetsPulled` 计数包含其子串 `packetsPulledAtFirstPcm`；`packets <数字>` 为数值打印行的独立计数。两次 run 的七项计数**完全一致**。
- 下文中「日志 A」= job `128617560798`，「日志 B」= job `128613044475`；`L<n>` 为该日志的行号。

---

## 表 1 · 仪表（`StreamMetrics` 字段清单与 CI 可见性）

| 字段 | 定义（`文件:行号`） | 量纲 | 日志 A 实测值（run 42442116861） | 出现次数 A / B | 日志 B 实测值（run 42442016087） |
|---|---|---|---|---|---|
| `peakHeldBytes` | `sdk/audio/decode/StreamMetrics.h:19` `size_t`；本次播放为当前曲目持有的音频数据字节数，覆盖 payload/包索引/页表/seek 表，不含 ring 与 AVIO 缓冲（`:14-18`） | 字节 | `4608`、`33325`、`50749`、`4608/4608`、`33325/33325`、`27042`、`25239`、`20539`、`127180`、`65536`、`17354`、`30434`、`156943`、`497465`、`33325` 等（如 `test_streaming_budget.cpp:294` MESSAGE `L24514`、`:599` `L24529`、`:974` `L24557`/`L24578`、`:1402` `L24600-24620`） | **24 / 24** | 同组结构值 `4608`/`33325`/`50749`（`L24517`/`L24521`/`L24532`） |
| `silenceFills` | `StreamMetrics.h:25` `size_t`；注释 `:20-24`：「`shouldFillSilence()` 不含 ring 为空的条件，取不到数据（`got == 0`）时消费端静默跳过本轮、不填静音，因此只覆盖 shuttingDown / switching / SEEKING 三类窗口」，「判的是稳态窗口内静音填充为零，**不是无欠流**」 | 次数 | MESSAGE 中均为 `silenceFills 0`（`L24514`/`L24518`）；另有 `CHECK(steadyRun.metrics.silenceFills == 0)`（`test_streaming_budget.cpp:918`，`L24549`）、`CHECK(seekRun.metrics.silenceFills > 0)`（`:928`，`L24550`）均 `SUCCESS` | **7 / 7** | 同（`L24517`/`L24521`，`L24552`/`L24553`） |
| `startToFirstPcmUs` | `StreamMetrics.h:28` `uint64_t`；`start()` → 首次把 PCM 交给消费端（ring 写入成功）的微秒数（`:26-27`） | 微秒 µs | `837`、`1457`、`671`、`662/593`、`1222/1054`（`L24514`/`L24518`/`L24529`/`L24557`/`L24578`） | **12 / 12** | `605`、`894`、`310`、`374/353`、`660/591`（`L24517`/`L24521`/`L24532`/`L24560`/`L24581`） |
| `initScanUs` | `StreamMetrics.h:32` `uint64_t`；提取器 init 只读扫描耗时；注释 `:29-31` 说明「当前链路 init 仍是全量装载 payload，E2 改成只读扫描后同一测量点自动变成只读扫描耗时」 | 微秒 µs | `20`、`5511`、`12/11`、`8921/8713`（`L24514`/`L24518`/`L24557`/`L24578`） | **11 / 11** | `23`、`3675`、`11/11`、`4933/4907`（`L24517`/`L24521`/`L24560`/`L24581`） |
| `packets` / `packetsPulled` / `packetsPulledAtFirstPcm` | `StreamMetrics.h:36`、`:38`；`:33-37` 说明二者构成「首帧交付时供给序列尚未耗尽」的结构证据 | 包数 | `packets 2851`（`:599` `L24529`）、`packets 11321`（mp3 `:649` `L24534`）、`9517`（aac `L24539`）、`48`/`13`/`74`/`3010`/`130`/`6`/`94`/`5634`（`L24547`/`L24583`/`L24588`/`L24600-24620`）、`packetsPulled 0`（ogg `:739` `L24541`）、`packetsPulledAtFirstPcm` 出现在 `:511`/`:513`/`:552`/`:555` 等 CHECK 行 | `packetsPulled`（含子串）**39 / 39**；`packets <数字>` **18 / 18** | 同结构值（`L24532`/`L24537`/`L24542`/`L24544`/`L24550`/`L24586`/`L24591`/`L24603-24623`） |
| `boundaryMicros`（= `packetBoundaryMicros`） | `StreamMetrics.h:42` `uint64_t`；现场重算包边界的 CPU 成本（µs，累计），mkv 起的格式非 0，其余恒 0（`:39-41`）。`sdk-coverage.yml:89-90` 注释与 `spec:232` 均说明「MESSAGE 里 `packetBoundaryMicros` 打印为 `boundaryMicros`」 | 微秒 µs | mkv `278`（`:834` `L24547`）、m4a `427`（`L24600`）、alac `354`（`L24605`）、ape `97`（`L24610`）、asf `338`（`L24615`）、wma `6541`（`L24620`），均在 `test_streaming_budget.cpp:1402` MESSAGE | **7 / 7** | mkv `84`（`L24550`）、m4a `115`（`L24603`）、alac `59`（`L24608`）、ape `24`（`L24613`）、asf `124`（`L24618`）、wma `417`（`L24623`） |

**结论（四个字段到底有没有数值）**：`peakHeldBytes` / `silenceFills` / `startToFirstPcmUs` / `initScanUs` **四个字段在两个成功 run 的 CI 日志里全部出现且带具体数值**（并非「一个都没出现」）；`boundaryMicros` / `packets` / `packetsPulled` 也全部出现。观测 step（`sdk-coverage.yml:91-96`）确实把 `-ts=StreamingBudget -s` 的 MESSAGE 打进了日志；日志 A `L24449` 为该 step 的 `##[group]Run LD_LIBRARY_PATH=...` 起始（`03:54:06.918`），`L24451-24452` 为命令回显，字段行集中在 `L24511-24620`（`03:54:18.845`~`03:54:24.594`）。

**数值稳定性**：结构性字段跨两次 run 一致（`peakHeldBytes` 4608 / 33325 / 50749、`packets` 2851、`silenceFills 0`）；时间类字段**不稳定**——`initScanUs` 20 vs 23、5511 vs 3675；`startToFirstPcmUs` 837 vs 605、1457 vs 894；`boundaryMicros` mkv 278 vs 84、m4a 427 vs 115、wma 6541 vs 417（约 3~16 倍差）。

**观测 step 在什么条件下才输出**：step 只在 `./build/sdk/test/UnitTests -ts=StreamingBudget -s` 的输出经 `grep -E "peakHeldBytes|silenceFills|startToFirstPcmUs|initScanUs|packets|boundaryMicros"` 有匹配行时才产生日志内容；`continue-on-error: true`（`sdk-coverage.yml:92`）与 `|| true`（`:96`）保证命令失败 / grep 无匹配 / 素材未下发 / 断言红都不让 CI 失败。素材未下发时 `test_streaming_budget.cpp:1011` 只打 `MESSAGE("[SKIP] ... (素材未随仓库下发)")`，不含六个 grep 关键字，会被过滤 → 该 step 输出为空。本次两个 run 素材齐备、断言全绿，故输出完整。

---

## 表 2 · 指标（生产链两个指标的量纲与 CI 可见性）

| 指标 | 日志行格式 / 生成点（`文件:行号`） | 量纲 | 生产链落点 | CI 日志是否可见 |
|---|---|---|---|---|
| `track_switch_ms` | `sdk/Metrics.cpp:23-42` `LogMetric()` 拼 `metric=<name> type=<type> value=<value> unit=<unit>`（`:27-30`），`LogMessage(SdkLogLevel::Info, "Metrics", oss.str())`（`:41`）；`sdk/Metrics.cpp:46-49` `RecordTiming(const char *name, uint64_t ms, ...)` → `LogMetric("timing", name, std::to_string(ms), tags, "ms")` | 毫秒 ms，`unit=ms` | `sdk/MusicPlayList.cpp:429-433`（`Metrics::RecordTiming("track_switch_ms", static_cast<uint64_t>(switchMs), MetricTags{...})` 在 `:431`）；票面给的 `402-433` 覆盖该切歌窗口 | **不可见**：日志 A/B Grep `metric=\|track_switch_ms\|open_file_ms` 均 `No matches found`（0 匹配） |
| `open_file_ms` | 同上 `Metrics::RecordTiming`（`sdk/Metrics.cpp:46-49`），量纲同为 ms | 毫秒 ms，`unit=ms` | `sdk/MusicPlayList.cpp:286-290`（`auto openMs = duration_cast<milliseconds>(openEnd - openStart).count()` 在 `:286-287`；`Metrics::RecordTiming("open_file_ms", static_cast<uint64_t>(openMs), MetricTags{...})` 在 `:288-290`，字面量在 `:289`）；票面给的 `:288` 即该调用起始行 | **不可见**（同上，0 匹配） |

- 量纲一致性：`doc/streaming-playback-spec.md:242-243`（§12.2）登记 `open_file_ms`（毫秒）/`track_switch_ms`（`metric=track_switch_ms type=timing value=<ms> unit=ms`）/`initScanUs`（微秒），与实现一致；两个生产链指标与 `StreamMetrics` 的三个 µs 字段（`startToFirstPcmUs`/`initScanUs`/`packetBoundaryMicros`）**量纲不同**。
- 不可见原因（事实）：CI 只构建并运行测试二进制（`UnitTests` / `ctest -R sdk_`）与 `coverage`，未运行播放器生产链；`Metrics` 日志经 `sdk/Metrics.cpp:41` 的 `LogMessage` 走日志库（g3log 文件日志），不落到 Actions stdout。
- 对照：CI 日志里**可见**的是 `sdk-coverage.yml:74-79` 覆盖度矩阵步骤的 15 行 `declared/measured`（日志 A `L24433-24448`，日志 B `L24436-24451`），与观测 step 的四字段 MESSAGE——二者都是 doctest `-s` 输出，不是 `metric=` 行。

---

## 表 3 · 素材矩阵 15 行（格式 × 时长形状）

定义：`sdk/test/unit/test_media_coverage.cpp:36-50`（15 行 `{文件名, 扩展名}`），`MESSAGE` 在 `:117`，汇总 `coverage matrix covered 15/15 file(s)` 在 `:163`；`doc/format-support.md:33-36` 记「15 份素材覆盖 11 个扩展名，用例在 `MediaCoverage` 套件，随 `ctest -R sdk_unit_tests` 运行，净耗时约 2.5 秒」。数值取自日志 A `L24433-24448`（日志 B `L24436-24451` 逐项相同）。容差口径 `sdk/test/fixtures/RealMediaFixture.hpp:80-95`：`max(declared×1%, 150ms)`，上限 `declared/4`。

| # | 文件（`music/`） | 扩展名 | declared | measured | tolerance | 时长形状 | 同格式长短配对 |
|---|---|---|---|---|---|---|---|
| 1 | `music.wav` | `.wav` | 24869 ms | 24869 ms | 248 ms | 短 24.9 s | `.wav` 两首（#1/#2）都短，非长短配对 |
| 2 | `ring.wav` | `.wav` | 19214 ms | 19214 ms | 192 ms | 短 19.2 s | 同上 |
| 3 | `48000_fltp_1.aac` | `.aac` | 10048 ms | 10048 ms | 150 ms | 短 10.0 s | **有**：#3 短 / #4 长（10 s vs 221 s，约 22×） |
| 4 | `WavinFlag.aac` | `.aac` | 220984 ms | 220984 ms | 2209 ms | 长 221.0 s | 同上 |
| 5 | `music-ape.ape` | `.ape` | 24869 ms | 24869 ms | 248 ms | 短 24.9 s | 仅 1 首，无配对 |
| 6 | `music-mkv.mkv` | `.mkv` | 24576 ms | 24576 ms | 245 ms | 短 24.6 s | 仅 1 首，无配对 |
| 7 | `俱乐部音乐循环-Freesound.aiff` | `.aiff` | 27428 ms | 27428 ms | 274 ms | 短 27.4 s | 仅 1 首，无配对 |
| 8 | `小镇姑娘-陶喆.flac` | `.flac` | 297693 ms | 297693 ms | 2976 ms | 长 297.7 s | 仅 1 首，无配对 |
| 9 | `像我这样的人-毛不易.128.mp3` | `.mp3` | 207542 ms | 207542 ms | 2075 ms | 长 207.5 s | `.mp3` 两首（#9/#10）都长，非长短配对 |
| 10 | `消愁-毛不易.320.mp3` | `.mp3` | 261407 ms | 261407 ms | 2614 ms | 长 261.4 s | 同上 |
| 11 | `Ringtones-耳聆网.ogg` | `.ogg` | 3709 ms | 3709 ms | 150 ms | 极短 3.7 s（全矩阵最短） | **ogg 仅 1 首，无配对** |
| 12 | `摇滚乐_Freesound.m4a` | `.m4a` | 69892 ms | 69892 ms | 698 ms | 中长 69.9 s | **有**：#12 中长 / #13 短（69.9 s vs 12 s，约 5.8×）；但 `doc/format-support.md:27` 说明两首编码不同（AAC 与 ALAC） |
| 13 | `music-alac.m4a` | `.m4a` | 12000 ms | 12000 ms | 150 ms | 短 12.0 s | 同上 |
| 14 | `萨克斯机.asf` | `.asf` | 34874 ms | 34737 ms | 348 ms | 中 34.9 s（−0.39%，唯一负偏差之一） | 仅 1 首，无配对 |
| 15 | `十七岁-陶喆.96.wma` | `.wma` | 259164 ms | 261642 ms | 2591 ms | 长 259.2 s（+0.96%，余量仅 113 ms） | **wma 仅 1 首，无配对** |

- 结论：**坐实「素材矩阵缺口」**——`ogg` 1 首、`wma` 1 首，均无同格式长短配对（`doc/streaming-playback-spec.md:256` §12.4 第 3 条 / #67 的描述今天仍成立）。真正有同格式长短配对的是 `.aac`（#3/#4）与 `.m4a`（#12/#13，跨编码）；`.wav`/`.mp3` 各有两首但同量级；其余 7 个扩展名各 1 首。
- `doc/format-support.md:60-64` 与 CI 实测一致：wma `259164→261642ms`（+2478ms / 容差 2591ms，1% 容差用掉 95.6%）、asf `34874→34737ms`（−137ms），其余 13 份 0.00%。
- `music/` 目录共 21 个文件（Glob 实测），矩阵只用 15 个；未入矩阵的 6 个：`music-au.au`、`music-nsp.nsp`、`music-w64.w64`、`music-wwsig.wsig`、`小镇姑娘-陶喆.128.mp3`（第 3 首 mp3）、`小镇姑娘-陶喆.96.amr`；`doc/format-support.md:89-95` 把 `.amr`/`.opus`/`.au`/`.nsp`/`.w64`/`.wsig` 列为不支持或从未注册。
- 文档内部口径差：`RealMediaFixture.hpp:81` 注释「实测最大偏差是 asf 的 +0.20%，留 5× 余量」与 `format-support.md:60-64`（wma +0.96%、asf −0.39%、余量 113 ms）不一致；`sdk-coverage.yml:71` 注释称「覆盖度矩阵的 **14 行** declared/measured」，而 `test_media_coverage.cpp:36-50`、`:163` 与 CI 日志 `L24448` 均为 **15 行 / 15/15**。

---

## 表 4 · CI 步骤（workflow 原文 + 实测输出）

| 工作流 / 来源 | step（名称与行号） | 命令与 grep 模式（原文） | `continue-on-error` / `\|\| true` | CI 实测 |
|---|---|---|---|---|
| `.github/workflows/sdk-coverage.yml:74-79` | `Report coverage matrix numbers` | `set -o pipefail`；`LD_LIBRARY_PATH=... ./build/sdk/test/UnitTests -ts=MediaCoverage -s \| grep -E "declared \|coverage matrix covered"` | **无** `continue-on-error`、**无** `\|\| true`（`pipefail` 下 grep 无匹配即失败，是门禁性步骤） | 日志 A `L24412` group，`L24433-24448` 输出 15 行 `declared/measured` + `coverage matrix covered 15/15 file(s)`；日志 B `L24436-24451` |
| `.github/workflows/sdk-coverage.yml:81-90`（注释）、`:91-96`（step） | `Report streaming budget instruments (observation only, non-gating)`（`:91`） | `:94` `LD_LIBRARY_PATH="${{ github.workspace }}/sdk/3rdparty/dist/linux_x86_64_gcc_debug/lib" \`；`:95` `./build/sdk/test/UnitTests -ts=StreamingBudget -s \`；`:96` `\| grep -E "peakHeldBytes\|silenceFills\|startToFirstPcmUs\|initScanUs\|packets\|boundaryMicros" \|\| true` | **有** `continue-on-error: true`（`:92`）、**有** `\|\| true`（`:96`） | 日志 A `L24449` group 起始、`L24451-24452` 命令回显、`L24511-24620` 字段行；日志 B `L24454-24455`、`L24514-24623`。四字段均有数值（见 表 1） |
| `.github/workflows/sdk-coverage.yml:81-90` 注释 | — | 原文要点：四个观测点由 `StreamingBudget` suite 的 `MESSAGE(...)` 打印；doctest 默认只在断言失败时进日志、通过时数值不可见（`:82-84`）；纯观测、不设阈值、不注册 ctest 分组、不改测试分组与超时、不改覆盖度矩阵报告步骤，代价是新增一次 StreamingBudget 测试运行（`:85-88`）；`packetBoundaryMicros` 打印为 `boundaryMicros`、包数打印为 `packets`/`packetsPulled`（`:89-90`） | — | 与实测一致（观测 step 输出可见）；但「不注册 ctest 分组、不动 TIMEOUT」已与今天 CMakeLists 现状不符，见「文档偏差清单」 |
| `.github/workflows/sdk-windows.yml:128-132` | `Run SDK tests` | `ctest --test-dir build -R sdk_ -j4 --output-on-failure` | — | Windows 侧 |
| `.github/workflows/sdk-windows.yml:147-157` | `Report coverage matrix numbers` | `set -o pipefail`；`./build/sdk/test/UnitTests.exe -ts=MediaCoverage -s > coverage.log 2>&1`；`grep -E "declared \|coverage matrix covered" coverage.log`（`:154-157`） | `continue-on-error: true`（`:148`），无 `\|\| true`（`:145-146` 注释说明「失败也不阻塞 PR」） | 只跑 `MediaCoverage` |
| `.github/workflows/sdk-windows.yml`（全文） | **无** `StreamingBudget` 等价 step | Grep `StreamingBudget\|streaming budget\|peakHeldBytes\|silenceFills` → **`No matches found`（0 匹配）**；`:136` 注释「与 Linux workflow 的同名步骤对齐」仅指覆盖度矩阵步骤 | — | **坐实：windows 侧没有同名或等价的 streaming budget instruments 观测 step** |
| `scripts/coverage/run_linux.sh:29`（由 `sdk/test/CMakeLists.txt:249-254` 的 `coverage` target 间接触发） | 隐式 ctest | `ctest --test-dir "${BUILD_DIR}" -R sdk_ --output-on-failure` | — | 日志 A `L23767-23780`：`Start 10..14: sdk_*`、`13/14 Test #13: sdk_streaming_tests ...... Passed 17.60 sec`（`L23774`）、`100% tests passed, 0 tests failed out of 14`（`L23778`）、`Total Test time (real) = 80.44 sec`（`L23780`）；日志 B `L23781-23783`：同样 14/14 全绿、`67.17 sec` |

附：`sdk/test/CMakeLists.txt` 的 **14 个 `sdk_*` ctest 目标**（`add_test` 行号）：`sdk_core_tests`(`:73`)、`sdk_resample_tests`(`:81`)、`sdk_decode_tests`(`:89`)、`sdk_player_tests`(`:97`)、`sdk_all_tests`(`:105`)、`sdk_media_smoke`(`:114`)、`sdk_extractor_spec_tests`(`:122`)、`sdk_unit_tests`(`:196`)、`sdk_badinput_rejection_tests` / `sdk_badinput_frames_tests` / `sdk_badinput_duration_tests` / `sdk_badinput_tags_tests`（`:209-220` foreach 由 `SB_TEST_BAD_INPUT_SUITES` `:184-188` 生成）、`sdk_streaming_tests`(`:223`)、`sdk_interaction_tests`(`:234`)。
**`StreamingBudget` 有独立 ctest 目标 `sdk_streaming_tests`**（`:223-226`，`-ts=${SB_TEST_STREAMING_SUITES}`，`SB_TEST_STREAMING_SUITES "StreamingBudget"` 在 `:191`），并被 `sdk_unit_tests` 的 `-tse=` 显式排除（`:198`）。
**TIMEOUT 挂在 7 个组**：`sdk_unit_tests` `TIMEOUT 180`（`:204`，注释 `:203`「快集实测约 30 s，180 s 留 6 倍余量」）、4 个 badinput 各 `TIMEOUT 120`（`:219`，注释 `:207` 单段 24/27/15/28 s）、`sdk_streaming_tests` `TIMEOUT 180`（`:231`，注释 `:230`「实测 59 s」）、`sdk_interaction_tests` `TIMEOUT 180`（`:243`，注释 `:242`「14 例 / 11.8 s」）；其余 7 个 legacy 目标无 TIMEOUT。CI 实测两个 run 均 14/14 全绿、**TIMEOUT 未命中**。

---

## 表 5 · 播放器可观测面

| 项 | `文件:行号` | 记录内容 | 消费用例（`sdk/test/unit/`） | 留存 |
|---|---|---|---|---|
| `MockPlayerCallbacks::event_sequence` | `sdk/test/mocks/PlayerMocks.hpp:29`（`std::vector<std::string>`，注释 `#86 / T01`「六个回调按到达顺序留档，供『曲目三连』『error→state』等契约判据复用」）；写入点 `:66`(state)、`:120`(track)、`:128`(duration)、`:136`(position)、`:144`(playlist)、`:156`(error)；getter `:75-79`；辅助 `firstEvent` `:88-96`、`countEvents` `:98-107` | 六个回调的事件名序列（`"state"`/`"track"`/`"duration"`/`"position"`/`"playlist"`/`"error"`），按到达顺序 | `test_player_integration.cpp:261-266`、`:1321-1326`、`:1433-1448`、`:1523` 等（详见下方断言数字） | **无跨运行留存**：`reset()` `:38-58` 在 `:53-55` 清空 `state_sequence`/`event_sequence`/`position_values` 与全部计数；纯进程内 `std::vector`，无文件/DB/环境变量持久化 |
| `MockPlayerCallbacks::position_values` | `PlayerMocks.hpp:31`（`std::vector<uint64_t>`，注释 `#88 / T03`「位置值序列，用于断言同曲单调 / 换曲归零 / seek 合法回退」）；唯一写入点 `:137`（`onPositionChanged`）；getter `:81-85` | 每次 `onPositionChanged` 的 `positionMs` 值序列 | `test_player_integration.cpp:1256-1290`、`:1367-1391` | 同上，无留存 |
| `MockPlayerCallbacks::state_sequence` | `PlayerMocks.hpp:27`（`std::vector<PlayerState>`，注释 `#72`「状态序列按到达顺序留档，才能断言相邻状态不重复」）；写入 `:65`；getter `:69-73` | 状态枚举序列 | `test_player_integration.cpp:1523-1530`、`:1571-1589` | 同上，无留存 |
| `MockMusicPlayerListener::event_sequence` | `PlayerMocks.hpp:197`（注释 `#86 / T01`「内部 listener 也留一份事件序列」）；写入 `:226`/`:234`/`:242`/`:250`/`:257`/`:269`；`reset()` `:201-219`（`:216` 清空） | 旧 listener 的事件名序列，**无** `position_values` | `test_player_integration.cpp:261-264` | 同上，无留存 |

**现有断言数字（回调顺序 / 幂等 / 状态序列）**，全部在 `sdk/test/unit/test_player_integration.cpp`：
- 错误路径：`countEvents(events, "error") == 1`（`:262`、`:1326`）、`countEvents(events, "state") == 0`（`:264`、`:1322`）、`countEvents(events, "position") == 0`（`:1324`）、`REQUIRE_FALSE(events.empty())`（`:266`）、`player.position() == 0`（`:1328`）。
- 位置序列：`positionValues().size() >= 3` 且 `last_position > 500`（`:1256-1257`）、`>= 2`（`:1368`）、`> beforeCount`（`:1384`）、单调性循环 `first`/`after`（`:1260-1290`）。
- 回调顺序：`firstEvent(events, "track") < firstEvent(events, "duration") < firstEvent(events, "position")`（`:1434-1439`）、`durationInWindow == 1`（`:1448`「首个曲目切换窗口内只应报一次 duration」）、`last_duration == player.duration()`（`:1449`）。
- 状态序列 / 幂等：`!duplicate`（相邻状态不重复，`:1530`、`:1589`）、`!states.empty()`（`:1582`）、`states.front() == Playing`（`:1584`）、`states.back() == Stopped`（`:1586`）；`TEST_CASE("repeated control commands do not duplicate state callbacks")`（`:1452-1453`，`#91 / T06` 规格 §4）、`:1520`「Stopped 下重复 stop() 发了额外的状态回调」。
- 这些序列只在单个测试进程内由 `reset()` 重置，**没有任何跨版本 / 跨运行的历史留存或基线比对机制**。

---

## 补充事实：稳态欠流（§12.3 今天是否仍成立）

- 计数点：`sdk/audio/decode/AudioStreamDecoder.cpp:293-304` `noteSilenceFill()`；守卫 `:300` `if (current != StreamDecoderState::SEEKING && !m_stopRequest.load() && !m_abortDecode.load()) return;`，计数 `:303` `m_silenceFills.fetch_add(1, std::memory_order_relaxed)`；清零 `:111`（`start()` 时 `m_silenceFills.store(0)`），读出 `:255`（`out.silenceFills = m_silenceFills.load()`）。
- `shouldFillSilence()` 定义 `sdk/MusicPlayer.cpp:482-505`：`:484` `isShuttingDown() || m_switching` → true；`:488` `task == nullptr || task->ring == nullptr || task->decoder == nullptr` → true；`:501` `cur == nullptr || task->trackIndex != cur->index` → true；`:504` `return false`。**函数体不读 ring 空条件**（无 `ring->empty()` / `available()` 判断），与 `StreamMetrics.h:21-23`、`spec:221` 的描述一致。`:507-515` `consumeAudioData()` 仅在 `shouldFillSilence()` 为真时写 4096 B 静音并记账。
- `got == 0` 不计数：`sdk/audio/decode/AudioStreamDecoder.cpp:905-909` `got = m_ring->write(...)`；`if (got == 0) { std::this_thread::sleep_for(std::chrono::milliseconds(1)); continue; }` —— ring 写不进时静默跳过本轮，不填静音、不 `fetch_add`。
- 判据仍在用且通过：`test_streaming_budget.cpp:918` `steadyRun.metrics.silenceFills == 0`、`:928` `seekRun.metrics.silenceFills > 0`；日志 A `L24549`/`L24550`、日志 B `L24552`/`L24553` 均 `SUCCESS: CHECK(...) is correct!`；`spec:108` 第 5 节判据「稳态窗口静音填充次数 `== 0` 即绿」。
- **结论：`spec:250`（§12.3）的假定今天仍成立** —— 未见水位驱动改造，`shouldFillSilence()` 仍不含 ring 空条件、`got==0` 仍不计数、稳态 `silenceFills == 0` 判据在 CI 仍绿。但该条本身是「条件性成立」的登记：`spec:250` 与 `:223` 均写明「若将来改水位驱动或出现真实解码慢于消费的用例，须回来重开此判据」。

---

## 文档偏差清单（`doc/streaming-playback-spec.md` §12.4 五条 + §12.5 事实补记，逐条对照实测）

1. **§12.4 第 1 条（`:254`）**「三个独立指标只有数值与口径、没有阈值；后续 effort 要立判据，须先用本票留下的 CI 日志数值定标」→ **部分已过时，需更正**：三个指标确实仍无阈值（仍成立），但「用本票留下的 CI 日志数值定标」对 `open_file_ms` / `track_switch_ms` **不成立**——两指标在两个成功 run 的日志里 0 匹配（表 2），CI 里没有它们的数值；只有 `initScanUs`（µs，表 1）在 CI 有数值。drift 无仪表（`spec:244`、`:246`）。

2. **§12.4 第 2 条（`:255`）**「端到端 drift 仍未判…本票不新增仪表、不给数值判据」→ **仍成立**：`StreamMetrics.h:13-43` 七个字段无 drift 字段；`spec:244` 只登记口径、`spec:246` 明确「drift 在 `StreamMetrics` 里没有现成仪表，本票不新增」。

3. **§12.4 第 3 条（`:256`）**「跨素材峰值差判据（`|peak_long − peak_short| ≤ P`）因素材矩阵缺口（ogg / wma 各仅 1 首、无同格式长短配对）不可验，#67 已如实标注」→ **仍成立**：表 3 实测 ogg 1 首（3709 ms）、wma 1 首（259164 ms），无同格式长短配对；`test_streaming_budget.cpp:334` 实际用的是 `longPeak < shortPeak * 70`，`:337` 自注「same cross-format pair」，是跨格式配对而非同格式。

4. **§12.4 第 4 条（`:257`）**「本地 ctest 的 `sdk_unit_tests` 在修好 SIGSEGV 后全量约 191 s、命中 `TIMEOUT 180`，本地 ctest 7/8；票面记『CI 路径不经它、不受影响』，12.5 补记显示 coverage 脚本间接会跑它，CI 实况本机未验证」→ **已过时（且已闭合）**：`sdk/test/CMakeLists.txt:196-204` 的 `sdk_unit_tests` 现用 `-tse=` 排除 BadInput / StreamingBudget / Interaction，注释 `:203` 为「快集实测约 30 s，180 s 留 6 倍余量」；本次实测两个成功 run 的日志 `100% tests passed, 0 tests failed out of 14`（A `L23778` / 80.44 s，B `L23781` / 67.17 s），TIMEOUT 未命中；「CI 实况本机未验证」已被本次真实日志核对覆盖。

5. **§12.4 第 5 条（`:258`）**「全量 `UnitTests.exe` 在 integration 上已验证 492 用例 / 50078 断言 / 0 failed；Linux `sdk-coverage` 本机未验证；**本票 CI 观测是否真的在 GitHub Actions 显示数值，本机无法运行，未在真实 CI 验证**」→ **已闭合 / 被实测推翻**：run `42442116861`（attempt 2）与 `42442016087` 的真实 job 日志里四字段均有数值（24/7/12/11 行，表 1），观测 step 确实在 GitHub Actions 显示了数值；两轮 ctest 14/14 全绿。

6. **§12.5 事实补记（`:260-262`）**逐句：
   - 「`sdk-coverage.yml` 文本里确实没有 `ctest -R sdk_` 命令」→ **仍成立**（实测 workflow 全文无 ctest）。
   - 「`coverage` target 经 `scripts/coverage/run_linux.sh:29` **间接**执行 `ctest -R sdk_`（含 `sdk_unit_tests` 全量与 `TIMEOUT 180`）」→ **仍成立且被实测坐实**：`run_linux.sh:29`；日志 A `L23767-23780` 出现 14 个 `Start N: sdk_*` 与 `100% tests passed ... out of 14`。
   - 「完整执行链里 **8 组** CTest 在 CI 也会跑」→ **已过时**：现状是 **14 组**（`CMakeLists.txt` 14 个 `add_test`，CI 日志 `out of 14`）。
   - 「只是不带 `-s`、`--output-on-failure` 只在失败时输出，四个仪表数值仍不可见，12.1 的观测 step 仍必要」→ 就 **ctest 那段输出**而言仍成立；但就**整份 CI 日志**而言**已过时**——`sdk-coverage.yml:91-96` 的观测 step 已把四字段数值打进日志（表 1 实测可见）。

补充（§12.1，票面未要求但同属事实偏差）：`spec:233`「CI 既有链只跑 `coverage` target + `UnitTests -ts=MediaCoverage -s`」与 `spec:234`「不注册 `add_test` 分组、不动 `TIMEOUT 180`、不拆快集 / 全量集」→ **均已过时**：现状有 14 个 ctest 目标（含 `sdk_streaming_tests` `TIMEOUT 180`、4 段 badinput `TIMEOUT 120`、`sdk_interaction_tests` `TIMEOUT 180`），并用 `-tse=` 把三组从 `sdk_unit_tests` 快集排除（`:191`、`:198`）。

**票面 / 地图 Notes 需显式更正的三点**：
- 票 #95 称「`StreamingBudget` suite 在 `sdk/test/TestSdkSuite.cpp` 的 `MESSAGE` 落点」→ 实测 `sdk/test/TestSdkSuite.cpp` Grep `StreamingBudget|MediaCoverage|MESSAGE|peakHeldBytes|coverage matrix` 为 **`No matches found`（0 匹配）**；实际落点是 `sdk/test/unit/test_streaming_budget.cpp`（`:294`、`:599`、`:834`、`:974`、`:1402` 等）与 `sdk/test/unit/test_media_coverage.cpp`（`:36-50`、`:117`、`:163`），`doc/format-support.md:35` 亦写明 `test_media_coverage.cpp`。`TestSdkSuite.cpp` 是 legacy `TestSdkSuite` 可执行（`:1-27` 头文件、`:30-39` `DecodeCollector`、`:41-61` `LegacyPlayerProbe`）。
- 票 #95 / `spec:232` 暗示「四个仪表在 CI 数值不可见、须新增 step」→ 新增 step 已落地且**实测可见**（表 1），不可再按「不可见」现状引用。
- `sdk-coverage.yml:71` 注释「覆盖度矩阵的 **14 行**」→ 实测 **15 行 / 15/15**（表 3）。

---

## 结论性事实摘要（对应票 #95 八条核对点）

1. **CI 观测 step 原文**：`sdk-coverage.yml:91-96`，`continue-on-error: true`(`:92`) + `|| true`(`:96`)，命令 `./build/sdk/test/UnitTests -ts=StreamingBudget -s | grep -E "peakHeldBytes|silenceFills|startToFirstPcmUs|initScanUs|packets|boundaryMicros"`(`:94-96`)；`sdk-windows.yml` **无**同名或等价 step（Grep 0 匹配），只有 `Report coverage matrix numbers`(`:147-157`，跑 `MediaCoverage`)。
2. **四字段在 CI 的实际可见性**：run `42442116861`(job `128617560798`, attempt 2) 与 `42442016087`(job `128613044475`) 日志里**全部出现且带数值**，出现行数两轮一致（`peakHeldBytes` 24、`silenceFills` 7、`startToFirstPcmUs` 12、`initScanUs` 11、`boundaryMicros` 7、`packetsPulled` 39、`packets <数字>` 18）；例：A `L24514` `initScanUs 20, startToFirstPcmUs 837, peakHeldBytes 4608, silenceFills 0`、B `L24517` `initScanUs 23, startToFirstPcmUs 605, peakHeldBytes 4608, silenceFills 0`。结构值稳定、µs 时间值跨运行波动（wma `boundaryMicros` 6541 vs 417）。
3. **生产链两指标**：`track_switch_ms`(`sdk/MusicPlayList.cpp:431`) / `open_file_ms`(`:288-290`) 经 `sdk/Metrics.cpp:46-49` 输出为 `metric=<name> type=timing value=<ms> unit=ms`，量纲毫秒且与 µs 仪表不同；**两份 CI 日志 `metric=`/`track_switch_ms`/`open_file_ms` 均 0 匹配，CI 不可见**。
4. **StreamingBudget / ctest / TIMEOUT**：MESSAGE 实际在 `test_streaming_budget.cpp`（非 `TestSdkSuite.cpp`）；`StreamMetrics.h` 共 7 字段（`:19`/`:25`/`:28`/`:32`/`:36`/`:38`/`:42`）；`sdk/test/CMakeLists.txt` 14 个 `sdk_*` 目标中**有** `sdk_streaming_tests`(`:223`) 专门跑 StreamingBudget（`TIMEOUT 180`，`:231`），并被 `sdk_unit_tests` `-tse` 排除；TIMEOUT 共挂在 7 个组（`sdk_unit_tests` 180、4 个 badinput 120、`sdk_streaming_tests` 180、`sdk_interaction_tests` 180）；CI 实测 14/14 全绿、`sdk_streaming_tests Passed 17.60 sec`、TIMEOUT 未命中。
5. **素材矩阵**：15 行定义 `test_media_coverage.cpp:36-50`、数值日志 A `L24433-24448`；同格式长短配对只有 `.aac`(10 s / 221 s) 与 `.m4a`(69.9 s / 12 s，AAC vs ALAC)；`.wav`(两短) / `.mp3`(两长) 同量级；**ogg 1 首、wma 1 首，无同格式长短配对 → 坐实「素材矩阵缺口」**。
6. **稳态欠流**：`silenceFills` 计数点 `AudioStreamDecoder.cpp:293-304`（仅 SEEKING / stop / abort 窗口），`shouldFillSilence()`(`sdk/MusicPlayer.cpp:482-505`) **不读 ring 空条件**，`got==0`(`AudioStreamDecoder.cpp:906-909`) 静默跳过不计数 → `spec:250` §12.3 假定**今天仍成立**（CI `silenceFills == 0` 判据仍绿）。
7. **播放器可观测面**：`PlayerMocks.hpp:27/:29/:31` 分别记状态 / 事件 / 位置值序列，仅 `test_player_integration.cpp` 消费，断言数字含 `countEvents(error)==1`、`countEvents(state)==0`、`durationInWindow==1`、`positionValues().size()>=3`、`!duplicate`、`states.front()==Playing`/`back()==Stopped`；`reset()` 清空，**无跨运行 / 跨版本留存**。
8. **文档偏差**：§12.4 五条中第 1 条部分过时（两生产链指标无 CI 数值可定标）、第 2/3 条仍成立、第 4 条已过时（`sdk_unit_tests` 不再命中 `TIMEOUT 180`，CI 已实测）、第 5 条已闭合（CI 观测数值实测可见）；§12.5 「8 组 CTest」已过时（现 14 组）、「四个仪表数值仍不可见」对整份日志已过时；另 `spec:233-234` 关于「只跑 MediaCoverage、不注册分组、不拆快集」均已过时。

> 本票只给事实，不做决策：未改动任何已有文件、未改代码、未关闭票、未动其他 issue；本分支只新增本文件。
