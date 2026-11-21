# dummy 驱动下的队列语义与容量上限：一手源码核对 + 本机实测（票 #201）

> 本文件由 `research` 流程产出：每条结论附一手来源（SDL 2.28.1 源码 `文件:行号`）或实测命令与原始读数。
> 只读研究，不改生产代码、不改 CI 配置、不新增 ctest 目标。
> 基线：`master`（提交见本文件所在提交的父提交），SDL 源码取自仓库内 `sdk/3rdparty/SDL-release-2.28.1/`，运行库用仓库自带预构建 `sdk/3rdparty/dist/windows_x86_64_gcc_debug/bin/SDL2d.dll`。
> 实测宿主：Windows 10 Pro，MinGW g++ 7.3.0，`SDL_AUDIODRIVER=dummy`，注入参数与 `sdk/audio/device/AudioDevice.cpp:22-25` 完全一致（44100 Hz / `AUDIO_S16SYS` / 2 声道 / 882 samples）。

## 0. 方法与探针位置

探针源码与可执行文件均在仓库外的临时目录（`/tmp`，本机落在 `D:\tmp`），未写入仓库、未触碰 `build/`：

| 探针 | 用途 | 关键输出 |
|---|---|---|
| `sdlq_probe.cpp` | 节拍采样、队列线性度、失败面全扫 | §1 / §2 / §3 |
| `sdlq_probe2.cpp` | 拍长抖动直方图、重复 start、多设备诊断 | §1 / §4 |
| `rate2.cpp` | 20 s 平均速率（少量轮询，避免自扰动） | §1 |
| `cb.cpp` / `sweep.cpp` | 回调路径 vs 队列路径的拍长对照、samples 扫描 | §1 |
| `reuse.cpp` | device id 复用、`SDL_Quit` 后写入 | §3 |

编译命令（一次，全部探针同构）：

```bash
g++ -O2 -std=c++11 -DSDL_MAIN_HANDLED <probe>.cpp \
  -I sdk/3rdparty/dist/windows_x86_64_gcc_debug/include/SDL2 \
  -L sdk/3rdparty/dist/windows_x86_64_gcc_debug/lib -lSDL2d -o <probe>.exe
# 运行前把 sdk/3rdparty/dist/windows_x86_64_gcc_debug/bin 加到 PATH
```

---

## 1. 消耗节拍：dummy 是否按 882/44100 ≈ 20 ms 推进？字节速率是多少？

**结论：名义拍长 20 ms、每拍恰好 3528 B；本机实测拍长 23.52–23.60 ms，速率 149.5–149.8 KB/s（146 KiB/s）。拍长比名义值长 ≈18%，且这个差额随 buffer 变大而变大，不是固定偏移。**

### 1.1 源码：播放侧的节拍不在 `SDL_dummyaudio.c`

`sdk/3rdparty/SDL-release-2.28.1/src/audio/dummy/SDL_dummyaudio.c:30-45` 只实现了 `OpenDevice` 与 `CaptureFromDevice`；唯一的 `SDL_Delay` 在**采集**路径（`SDL_dummyaudio.c:37-45`）。dummy 的 `DUMMYAUDIO_Init`（`SDL_dummyaudio.c:47-58`）没有设置 `WaitDevice` / `PlayDevice` / `GetDeviceBuf`，因此这三个入口回落到空实现：

- `SDL_AudioWaitDevice_Default`：空操作（`src/audio/SDL_audio.c:254-256`）
- `SDL_AudioPlayDevice_Default`：空操作（`SDL_audio.c:258-260`）
- `SDL_AudioGetDeviceBuf_Default`：恒返回 `NULL`（`SDL_audio.c:262-265`）

于是音频线程 `SDL_RunAudio` 的每轮循环里 `data == NULL` → 走 `work_buffer` 分支，靠**一次 `SDL_Delay`** 定拍：

