# 首播门槛的实测取证（issue #174）

> 基线：`master @ be5d85365`（工作区取证开始时干净，取证结束后仍然干净）。
> 机器：Windows 10 Pro / MinGW 7.3.0 / Debug 构建；CI 对照为 Linux `sdk-coverage` run `43730199160`（2026-11-15，同 commit）。
> 手段：单目标运行 `UnitTests.exe -ts=StreamingBudget -s`（未跑 `ctest -R sdk_`、未新增 ctest 目标、未改判据与 TIMEOUT），
> 临时插桩只写 stdout，取数后已 `git checkout HEAD -- sdk/audio/decode/AudioStreamDecoder.cpp` 还原并重编，
> `git status --short` 为空。原始日志与插桩副本留在 `.workbuddy/tmp/`（该目录被 `.gitignore:15` 忽略）。
> 本文每条结论都带 `文件:行号` 或 run id；估算值单独标注。

---

## 1. 结论摘要

1. **门槛 150 ms 可以落地**。11 种格式在**本机**攒够 150 ms PCM 的墙钟时间中位数 0.44–4.44 ms、最慢 **9.05 ms**（m4a-AAC）；
   CI（Linux，同 commit）两个可比口径是 ogg **998 µs** / wma **1470 µs**。150 ms 的门槛相对实测最慢值有 16× 余量。
2. **等待上限 300 ms 合适**：它是门槛的 2 倍（迟滞），是本机实测最慢 9.05 ms 的 33 倍、CI 最慢 1.54 ms 的 194 倍，
   在本机与 CI 都不会被触发；触发时应当**照常 `start` 设备**（不阻塞），SDL 会用静音补齐，行为与今天一致。
3. **mkv / m4a 的现场重算包边界路径没有「300 ms 内攒不够」的素材**：mkv 首个 demux 步就交出 90 144 B（≈512 ms，一次跨过 150/300 ms 两个门槛），
   m4a-AAC（maxPacket 970 B）需约 28 包、实测 4.1–9.0 ms。两条路径的 demux 懒打开（`avformat_open_input`）发生在解码线程内，
   已经**计入** `startToFirstPcmUs`，仍然只有毫秒级。
4. **口径**：`startToFirstPcmUs` **不含** init 只读扫描（后者在提取器构造期完成，早于 `start()`），
   但**含**线程创建、ring `reset()`、以及 mkv/m4a 在解码线程里重开 demux 的时间。
   因此「用户点击到出声」的真实首帧 ≈ `initScanUs + startToFirstPcmUs`（本机中位：ogg 31+998 µs 级、wma 6.6+1.5 ms 级）。
5. **网络源（http / hls）首帧由网络支配，300 ms 上限会例行失效**：`rw_timeout=3 s`、重连按 1 s 分片等待，
   任何一次读阻塞都可以是 300 ms 的 10 倍以上。正确处置不是把上限调大，而是**上限到点即放行**。

---

## 2. 实测数据表

### 2.1 测量方法（两个量，分开量）

| 量 | 定义 | 取数点 | 备注 |
|---|---|---|---|
| `startToFirstPcmUs` | `start()` 入口 → 首次 ring 写入成功 | `AudioStreamDecoder.cpp:116`（起表）、`:971-980`（落表）、`:270-282`（读出） | 既有仪表，未改动语义 |
| 「攒够 N ms」 | `start()` 入口 → **累计写进 ring 的 PCM** 首次 ≥ N ms | 临时插桩，挂在 `writeDecodedBytes` 的写循环 `:961-967` 之后 | 预填充窗口内消费端尚未启动（`MusicPlayer.cpp:336` 才起 10 ms 定时器），故「累计写入 = 此刻 ring 占有量」，与「ring 攒够 N ms」同口径 |

- N ms → 字节按 **ring 里那一档** 的字节率换算（`AudioStreamDecoder.cpp:420-433` `positionBytesPerMs()`；
  重采样生效时是设备格式，否则源格式），插桩直接调用该函数，避免自造换算。
