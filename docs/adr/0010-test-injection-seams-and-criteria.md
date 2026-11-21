# 0010 测试注入面与判据观测点：设备队列读数、供给限速与三条判据的判别式

- 状态：已接受
- 日期：2026-11-17
- 来源：[地图：三条待验证播放行为判据的可测试化](https://github.com/wt159/SoundBridge/issues/184)、[队列读数的出口形状](https://github.com/wt159/SoundBridge/issues/185)、[静默窗口口径二选一](https://github.com/wt159/SoundBridge/issues/186)、[欠流判据的场景构造与断言式](https://github.com/wt159/SoundBridge/issues/187)、[切换失败不净空的判据与成功路径对照](https://github.com/wt159/SoundBridge/issues/188)、[EOS 不净空的判据场景](https://github.com/wt159/SoundBridge/issues/189)、[dummy 驱动下队列读数的实测标定](https://github.com/wt159/SoundBridge/issues/191)。规格落点 [`doc/buffering-switching-spec.md`](../../doc/buffering-switching-spec.md) §7.1。

## 背景

`doc/buffering-switching-spec.md` §7.1 把三条行为判据登记为「未自动化」：欠流计数与静默窗口不计数、EOS 不净空、切换失败不净空。三者的写入点都在 `MusicPlayer::Impl::consumeAudioData()` 的 10 ms 消费拍里，而测试够不到那里：`MusicPlayer` 的公开面只有控制与列表操作（`sdk/MusicPlayer.h:56-72`），既不暴露设备队列读数，也不暴露 per-decoder 仪表。

两个容易走错的前提，先在此钉死：

1. **CI 是有音频设备的。** 两个工作流都设 `SDL_AUDIODRIVER=dummy`（`.github/workflows/sdk-windows.yml:46-48`、`.github/workflows/sdk-coverage.yml:68-69`）。SDL 的 dummy 驱动按 `samples / freq` 一拍真实消耗队列（`sdk/3rdparty/SDL-release-2.28.1/src/audio/dummy/SDL_dummyaudio.c:40`），实测量子 3528 B、有效拍长 ≈ 23.6 ms、有效速率 ≈ 150 KB/s。所以「判据必须设备无关」是伪约束。
2. **`MusicPlayer` 级没有供给注入路径。** `addMusicDir` 经 `recursiveFileSearch` 按 `defaultExtractorMap`（`sdk/extractor/ExtractorHelper.hpp:32-44`，`static const`）过滤扩展名，白名单内扩展名在静态初始化期被 `REGISTER_EXTRACTOR_*` 占满，`registerExtractor` 重名返回 `false`。合成提取器只能从 `MusicPlayList` 进入，够不到消费拍。

## 决策

**读数走遗留类上的两个 `ForTest` 出口；慢供给走 `FileSource` 的读延迟开关（解码线程读点，本文件的显式例外）；三条判据的判别式统一为「归零 / 非零 + 10× 余量」。**

- **读数出口**：`MusicPlayer::queuedBytesForTest()` 与 `MusicPlayer::metricsForTest()`，均 `const`、直通 `Impl`。**不进 `soundbridge::Player`**——`Player::Impl` 只是 `MusicPlayer` 的适配层（`sdk/player_impl.cpp:141`），判据穿过它会多一层状态映射；且 `sdk/include/soundbridge/` 是 `BUILD_INTERFACE` / `INSTALL_INTERFACE` 标出的新公开 API 面（`sdk/CMakeLists.txt:42-46`），带 `ForTest` 后缀也不进。
- **不做进程级静态观测**：`SDL_GetQueuedAudioSize` 是按设备的读数，静态「当前设备」在多 player 测试里会静默读错并返回合法非零值；实例绑定没有这个失败模式。
- **供给限速**：`FileSource::setReadDelayForTest(uint32_t ms)`，进程级 `std::atomic<uint32_t>`、`store(release)` / `load(acquire)`、默认 0、**只对 `size >= 4096` 的读生效**。以 4096 为界把「64 KB 负载包」与「8~70 B 的头部 / 格式探测」分开，且这条分界线与 `prefillTargetBytes` 的下限 4096 同源。
- **纪律例外及其边界**：`AGENTS.md` 要求测试钩子绝不落解码线程 / 消费主循环 / 判据主循环。`FileSource::readAt` 正在解码线程。例外成立的理由是：该纪律的原始意图是「不给热路径加无谓原子读」，而限速钩子的读正是它的功能本身。边界写死为——只此一处、只这一个钩子、默认 0、无缓存、不得照抄成通例；任何新的解码线程钩子必须重开一张票。
- **`silenceFills` 的口径**：维持只覆盖 SEEKING。计数器只报告它确定的事实——解码器看不到 `m_switching` / `shuttingDown`（`sdk/audio/decode/AudioStreamDecoder.cpp:346` 自述）。把窗口判定复制到解码器会得到两份会漂移的真源。
- **三条判据的判别式**：

  | 判据 | 场景 | 断言 | 余量 |
  |---|---|---|---|
  | 切换失败不净空 | 单曲列表 `next()` → `endSegmentSwitch(false)` | `q1 > 0` 且 `q0 − q1 <= 16384`（≈4 拍自然消耗） | 清空时差值 ≥ `q0`（数百 KB） |
  | 切换成功净空（对照） | 两曲列表 `next()` → `endSegmentSwitch(true)` | `q2 < q0 / 4` | 净空后队列近空 |
  | EOS 不净空 | 2 s 合成 WAV 播到 EOS | EOS 后 100 ms 内 `q > 0` | ≈1.75 s / 100 ms = 17.5× |
  | 稳态欠流计数 | 2 s WAV + 500 ms 读延迟 | `underrunTicks >= 3`、`<= elapsedMs / 10`、`silenceFills == 0` | 首个包到达前约 8 拍 |

- **确定性约束**（对后续新增判据同样生效）：断言 ≥10× 时间余量；观测窗口 ≥ 一拍（实测 23.6 ms）；只判别「归零 / 非零」，不断言绝对字节数。
- **队列上限的归属**：256 KB 是应用侧 `kQueueThreshold`（`sdk/MusicPlayer.cpp:648`），SDL 侧不设限（探针实测连续入队 819,200 B 全部成功）。所以「队列到达上限」只能由消费端的喂料条件产生。
- **读数为 0 的歧义**：设备未 open 与「已 open 但空」读数同为 0，任何判据都不能用「读数为 0」单独推断净空发生，必须与「设备处于 open 状态」绑定。

## 考虑过的替代方案

**(a) 设备后端替身（把 SDL 队列抽成内存队列）。** 能断言「尾音顺序」这类更高精度的事实。否决：dummy 驱动已提供真实按拍消耗的队列，替身换不来精度；且要改 `MusicPlayer::Impl` 构造链与全部 SDL 调用点。它只在本图目的地被重画为「提高判据精度」时才有意义。

**(b) 读数挂在 `soundbridge::Player` 上。** 判据可写在新公开 API 上。否决：适配层会吞被测行为，且与「不新增公开能力位」的既有裁定（E1–E15、#104）在同一张纸上解释不清。

**(c) 读数走 `AudioDevice` 的进程级静态。** 最贴合既有钩子落点（内部类型 + static + 原子）。否决：按设备读数 + 多实例 = 静默读错，且错得不像错误。

**(d) 修 `noteSilenceFill()` 的过滤器，让计数覆盖四类窗口。** 否决：会改既有观测基线（`StreamingBudget` 历史数值与规格 §5 的「已知代价」段），收益只有「计数更全」，没有判据消费它；更糟的是它会让欠流场景的对照组随 `m_switching` 抖动而失效。

**(e) 用 `MusicPlayList` 级合成提取器驱动判据。** 无需任何生产改动（`addMusic` 接受任意单路径、`FileSource::initCheck` 只要求能打开）。否决：三条判据的写入点在 `MusicPlayer::Impl::consumeAudioData()`，`MusicPlayList` 级没有消费拍。

## 后果

- 生产代码新增两个 `ForTest` 出口与一个进程级原子；默认路径零开销（`load(acquire)` + 一次 `delay > 0` 判断）。
- 解码线程被允许持有一个测试开关的读点。代价是纪律出现一处例外，靠本文件与 `FileSource.h` 的注释双重约束；收益是欠流判据从「不可自动化」变为可红可绿。
- 判据只对 dummy 驱动的行为负责。真实设备（在飞块、硬件水位、`SDL_QueueAudio` 失败）仍只由人工冒烟覆盖——这一点在规格 §7.1 的「可测不等于已自动化」注记里明确保留。
- `deviceWriteFailures` 在 dummy 驱动上不可构造（探针实测 200 × 4096 B 零失败），它不在本 effort 的三条判据内，将来要覆盖必须另找设备替身。

## 补充（2026-11-20）：两条遗留判据的落点与证明力边界

来源：[地图：首播超时降级与切换窗口峰值的判据落地](https://github.com/wt159/SoundBridge/issues/194)。规格落点 [`doc/buffering-switching-spec.md`](../../doc/buffering-switching-spec.md) §7.1.2。

### 1. `PlayingState` 作为「设备已 start」代理的边界

`_play()` 的顺序是 open → 等门槛 → `m_audioDev->start()` → 起消费定时器 → 置 `PlayingState`。判据用「观察到 `PlayingState`」代理「设备仍 start」，它能证明的只有一件事：**控制流穿过了 `start()` 调用点**。

它证明不了的事必须明写：

- `AudioDevice::start()` 恒返回 0，`_play()` 里的 `AudioDeviceStartFailed` 分支不可达；因此「设备真的开始消耗队列」没有任何自动化证据，dummy 驱动下尤其如此。
- 设备侧的真实失败（硬件水位、飞块、`SDL_QueueAudio` 报错）仍只由人工冒烟覆盖。

之所以不加 `isStartedForTest()`：代理证据的失败模式是「假绿」——分支不可达时观察不到红；而新增公开钩子会把这个不可达分支的语义钉进 API 面，成本高于收益。分支一旦变得可达（`start()` 返回真实结果），这条代理必须重开。

### 2. 切换窗口 per-decoder 上界的取值写法

窗口 = 新任务 `start()` 起 → 旧任务 join 完成止（规格 §5.3）。判据对窗口内并存的两个 decoder 各自断言：

`peakHeldBytes ≤ AudioRingBuffer::kDefaultCapacity + 2 × P + 64 KB`

- `P` 由素材实测（`PacketCursor::nextPacket()` 逐包取 `max(packet->size())`），不写死常量：wav 是 64 KB 定长切片，压缩格式随帧长变化。
- 与 `test_streaming_budget.cpp` 的稳态上界同一写法。`peakHeldBytes` 本身不含 ring 与 AVIO（既有口径），公式里的这两项因此是余量而不是重复计算。
- 这条上界比规格 §5.3 的字面口径（「各自 `peakHeldBytes ≤ P`」）宽，差异是有意的：实测解码器稳态会同时持约 4 包（本机 `peakHeldBytes = 266240 B`，`P = 65536 B`），字面口径本来就成立不了。取宽上界的目的仍是「断言有界、且能被打红」，分辨力由「整片常驻」这一类回归保证（实测 1062068 B）。§5.3 既有行不改，差异登记在规格 §7.1.2。
- **旧** decoder 的读数在其线程退出后取：`metrics()` 是原子快照，线程退出后峰值冻结，提前读只会读到偏小的中间值。新 decoder 的线程仍在跑，读到的是当时的上界快照——对上界断言无害，但它是下界性质的证据，不是最终峰值。
- 两条读数都附加 `> 0` 检查，避免「读到的不是本次任务的记账」被空快照蒙过。

### 3. 为什么两条判据都不需要新接缝

- 门槛侧：`FileSource::setReadDelayForTest()`（本 ADR 允许的唯一解码线程钩子）让首包晚于 300 ms 门槛；`MusicPlayer::metricsForTest()` 已能读到 `prefillTimeouts`。场景与 `steady-state underrun is counted` 同源，但单独成条——后者已断言 `silenceFills == 0`，混在一起会让一条红了两因难分。
- 峰值侧：`AudioStreamDecoder::metrics()` 是 public，`MusicPlayList` 的同步控制入口足以构造「两个 task 并存」的窗口，不需要设备、也不需要消费拍。
- 两条都遵守既有确定性约束：事件 + 上限、≥10× 余量、不断言绝对字节数。

### 4. 内存上界类判据的「能红」形态

上界由素材决定，「把上界调小」属于改断言、不是变异。可接受的替代形态是**变异被测读数**：把 `WAVExtractor::heldPayloadBytes()` 临时改成返回 `m_dataSize`（复刻整首常驻的旧供给形态），两条断言随即变红（实测 1062068 B > 458752 B）；还原后复绿。它证明的是「断言读的是真实记账路径、且有分辨力」，不是「上界选得对」——后者只由 §5.3 的公式与 P 的实测值保证。

## 附注（2026-11-21）：设备替身的前提已变

本文件「考虑过的替代方案 (a)」否决了设备后端替身，理由是当时的目的地不含写失败、dummy 队列已够真实。地图 [#200](https://github.com/wt159/SoundBridge/issues/200) 把目的地重画为「三条设备层判据可判定」，该否决的前提随之失效；设备替身改由 [ADR 0011](0011-device-double-and-injection-surface.md) 决策（抽接口 + 装饰器）。上文 (a) 段与「后果」段原文保留不动。

三处事实漂移按「只登记、不改既有行」处理，来源为 [#201](https://github.com/wt159/SoundBridge/issues/201) 的一手实测：

- 第 13 行引用的 `sdk/3rdparty/SDL-release-2.28.1/src/audio/dummy/SDL_dummyaudio.c:40` 是**采集**路径；播放侧定拍在 `src/audio/SDL_audio.c:743-746`（dummy 不设 `WaitDevice`，走 `work_buffer` 分支）。结论数值（3528 B / ≈23.6 ms / ≈150 KB/s）不变。
- 第 35 行 `sdk/MusicPlayer.cpp:648` 现为 `:668`；`256 * 1024` 未变。
- 「观测窗口 ≥ 一拍」应收紧为 **≥2 拍**：实测拍长 23.52–23.60 ms、抖动上界 27 ms，一拍不足；用队列读数当窗口凭据时取 ≈47 ms。