```c
} else if (data == device->work_buffer) {
    const Uint32 delay = ((device->spec.samples * 1000) / device->spec.freq);
    SDL_Delay(delay);          // SDL_audio.c:743-746
}
```

每轮从队首取走的字节数 = `callbackspec.size`（`SDL_audio.c:549-566` 的 `SDL_BufferQueueDrainCallback` → `SDL_ReadFromDataQueue(..., len)`，`len = callbackspec.size`，见 `SDL_audio.c:692`）。而 `spec.size` 由 `SDL_CalculateAudioSpec` 定为 `字节宽 × 声道 × samples`（`SDL_audio.c:1728-1734`）：`2 × 2 × 882 = 3528`。

即：**名义**每 20 ms 消耗 3528 B = 176,400 B/s = 172.3 KiB/s。这与 `SDL_dummyaudio.c:40` 无关——ADR 0010 引用的那一行是采集侧，见 §5 偏差第 2 条。

### 1.2 实测：拍长 23.6 ms、每次恰好 3528 B

队列变化点采样（1 ms 轮询，原始读数）：

```
obtained: freq=44100 format=0x8010 channels=2 samples=882 size=3528
nominal tick = samples*1000/freq = 20 ms, bytes/tick = 3528
[B] change points (t_ms, queued): 50 events over 1200 ms
    bytes/tick: n=47 min=3528 p50=3528 max=3528 mean=3528.00
    tick_ms:    n=47 min=20 p50=23 max=27 mean=23.60
    steady rate = 149518 B/s (146.0 KiB/s), period = 23.60 ms
    window rate = 149543 B/s over 1156 ms
```

低扰动复核（20 s，8 MiB 预装，避免队列跑空导致的假读数）：

```
mode=1ms-poll    secs=20 elapsed=20.000 consumed=2995272 rate=149764 B/s (146.3 KiB/s) period=23.56 ms
mode=sparse-poll secs=20 elapsed=20.022 consumed=2998800 rate=149775 B/s (146.3 KiB/s) period=23.56 ms
```

两次运行、两种轮询密度给出同一速率 → 拍长与轮询无关。

### 1.3 拍长偏差随 buffer 变大而变大（不是固定的 +3.5 ms）

用带回调的同一设备（回调计数）扫 `samples`，每档 8 s：

| 请求 samples | 名义拍长 | 实测拍长 | 实测/名义 | 实测速率 |
|---|---|---|---|---|
| 88 | 2.0 ms | 2.86 ms | 1.43× | 123 KB/s |
| 176 | 4.0 ms | 5.76 ms | 1.44× | 122 KB/s |
| 441 | 10.0 ms | 11.61 ms | 1.16× | 152 KB/s |
| **882（本项目）** | **20.0 ms** | **23.53–23.60 ms** | **1.18×** | **149.5 KB/s** |
| 1764 | 40.0 ms | 46.79 ms | 1.17× | 151 KB/s |
| 3528 | 80.0 ms | 91.95 ms | 1.15× | 153 KB/s |
| 7056 | 160.0 ms | 181.82 ms | 1.14× | 155 KB/s |

对照实验：在等价线程（64 KB 栈 + `SDL_THREAD_PRIORITY_TIME_CRITICAL`）里直接 `SDL_Delay(20)`，实测 **20.49 ms**（min 20 / p50 20 / max 21）；`SDL_Delay(10)` = 10.61 ms。也就是说 `SDL_Delay` 本身没有这个偏差，多出来的部分发生在音频线程的每轮循环内，并随单轮字节数增长（`SDL_audio.c:743-746` 的 delay 是唯一显式等待点，因此差额只能来自该轮其余工作与唤醒延迟）。本文不把成因归到某一行源码：**结论只用到「实测 23.6 ms」这个数**。