- 素材与用例全部来自 `sdk/test/unit/test_streaming_budget.cpp`（24 例 / 6 次运行 / 每次 35 次播放任务，
  共 **210 个任务样本**，全部计入下表）。
- 单次运行墙钟 58 s（本机），退出码 0，`24 passed / 0 failed`（`prefill_run1.log` 尾部）。

### 2.2 主表：按格式分组（本机，Windows / Debug，6 次运行合并）

单位 µs。`n` = 该格式在 6 次运行里被播放的任务数（同一素材多次播放各算一次）。

| 格式 | n | `startToFirstPcmUs` min / med / max | 攒够 150 ms min / med / max | 攒够 300 ms max | `initScanUs` med | 数据来源 |
|---|---|---|---|---|---|---|
| WAV/PCM（wav+aiff，非重采样） | 30 | 238 / 450 / 860 | 233 / 436 / 842 | 856 | 297 | 插桩实测 |
| MP3 | 6 | 1595 / 1718 / 2575 | 1777 / 1901 / 2859 | 3121 | 195 | 插桩实测 |
| AAC（裸 ADTS） | 12 | 1844 / 2436 / 4936 | 2098 / 3050 / 5461 | 6096 | 12766 | 插桩实测 |
| FLAC | 24 | 521 / 971 / 2089 | 633 / 1121 / 2512 | 3582 | 211 | 插桩实测 |
| Vorbis（ogg） | 48 | 888 / 2279 / 3811 | 1118 / 2800 / 4120 | 4460 | 112 | 插桩实测 + 既有仪表 |
| m4a（AAC） | 6 | 3885 / 4241 / 8669 | 4104 / 4440 / **9048** | 9414 | 4595 | 插桩实测 |
| m4a（ALAC） | 6 | 1578 / 1908 / 4037 | 1861 / 2219 / 4791 | 6298 | 1039 | 插桩实测 |
| mkv（LPCM） | 24 | 1847 / 3525 / 7892 | 1843 / 3517 / 7873 | 7887 | 1037 | 插桩实测 |
| *附：WMA* | 24 | 1683 / 2065 / 5273 | 1875 / 2270 / 5811 | 6330 | 10917 | 插桩实测 + 既有仪表 |
| *附：ASF（WMA）* | 6 | 2534 / 4222 / 6920 | 2529 / 4208 / 6908 | 6917 | 2353 | 插桩实测 |
| *附：APE* | 6 | 1547 / 4196 / 4670 | 1544 / 4188 / 4661 | 5929 | 611 | 插桩实测 |

全表（210 个任务）`攒够 150 ms` 的全局最大值 = **9 048 µs**（m4a-AAC 的一次运行），全局中位数 = 2 100 µs。

### 2.3 离群样本（口径不同，不计入主表）

| 场景 | `startToFirstPcmUs`（6 次运行的范围） | 攒够 150 ms（范围） | 为什么不计入 |
|---|---|---|---|
| wav **整块供给**（`WholeViewExtractor` 反例，`test_streaming_budget.cpp:524-562`） | 42 543–61 374 | 232–424 | 这是「整首一包」的**反例路径**，生产链已无此供给形式（`AudioStreamDecoder.cpp:792-795`） |
| wav **16 kHz 单声道 → 44.1 kHz 重采样**（`test_streaming_budget.cpp:1108-1140`） | 10 220–28 107 | 4 729–8 041 | 首个写入块是重采样后的整段输出（一次写满 262 144 B），`firstPcm` 记在这整次写完之后；口径偏保守 |

这两个离群点说明一件事：`startToFirstPcmUs` 记的是「**首次 write 调用整体返回**」，
当单次 write 的块大于门槛时，它会把「ring 已经攒够」的时点报晚（本表里 424 µs 就已经攒够 150 ms，而 `firstPcm` 报 45 ms）。

### 2.4 CI 侧（Linux，第三方数据，用于交叉核对）