> 跨平台注意：`sdk-windows.yml:46-48` 跑 `windows-latest`，`sdk-coverage.yml:68-69` 跑 Linux。Linux 的 `SDL_Delay` 是 `nanosleep`，拍长通常贴近名义 20 ms；上表是 Windows 值。任何把拍长写死进推算式的地方都应以 23.6 ms 为上限取值（见 §4）。

### 1.4 两个容易被忽略的前置条件

- **未 `start` 不消耗**：入队 200,000 B 后静置 120 ms，读数不变（`[A] before start: q=200000 after 120ms q=200000 (delta=0)`）。对应 `AudioDevice::start()` 才调 `SDL_PauseAudioDevice(id, 0)`（`sdk/audio/device/AudioDevice.cpp:145-153`）。
- **暂停即停消耗**：暂停后入队 4 MiB，150 ms 内读数完全不变（`[E] paused 4MiB: rc=0 queued=4194304, after 150ms=4194304`）。对应 `SDL_audio.c:714-718` 的 paused 分支只填静音、不调回调。

---

## 2. 容量上限：`SDL_QueueAudio` 有无隐藏上限？超过 256 KB 后是否仍线性？

**结论：没有隐藏上限。队列是 8 KiB 包的链表，只有 OOM 才会失败；超过 256 KB 后读数严格线性增长（实测 4 MiB 级精确到字节）。唯二的「上限」都在接口类型上：读数返回 `Uint32`（>4 GiB 回绕），以及 `AudioDevice::write` 把 `size_t` 隐式截断成 `Uint32`。**

### 2.1 文档与实现

- 文档承诺：「There are no limits on the amount of data you can queue, short of exhaustion of address space.」（`sdk/3rdparty/SDL-release-2.28.1/include/SDL_audio.h:1195-1196`）
- 实现只在分配失败时返回错误：`SDL_WriteToDataQueue`（`src/SDL_dataqueue.c:175-231`）逐包 `SDL_malloc`（`SDL_dataqueue.c:155-159`），失败即整体回滚并 `SDL_OutOfMemory()`（`SDL_dataqueue.c:200-217`）。`queued_bytes` 是 `size_t`（`SDL_dataqueue.c:41`），没有计数上限。
- 包长固定 8 KiB（`src/audio/SDL_sysaudio.h:63` 的 `SDL_AUDIOBUFFERQUEUE_PACKETLEN (8 * 1024)`），创建于 `SDL_audio.c:1470-1481`，初始池 = 两个回调的余量。

### 2.2 实测：跨过 256 KB 后逐字节线性

4096 B × 1024 次入队（合计 4 MiB），全部成功，读数与理论值完全相等：

```
[C] base queued = 23600, enqueue 4096 B x 1024 (4 MiB total)
    after 256 KiB: queued=285744
    after 512 KiB: queued=547888
    after 1024 KiB: queued=1072176
    after 2048 KiB: queued=2120752
    after 4096 KiB: queued=4217904
[C] final queued=4217904 fails=0 (expected 4217904 if fully linear)
[C] after clear: queued=0
```

单块极端值：`[D6] single 4 MiB: rc=0 queued=4194304`、`[D7] single 32 MiB: rc=0 queued=37748736`——32 MiB 单次入队也不失败。

### 2.3 两个真实的「上限」都是类型，不是策略

1. **读数是 `Uint32`**：`SDL_GetQueuedAudioSize` 返回 `(Uint32)SDL_CountDataQueue(...)`（`SDL_audio.c:624-642`，截断在 `:637`）。队列超过 4,294,967,295 B 时读数回绕而不是报错。以 149.8 KB/s 计，约 7.9 小时连续播放才会触及；对本项目不是现实风险，但「读数单调线性」这一断言只在 4 GiB 以下成立。
2. **写入长度是 `Uint32`**：`AudioDevice::Impl::write(const void *data, size_t len)` 把 `len` 直接传给 `SDL_QueueAudio(..., Uint32 len)`（`AudioDevice.cpp:165-175`）。≥4 GiB 的单块会被静默截断（写入量小于调用者以为的量，`rc` 仍为 0）。