CI 的观测 step 只把 `MESSAGE(...)` 打进 job 日志（`.github/workflows/sdk-coverage.yml:97-102`），
**只有 ogg / wma / flac 三个用例的 `MESSAGE` 里带 `startToFirstPcmUs`**（`test_streaming_budget.cpp:294`、`:599`、`:974`），
mp3 / aac / mkv / m4a 的 `MESSAGE` 里没有这个字段（`:649`、`:699`、`:834`、`:1402`）——这是 CI 数据无法覆盖全部格式的**结构性原因**。

| 来源 | ogg（3.7 s） | wma（259 s） | flac（297 s） |
|---|---|---|---|
| CI run `43730199160`（2026-11-15，`be5d85365`） | 998 | 1470 | 845 |
| 同上，复现性用例两次 | 689 / 574 | 1224 / 1178 | — |
| 13 次历史 run 的 min / med / max（`docs/research/observability-calibration.md:57`、`:67`） | 594 / 822 / 910 | 982 / 1315 / 1543 | 未登记 |

CI `initScanUs`：ogg 31、wma 6590（run `43730199160`）；13 次历史 run 分别是 19/30/36 与 3726/5943/6307（同上 `:56`、`:66`）。
CI 只用 dummy 驱动（`sdk-coverage.yml:69` `SDL_AUDIODRIVER: dummy`），不打开真实设备。

**结论：CI 没有「按格式」的 `startToFirstPcmUs` 数据可用，且短期内也拿不到**——不是日志缺失，而是测试用例根本不打印该字段。
本报告的主表因此只能来自临时插桩（优先级第 2 档），CI 数据只用于标定「同机形差距」（本机中位数 ≈ CI 的 1.2–3 倍）。

---

## 3. 逐条回答

### 3.1 问题 1：`startToFirstPcmUs` 的实测分布

见 §2.2。可直接引用的数：

- **最慢的格式是 m4a-AAC（med 4 241 µs / max 8 669 µs）与 mkv（med 3 525 µs / max 7 892 µs）**，两者都是「现场重算包边界」路径。
- **最快的是 WAV/PCM（med 450 µs）**，因为 `AudioStreamDecoder::threadFunc` 的 PCM 分支不建解码器，
  直接按偏移切片写 ring（`AudioStreamDecoder.cpp:677-681`、`:719-755`）。
- 同一格式内部的离散度约 2–4×（例：FLAC 559–1824 µs），与 CI 上「µs 时间值 13 次取值互不相同」的结论同源
  （`docs/research/observability-calibration.md:90`）。
- 「同一份素材连播两次」的差值也在同一量级（`test_streaming_budget.cpp:935-981` 只要求两次都 > 0，不比对相等——这是有意的，计时型仪表不可逐位复现）。

**数据来源判定**：
1. 现有测试输出——只能覆盖 ogg / wma / flac 三个用例（见 §2.4），**不足以按格式分组**；
2. 临时插桩实测——本报告主表（6 次运行 × 35 任务）；
3. CI 历史日志——**确认存在但不可用于按格式分组**：日志不随仓库下发（`sdk-coverage.yml:71-75` 只上传 `build/coverage/html`，
   没有日志 artifact），本次用 `gh run view 43730199160 --log` 取到最新一轮（`gh` 已登录 `wt159`），
   其中 `MESSAGE` 41 行、带 `startToFirstPcmUs` 的只有 5 行共 7 个取值，另有两个 13-run 的历史分布归档在
   `docs/research/observability-calibration.md:52-88`。

### 3.2 问题 2：`start()` → ring 首次攒够 150 ms 的墙钟

见 §2.2 第 4 列。要点：

- **全格式 ≤ 9.05 ms**（本机），中位数 0.44–4.44 ms；CI 同量纲的 ogg/wma 是 998 / 1470 µs 的 `firstPcm`，
  攒够 150 ms 只会再多 1 个包的时间（3.7 s ogg 一页 ≈ 4 096 B ≈ 23 ms 音频，一页就够）。
- **300 ms 内攒不够门槛的素材：没有**。最接近上限的是 m4a-AAC（9.05 ms，33× 余量）与 mkv（7.87 ms，38× 余量）。

**mkv 细节（为什么它反而最快跨过门槛）**：mkv 走 `runFfmpegPacketLoop`（`AudioStreamDecoder.cpp:782-790`、`:1005-1018`），
包边界由 `MKVPacketReader::nextPacket` 在播放期现算（`sdk/extractor/mkv/MKVPacketReader.cpp:224-262`，
计时点 `:233`、`:245-247`）。`music-mkv.mkv` 的音频是 LPCM，**第一个 demux 步就交出 90 144 B**（= 512 ms @44.1k/2ch/S16），
所以 `mark=150ms` 与 `mark=300ms` 的时间戳差只有 **3–14 µs**：门槛一次被跨过，预填充量远超设定值（仍是 ring 容量的 34%）。

**m4a 细节**：`摇滚乐_Freesound.m4a` 的 maxPacket 970 B（`test_streaming_budget.cpp:1410` 的 MESSAGE），
攒够 26 400 B 需约 28 包；现场重算成本 CI 实测 `boundaryMicros 1011`（3010 包累计，run `43730199160`）≈ 0.34 µs/包，
本机 `9 774 µs`/3010 包 ≈ 3.2 µs/包——**边界重算本身不是瓶颈**，首帧时间主要花在解码线程内的 demux 打开
（`sdk/extractor/m4a/M4APacketReader.cpp:232` 懒打开 → `:157` `avformat_open_input`）。

**为什么这 35 个任务可以当作「ring 攒够」的样本**：预填充窗口里消费端还没起来——`MusicPlayer::Impl::_play` 的顺序是
任务 → `open()` → `start()` → `startConsumerThread()`（`sdk/MusicPlayer.cpp:318`、`:326`、`:331`、`:336`），
10 ms 定时器在 `:652` 才创建。所以「累计写入字节」就是「ring 里的字节」。
（测试套件自己的消费端是 5 ms 轮询 + 一次 drain 空，`test_streaming_budget.cpp:52-53`、`:71-86`、`:204`，
**不代表生产消费速率**，这也是我没有直接量 ring 占有量、而量累计写入量的原因。）

### 3.3 问题 3：`startToFirstPcmUs` 与 `initScanUs` 的口径差异；网络源首帧

**（a）两者的时间轴是串行、互不包含的。**

| 阶段 | 计时仪表 | 证据 |
|---|---|---|
| 提取器构造（= init 只读扫描：demux 装配、标称时长、包工厂） | `initScanUs` | `sdk/extractor/ExtractorFactory.cpp:185-195`（`begin` / `elapsed` / `setInitScanMicros`） |
| `AudioStreamDecoder::start()` 入口 | `m_startClock` 起表 | `AudioStreamDecoder.cpp:111-116` |
| `start()` 里抄 init 值 | `m_initScanUs.store(extractor->initScanMicros())` | `:144` |
| 解码线程：`applySegmentBase(0)` → `ring->reset()` → 各格式分支 | `startToFirstPcmUs` 计入 | `:504-515`、`:517-543`、`:677-681`、`:782-790` |
| 首次 ring 写入成功 | `startToFirstPcmUs` 落表 | `:969-980` |

- 提取器在 `start()` **之前**就建好了（生产链 `sdk/MusicPlayList.cpp:462-465` 用 `processProperties.extractor`，
  测试链 `test_streaming_budget.cpp:233-235` 先 `createExtractor` 再 `decoder.start`），
  且 `start()` 只是把已量好的 `initScanMicros` 抄进本任务（`AudioStreamDecoder.cpp:144`）。
  → **`startToFirstPcmUs` 不含 init 扫描**，与 `StreamMetrics.h:26-27` 的字面说明一致；两者相加才是「点击到出声」。
- 反向的包含关系也要写清：`startToFirstPcmUs` **含**线程创建（`:207-209`）、ring `reset()`（`:515`）、
  FLAC/Vorbis 解码器在 thread 里初始化（`:517-543`、`:607-621`）、以及 mkv/m4a 在 thread 里重开 demux
  （`MKVPacketReader.cpp:227-229` → `:153`；`M4APacketReader.cpp:232` → `:157`）。