**因此「队列到达上限」只可能来自应用侧**：`kQueueThreshold = 256 * 1024`（`sdk/MusicPlayer.cpp:668`），喂料条件 `if (m_audioDev->getQueuedBytes() > kQueueThreshold) return;`（`MusicPlayer.cpp:669-671`）。这与 ADR 0010 的裁定一致（`docs/adr/0010-test-injection-seams-and-criteria.md:35`），只是行号已从 648 漂到 668。

---

## 3. 失败面：dummy 下 `SDL_QueueAudio` 有哪些可构造的失败条件？

| # | 构造方式 | 实测结果 | 判定 |
|---|---|---|---|
| F1 | 未 open 即写（`devid = 0`） | SDL 层 `rc=-1`，`Invalid audio device ID`；项目层 `AudioDevice::write` 先短路返回 `-1`，不产生 SDL 错误 | **可稳定复现**（100%） |
| F2 | 已 close 后写（旧 id） | `rc=-1`，`Invalid audio device ID`；读数 `0` | **可稳定复现** |
| F3 | 重复 start | `AudioDevice::start()` 是 `void` 且有 `m_isStart` 守卫（`AudioDevice.cpp:145-153`），重复调用只打日志；`SDL_PauseAudioDevice(id,0)` 无错误返回 | **不可复现**（不存在失败路径） |
| F4 | 极端大块 | 4 MiB / 32 MiB 单次入队 `rc=0`；唯一失败是内存耗尽 | **不可复现**；注意 ≥4 GiB 的静默截断（§2.3） |
| F5 | 驱动线程已退出 | 正常路径不可达：`close()` 会 join 线程并把 id 置 0（`AudioDevice.cpp:127-138`）。`SDL_Quit()` 后仍拿旧 id 写入 → `rc=-1`（实测），但该状态要求 `AudioDevice` 自身已析构（`AudioDevice.cpp:84-87` 析构里 `SDL_Quit`），属 use-after-free | **不可复现**（经由项目 API） |
| F6 | 带回调的设备上排队 | SDL 拒绝：`Audio device has a callback, queueing not allowed`（`SDL_audio.c:593-595`） | **不可达**：`open()` 固定 `callback = NULL`（`AudioDevice.cpp:74-79`） |
| F7 | 采集设备上排队 | SDL 拒绝：`This is a capture device, queueing not allowed`（`SDL_audio.c:591-592`） | **不可达**：只用 `iscapture=0`（`AudioDevice.cpp:115`） |
| F8 | write 与 close 并发 | 无锁竞态：`AudioDevice::write` 的 `m_isOpen` 判断与 `SDL_QueueAudio` 之间没有互斥（`AudioDevice.cpp:167-170`），SDL 侧 `get_audio_device` 也不持锁（`SDL_audio.c:222-231`），设备结构可能已被释放 | **不可稳定复现**（真竞态，不建议构造判据） |

原始读数：

```
[D] failure surface:
    D1 queue to devid 0 (未 open): rc=-1 err=Invalid audio device ID
    D3 queue on capture device: rc=-1 err=This is a capture device
    D4 queue len=0: rc=0 queue=0
    D5 double start: no error path
    D6 single 4 MiB: rc=0 queued=4194304
    D7 single 32 MiB: rc=0 queued=37748736
    D8 queue after close (stale id 2): rc=-1 err=Invalid audio device ID
    D8 getQueued after close: 0
```

F2 有一个**危险变体**必须单列：device id 会被复用。实测同一进程内 close 再 open 拿到同一个 id：

```
first  id=2 err=''
second id=2 err=''
queue to stale id 2 after reopen (now id 2): rc=0 err='' queued(on stale)=1024
```