- 它**不**含设备 `open` / `start`：测试链根本没有 `AudioDevice`；生产链的设备 open/start 与解码线程**并发**执行
  （`MusicPlayer.cpp:326`、`:331` 在控制线程），时间上是重叠而不是串行，无法从这一个数里分离。CI 用 dummy 驱动，
  连真实设备的 open 成本都不含。

**（b）网络源（http / hls）首帧是否天然让 300 ms 上限失效——会，且是结构性的。**

| 事实 | 证据 |
|---|---|
| 直通路径的播放期读由 FFmpeg 原生 URL 层完成，唯一墙钟是 `rw_timeout=3 s`（一次读可以阻塞 3 s） | `sdk/extractor/hls/HlsExtractor.cpp:107-113`、`:115-128` |
| 头部嗅探（`.m3u8` 判定）也带 3 s 超时，且在**提取器构造期**（计入 `initScanUs`，不在 `startToFirstPcmUs`） | `sdk/extractor/UrlSource.cpp:195-233`（`:204` timeout 3 000 000 µs） |
| 断流后的重连按 **1 s 分片 ×10** 等待，单次窗 ≤4 s、总预算 ≤12 s | `AudioStreamDecoder.cpp:1156-1165`（`:1158-1164` 分片循环） |
| LIVE 判定 = `durationMs == 0` + 提取器支持无状态重开；LIVE 上 seek 被静默忽略，不会进 SEEKING 静音窗口 | `AudioStreamDecoder.cpp:128-132`、`:1028-1030`、`:691-693` |

结论：对 http / hls，`start()` 之后的第一次 `av_read_frame` 要等 DNS / TCP / 首分片，**任何一次 300 ms 级的等待上限都会先到点**。
但这是「上限到点、照常 start」，不是错误：设备开始跑空队列后 SDL 用静音补齐（见 §3.4），
与今天「起设备时队列本来就是空的」行为完全一致——上限只是**不让网络延迟把首播卡住**，它不可能也不应该让网络变快。
（本项是**代码结论，无实测样本**：本仓库没有任何网络首帧的计时数据，`sdk/test/LiveSmoke.cpp` 不在本次取证范围内。）

### 3.4 问题 4：设备侧事实（以 `sdk/3rdparty` 内的 SDL2 为准）

**版本**：`sdk/3rdparty/CMakeLists.txt:124` 固定 `SDL-release-2.28.1`；
版本号字面量在 `sdk/3rdparty/SDL-release-2.28.1/include/SDL_version.h:60-62`（2.28.1）。
Windows 与 Linux 用的是**同一份源码**：两侧都从 `sdk/3rdparty/CMakeLists.txt:124` 这棵源码树构建，
只落在不同 dist 目录（`dist/windows_x86_64_gcc_debug` / `dist/linux_x86_64_gcc_debug`，
Linux 侧的构建开关见 `.github/workflows/sdk-coverage.yml:58`）。

| 事实 | 证据 |
|---|---|
| **设备 `start` 后队列空 → 输出静音**，不是噪声、不是停播 | `src/audio/SDL_audio.c:549-567`：`SDL_BufferQueueDrainCallback` 先 `SDL_ReadFromDataQueue`，余下的 `SDL_memset(stream, device->callbackspec.silence, len)`（`:563-566`） |
| 头文件同一承诺：「If the device needs audio but there is not enough queued, it will play silence to make up the difference」 | `include/SDL_audio.h:1197-1200` |
| 队列**无上限**（256 KB 是应用侧自设的门限，不是 SDL 的） | `include/SDL_audio.h:1195-1196`；应用侧 `sdk/MusicPlayer.cpp:540-543` `kQueueThreshold = 256*1024` |
| `SDL_GetQueuedAudioSize` **不含已经送进硬件的块**（少算最多一个设备缓冲） | 头文件：`include/SDL_audio.h:1280-1287`（"queued ... but have not yet been sent to the hardware"，并明说无法给出已播字节边界）；实现：`src/audio/SDL_audio.c:624-642` 直接返回 `SDL_CountDataQueue(device->buffer_queue)`；计数在 drain 回调 `SDL_ReadFromDataQueue` 里当场扣减（`src/SDL_dataqueue.c:262-304`，计数 `:307-314`） |
| 一个设备缓冲 = `samples=882` @44.1 kHz = **20 ms** | `sdk/audio/device/AudioDevice.cpp:22-25`（`m_defaultSamples = 882`）；`samples * 1000 / freq` 的换算见 `src/audio/SDL_audio.c:733`、`:745` |
| `open` 之后设备**默认是 paused**，`start()` = `SDL_PauseAudioDevice(id, 0)` = 取消暂停 | `src/audio/SDL_audio.c:1386`（`paused = 1`）；`AudioDevice.cpp:145-153`（`:151`） |
| **paused 期间 SDL 不消费队列**：paused 分支直接把 buffer 填静音、不调 drain 回调 | `src/audio/SDL_audio.c:712-719`（`:714-718`） |
| 音频线程在 `open` 时就起来，**paused 也按实时节奏往硬件塞静音** | `src/audio/SDL_audio.c:1509`（建线程）、`:691-751`（主循环，`:747-750` Play+Wait） |
| `SDL_QueueAudio` 只写数据队列，不检查是否 start、不阻塞、拷贝入队 | `src/audio/SDL_audio.c:584-604` |
| Windows 后端把队列数据写进 WASAPI render buffer（`spec.samples` 帧一次） | `src/audio/wasapi/SDL_wasapi.c:181`、`:194`；`GetBufferSize` / `spec.samples` 见 `:476`、`:486` |
| Linux(ALSA) 后端 `snd_pcm_writei` + `snd_pcm_wait` 写同样长度的 period 缓冲 | `src/audio/alsa/SDL_alsa_audio.c:369`、`:424` |
| CI 用 dummy 驱动：`GetDeviceBuf` 返回 NULL → 走 work_buffer，**仍按 `samples/freq` 实时节拍消费队列并补静音** | `src/audio/dummy/SDL_dummyaudio.c:30-35`、`:47-58`；`src/audio/SDL_audio.c:262-265`（默认 GetDeviceBuf 返回 NULL）、`:743-746`（work_buffer 分支 `SDL_Delay`） |

**Windows 与 Linux 是否一致**：语义**一致**——队列、计数、静音补齐、paused 行为全在 core（`SDL_audio.c`），
两个平台只在「一个设备缓冲有多深、硬件还剩多少没播」上有差异：`GetQueuedAudioSize` 在两端都**少算 1 个在飞的设备缓冲**
（本仓库配置 = 20 ms），差别只在 WASAPI 的 period 与 ALSA 的 period 不同，本仓库把它固定成 `samples=882`（两端同一份 `AudioDevice.cpp`）。

**对本票的直接含义（两个必须写进设计的推论）**：

1. **`GetQueuedAudioSize` 当高水位会低估 20 ms**：应用侧 256 KB 门限实际对应的在飞音频是 256 KB + 20 ms（≈1.51 s），不是 1.49 s。
2. **只做 ring 预填充，仍会有 ≤ 20 ms 的起播空隙**：设备 `start` 之后第一拍 drain 回调必然看到空队列 → 整块静音；
   消费端第一拍在 10 ms 之后才写 4 096 B（`MusicPlayer.cpp:545-550`）。若要做到「第一帧就是音频」，
   必须在 `start()` **之前**把首批 PCM 直接 `SDL_QueueAudio` 进设备队列（paused 期间队列不被消费，`SDL_audio.c:714-718`，
   且队列无上限，`SDL_audio.h:1195-1196`），ring 预填充本身做不到这一点。

### 3.5 问题 5：数据不足时的可复现测量方案

见 §4。

---

## 4. 测量方案（不改生产代码、不新增 ctest 目标）

### 4.1 本报告已用的方案（可原样复现）