即「拿旧 id 写」在 id 被复用时会**静默写进新设备并返回成功**。它不是一个可用的失败检测点，反而是「按设备读数 / 按设备写入」在 close-reopen 场景下的静默错投模式——这从反面支持 ADR 0010 拒绝「进程级静态当前设备」的理由（`docs/adr/0010-...:21`、`:44`）。

另有一条与测试直接相关的硬约束：dummy 声明 `OnlyHasDefaultOutputDevice = SDL_TRUE`（`SDL_dummyaudio.c:53`），于是 `SDL_OpenAudioDevice(NULL, ...)` 在**同进程内只能开一个输出设备**，第二次返回 `0` 且错误为 `Audio device already open`（`SDL_audio.c:1334-1348`；实测 `[D2] open with callback: id=0 err='Audio device already open'`）。这解释了为什么多 player 场景不可能「各持一个设备」，也说明第二个设备不能用来构造写失败。

---

## 4. 时间窗余量：20 ms 拍长下，「窗口内观察到两个 `PlaybackTask` 并存」最短窗口与抖动

**结论：观测本身不受 20 ms 拍驱动（判据读的是 decoder 仪表，窗口由解码线程退出延迟决定）。若判据非要借「设备队列读数」证明窗口仍开着，最小可观察单位是 1 拍 ≈ 23.6 ms，抖动上界 27 ms；因此窗口下限应取 ≥ 2 拍（≈47 ms），按 10× 纪律应断言 ≥ 240 ms，不能按名义 20 ms 推算。**

### 4.1 这条判据的窗口不由音频拍决定

切换窗口的定义是「新任务 `start()` 起 → 旧任务 join 完成止」（ADR 0010 补充 §2，`docs/adr/0010-...:72-80`），证据是两侧 decoder 的 `peakHeldBytes` 与线程退出标志：

- `sdk/test/unit/test_playback_lifecycle.cpp:340-353`：切歌后旧任务保活、新任务已 `start()`，等到 `oldTask->decoder->isThreadFinished()` 才算窗口闭合。
- 全程不碰设备、不碰消费拍（`MusicPlayList` 级即可构造），所以 20/23.6 ms 的拍长**不是**这条判据的采样粒度。

### 4.2 若必须用「队列读数」当窗口开着的凭据

此时可观察到的最小时间单位就是一拍。实测抖动（882 samples 配置，两次 3 s 采样，去除首尾）：

```
[T1] n=126 min=21 p10=22 p50=23 p90=25 max=26 mean=23.52 | hist: 21ms x4 22ms x14 23ms x49 24ms x36 25ms x18 26ms x5
[T2] n=126 min=21 p10=22 p50=24 p90=25 max=26 mean=23.56 | hist: 21ms x5 22ms x15 23ms x41 24ms x42 25ms x16 26ms x7
```

- 单拍：p50 = 23–24 ms，p90 = 25 ms，max = 26–27 ms（1 ms 轮询下的上界），相对名义 20 ms 的抖动约 −3.6 / +3.4 ms。
- 结论：**1 拍（23.6 ms）在峰上会漏采**（27 ms 那些拍），窗口 ≥ 2 拍（≈47 ms）才有 2× 余量；ADR 的 ≥10× 纪律要求窗口 ≥ 236–270 ms。

### 4.3 观测侧还有一个更粗的粒度

消费拍是 `WorkQueue` 的 10 ms 定时任务（`sdk/MusicPlayer.cpp:790`：`startTimerTask(true, 10, ...)`），喂料上限每拍 4096 B（`MusicPlayer.cpp:673`）。本机同栈的 10 ms 周期代理实测：

```
sleep_for(10ms): min=14 p10=15 p50=15 p90=16 max=30 mean=15.26
```

即 Windows 默认 15.6 ms 定时粒度下，「10 ms 拍」实际约 15–16 ms（偶发 30 ms）。**这是代理值，不是 `WorkQueue` 的实测**——但它足以说明：任何以「拍数」推算窗口长度的判据，在 Windows 上都要按 1.5 倍以上放大，不能按标称 10 ms / 20 ms 计算。