```bash
# 1) 临时插桩：只在 AudioStreamDecoder.cpp 里加 stdout（副本见 .workbuddy/tmp/AudioStreamDecoder.cpp.instrumented）
#    a. 文件头加 prefillReset() / prefillNoteWritten()：累计写入字节跨 50/100/150/200/250/300 ms 时打印墙钟
#    b. start() 里 m_startClock 之后调用 prefillReset()
#    c. writeDecodedBytes 的写循环 written += got 之后调用 prefillNoteWritten(got, positionBytesPerMs(), codec, m_startClock)
#    d. 首帧分支打印 [FIRSTPCM] codec / us / bytesPerMs / initScanUs
export PATH="/d/workspace/SoundBridge/sdk/3rdparty/dist/windows_x86_64_gcc_debug/bin:/d/Qt/5.14.2/mingw73_64/bin:/d/Qt/Tools/mingw730_64/bin:$PATH"
cmake --build build --target UnitTests -j4
cd sdk/test && for i in 1 2 3; do
  /d/workspace/SoundBridge/build/sdk/test/UnitTests.exe -ts=StreamingBudget -s > run$i.log 2>&1
done
# 2) 取数：按 mark=50ms 分组、按 FIRSTPCM 序号配对（脚本见 .workbuddy/tmp/pair3.awk / stats4.py）
# 3) 还原：git checkout HEAD -- sdk/audio/decode/AudioStreamDecoder.cpp && cmake --build build --target UnitTests -j4
#    git status --short 必须为空
```

要点：**只跑一个套件**（`-ts=StreamingBudget`，24 例，本机 58 s），不跑 `ctest -R sdk_`；
新增临时文件全部落在 `.workbuddy/tmp/`（`.gitignore:15`）。

### 4.2 缺口的测法

| 缺口 | 建议测法（仍不碰生产代码） |
|---|---|
| **网络源首帧**（http / hls） | 用 `sdk/test/LiveSmoke.cpp` 的本地 mock 网络（`test/mocks/HttpMocks.hpp`）起 http 服务，在 `UnitTests` 之外手工跑一个临时 main（放 `.workbuddy/tmp/`，用同一套 include 与链接参数），量 `extractor 构造` 与 `start()→首 PCM` 两段。**不要**把它注册成 ctest，也不要在 CI 里跑（会引入公网/端口依赖）。 |
| **真实设备上的起播空隙** | 本机手工跑 `sdk_interaction_tests`（`ctest -R sdk_interaction_tests`，14 例 / 11.8 s，`sdk/test/CMakeLists.txt:251-261`），并临时在 `MusicPlayer::Impl::consumeAudioData` 里打印 `(now, getQueuedBytes(), ring->availableRead())` 三元组；用 `SDL_AUDIODRIVER=dummy` 与真实驱动各跑一次做对照。 |
| **生产消费速率下的 ring 水位曲线** | 同上，只打印不判断；或把测试消费端临时改成「10 ms / 4 096 B」（`test_streaming_budget.cpp:52-53`）跑一轮反例——**只作诊断，不许改成判据**。 |
| **冷缓存 / 慢盘** | 重启后首跑（本次首跑 `initScanUs` wma 48 957 µs，热跑 7 840–29 673 µs，差 1.6–6×），或对素材做一次 `fsutil file setvaliddata` 级别的清缓存；不必改代码。 |
| **CI 侧按格式** | 需要改测试（在 mp3/aac/mkv/m4a 的 MESSAGE 里加 `startToFirstPcmUs`）——**本票不改**；若将来要，属「加一个只读字段到 MESSAGE」的独立小票，且必须先过 `doc/observability-contract.md` §3.2 第 5 步（禁写断言/禁注册分组）。 |

---

## 5. 边界与不确定性

**哪些是实测、哪些是估算**

- 实测：§2.2 / §2.3 全部数字（本机 6 次运行 × 35 任务）；§2.4 的 CI 数字（run `43730199160` 的 job 日志原文）。
- 估算（文中已标注）：字节↔毫秒换算用 `positionBytesPerMs()` 的整数除（44.1 kHz/2ch/S16 → 176 B/ms，误差 <0.3%）；
  「mkv 首块 = 511 ms」由 90 144 B ÷ 176 B/ms ≈ 512 ms 推得；「m4a 需 28 包」由 26 400 B ÷ 970 B/包 推得。
- 代码结论（无实测）：§3.3(b) 网络首帧、§3.4 的设备队列语义与 Windows/Linux 一致性。

**受机器影响的部分**

1. **绝对毫秒数不可跨机搬运**：本机（Windows Debug，热缓存）`firstPcm` 中位数是 CI（Linux，优化构建）的 1.15–2.3 倍
   （ogg 2 279 vs 998 µs、wma 2 065 vs 1 470 µs、flac 971 vs 845 µs；13 次历史 run 的 ogg 中位 822 µs）。
   本报告的**结论方向**（≤10 ms、300 ms 上限有 33× 余量）在两端都成立，但具体阈值若要写进判据，应取两端较慢者。
2. **`initScanUs` 受页缓存影响极大**：wma 首跑 48 957 µs → 热跑 7 840–29 673 µs（1.6–6×）。
   `startToFirstPcmUs` 同样受影响（wma 首跑 6 304 µs → 热跑 1 683–5 273 µs）。
3. **`startToFirstPcmUs` 是「首次 write 整体返回」**：单次块大于门槛时会报晚（§2.3 两行离群），
   跨格式比较时这一点会被误读成「格式慢」，必须和「攒够 N ms」分列。
4. **测试链没有 AudioDevice**：所有数字都不含真实驱动的 `open` / `start`（CI 用 dummy，本机也没有起设备），
   生产链的起播延迟还要加上设备打开时间（未测）。
5. **`silenceFills` 在预填充窗口内恒为 0** 不能当作「无欠流」的证据：它只覆盖 shuttingDown / switching / SEEKING 三类窗口
   （`AudioStreamDecoder.cpp:312-323`、`StreamMetrics.h:20-24`），预填充窗口不在其中。
6. **样本量**：本机 6 次运行、每个格式 6–48 个样本；m4a-AAC / m4a-ALAC / APE / ASF 各只有 6 个样本，
   其 max 值（8 669 / 4 037 / 4 670 / 6 920 µs）不宜当作上界引用。
7. **CI 日志按格式不可得**，且将来若有人把 `startToFirstPcmUs` 加进更多 MESSAGE，会改变 CI 日志的 grep 命中行数
   （`sdk-coverage.yml:102` 的 `grep -E` 会自然覆盖，但历史 13 run 的基线不再可比）。

---

## 附：推荐值（供主 agent 拍板）

| 参数 | 建议 | 依据 |
|---|---|---|
| 首播门槛 | **150 ms**（26 400 B @44.1k/2ch/S16 = 176 B/ms × 150 ≈ ring 容量的 10.1%，≈ 6.4 个 4 096 B 消费块） | 覆盖 7.5 个 20 ms 设备缓冲；实测到达时间中位 0.44–4.44 ms、最慢 9.05 ms（门槛相对最慢值 16× 余量）；把门槛抬到 300 ms 只再多花 ≤1.27 ms 墙钟，但会把「最坏等待」推高 |
| 等待上限 | **300 ms**（2× 门槛），到点**照常 `start`**，不阻塞、不报错 | 33× 本机最慢、194× CI 最慢；网络源必然会打满上限，此时静音补齐 = 今天的行为 |
| 门槛的计量点 | `start()` 之后、ring 累计写入 ≥ 门槛即放行；**不要**把 `initScanUs` 算进这个等待 | `startToFirstPcmUs` 的既有口径不含 init（§3.3a）；把 init 算进来会让 259 s 素材的 wma 白等 7–49 ms |
| 附加建议（不在本票范围） | 若要求「第一帧就是音频」，预填充必须落在**设备队列**而不是 ring：paused 期间队列不被消费，`SDL_QueueAudio` 无上限，应用侧 256 KB 门限仍然有效 | `SDL_audio.c:714-718`、`SDL_audio.h:1195-1196`、`MusicPlayer.cpp:540-543` |