顺带一个可用的量化：喂料 4096 B/拍 = 409,600 B/s（按名义 10 ms）或 268,410 B/s（按实测 15.26 ms），消耗 149,800 B/s，净积累 259,800 / 118,610 B/s，故队列从 0 涨到 256 KB 需 1.01 s（名义）到 2.21 s（最坏）。`sdk/test/unit/test_player_integration.cpp:1343` / `:1386` 的 64 KB / 192 KB 各留 3000 ms：64 KB 余量 5.4–11.9×，192 KB 余量 1.8–4.0×——两处都是 `skipCase` 而不是断言，所以 1.8× 的最坏情形只是让用例跳过，不会假绿。

---

## 5. 与 ADR 0010 既有读数的对照

### 5.1 一致（本次实测复现）

| ADR 0010 的说法 | 位置 | 本次结果 |
|---|---|---|
| 实测量子 3528 B | `0010-...:13` | 全部 tick 的 delta 恒为 3528 B（n=47，min=max=3528） |
| 有效拍长 ≈ 23.6 ms | `0010-...:13`、`:34` | 23.52–23.60 ms（20 s 平均 23.56 ms） |
| 有效速率 ≈ 150 KB/s | `0010-...:13` | 149.5–149.8 KB/s（146 KiB/s） |
| 256 KB 是应用侧阈值，SDL 侧不设限 | `0010-...:35` | 4,217,904 B 精确线性、零失败；单次 32 MiB 成功 |
| 探针 200 × 4096 B 零失败 | `0010-...:55` | 1024 × 4096 B 零失败（更强） |
| `deviceWriteFailures` 在 dummy 上不可构造 | `0010-...:55` | 四种可构造路径全部不可达（§3） |
| 观测窗口 ≥ 一拍 | `0010-...:34` | 成立，且补充上界：一拍内抖动到 27 ms |

### 5.2 需要更正的三处（都不是读数冲突，是引用与表述）

1. **`kQueueThreshold` 行号漂移**：ADR 0010:35 写 `sdk/MusicPlayer.cpp:648`，现为 `sdk/MusicPlayer.cpp:668`（`const size_t kQueueThreshold = 256 * 1024;`）。数值未变。
2. **拍长的源码归属错了一处**：ADR 0010:13 把 dummy 的按拍消耗归到 `SDL_dummyaudio.c:40`。该行在采集路径（`DUMMYAUDIO_CaptureFromDevice`）里；播放侧的定拍点是 `src/audio/SDL_audio.c:743-746`（dummy 没有 `WaitDevice`，落回空实现 `SDL_audio.c:254-256`，`GetDeviceBuf` 返回 `NULL` → `work_buffer` 分支），每拍取走 `callbackspec.size`（`SDL_audio.c:549-566`）。结论（23.6 ms / 150 KB/s）不受影响，但「dummy 驱动按 samples/freq 一拍真实消耗」这句话的出处应改。
3. **「观测窗口 ≥ 一拍」应加抖动上界**：ADR 0010:34 只给了 23.6 ms 的典型值；实测单拍上界 27 ms（§4.2），窗口取单拍会在峰上漏采，宜写成「≥ 2 拍」。

### 5.3 本次新增、ADR 未记录的事实

- 拍长偏差随 buffer 变大而变小（88→7056 samples 扫描，1.43×→1.14×），名义 20 ms 不能当作实测拍长。
- **同进程只能开一个输出设备**（dummy 的 `OnlyHasDefaultOutputDevice`，`SDL_audio.c:1334-1348`）——多 player 并行开设备的测试不可能成立。
- **device id 会复用**，旧 id 写入在 close-reopen 后会静默写进新设备并返回 0（§3 末尾）。
- 读数的 4 GiB 回绕与写入的 `Uint32` 截断（§2.3）。
