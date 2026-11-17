# 缓冲与会话切换策略

- 状态：定稿（2026-11-16）
- 来源：[地图：播放链路缓冲与会话切换策略](https://github.com/wt159/SoundBridge/issues/173)；决策票 [#176](https://github.com/wt159/SoundBridge/issues/176) / [#177](https://github.com/wt159/SoundBridge/issues/177) / [#178](https://github.com/wt159/SoundBridge/issues/178) / [#179](https://github.com/wt159/SoundBridge/issues/179) / [#180](https://github.com/wt159/SoundBridge/issues/180) / [#181](https://github.com/wt159/SoundBridge/issues/181)；事实票 [#174](https://github.com/wt159/SoundBridge/issues/174) / [#175](https://github.com/wt159/SoundBridge/issues/175)
- 事实基线：`master @ be5d85365`，取证记录见 `research/prefill-evidence` 与 `research/metrics-and-injection` 两个 throwaway 分支
- 关系：本规格只回答「播放会话的缓冲与会话切换」四件事；增量供给契约、`positionMs` 口径、三态与错误码仍以 `doc/streaming-playback-spec.md`、`doc/player-state-contract.md` 为准。**既有规格的既有行一字不动**，接缝只加时点注记（§9）

## 1. 问题与现状

| 症状 | 现状证据（`be5d85365`） | 后果 |
|---|---|---|
| 起播空窗 | `_play()` 顺序为「建任务 → 设备 open → 设备 start → 起消费定时器」，全程无预填充（`sdk/MusicPlayer.cpp:315-337`）；规格承诺的「首播阈值 4096 B」未实现 | 设备 start 时队列为空，首帧前的输出由 SDL / 驱动兜底 |
| 切歌卡顿 | `replacePlaybackTask()` 先 `stopCurrentTask()`（`stop()` + `join()`）再建新任务（`sdk/MusicPlayList.cpp:456-490`）；join 与 10 ms 消费定时任务同处一条 WorkQueue 线程 | 切歌时消费节拍被 join 顶住；网络源最坏可到 `rw_timeout=3 s` |
| seek 残留 | `_setPosition()` 只写 seek 单槽，不 `clearQueue`（`sdk/MusicPlayer.cpp:388-418`） | 设备队列最多 256 KB 旧 PCM（≈1.49 s @44.1k/2ch/S16）先播完才听到新位置 |
| 欠流不可见 | 消费端取不到数据时静默跳过本轮，不填静音、不计数（`sdk/MusicPlayer.cpp:547-570`）；`noteSilenceFill()` 实际只覆盖 SEEKING（`sdk/audio/decode/AudioStreamDecoder.cpp:318-321`） | 「稳态静音填充为零」判据抓不到真实欠流 |

**三个不变**（本规格的前提，不重开）：10 ms 定时 push + 每拍最多 4096 B；ring 无 mutex、单生产者单消费者；`positionMs = (segmentBaseBytes + readPos) / bytesPerMs`。

## 2. 首播门槛

### 2.1 定义与度量

**首播门槛**：冷启动播放时，设备 `start()` 之前要求 ring 里已攒够的一段 PCM。它不是判据阈值，而是**起播前的准入条件**。

| 参数 | 取值 | 说明 |
|---|---|---|
| `prefillMs` | 150 ms | 依据 #174：11 种格式攒够 150 ms 的中位 0.44–4.44 ms、最慢 9.05 ms（m4a-AAC），余量 16× |
| 字节换算 | `max(4096, rate × channels × bitsPerSample/8 ÷ 1000 × 150)` | 按设备 spec；生产链 44100/2/S16 ⇒ 26 460 B |
| 下限 | 4096 B | 设备 spec 无 PCM 格式时退回源 spec 同式换算；两者都不可用时用 4096 B |
| 等待上限 | 300 ms | 门槛的 2 倍迟滞；本机与 CI 正常路径都不会触发（最慢 9.05 ms / 1.54 ms） |
| 检查粒度 | 1 ms | 与解码线程写 ring 的背压 `sleep_for(1ms)` 同款 |

**不分档**：不按采样率 / 声道 / 格式建表；统一毫秒数，字节换算天然适配。

### 2.2 等待者与顺序

`MusicPlayer::Impl::_play()` 的控制线程，顺序固定：

1. `startCurrentTask()`：建 ring + decoder，解码线程开跑
2. `m_audioDev->open()`（若未打开）
3. **等待 `ring->availableRead() >= prefillBytes`，上限 300 ms**
4. `m_audioDev->start()`
5. `startConsumerThread()`

等待期间设备已 open 但未 start，SDL 不消耗队列；消费者尚未启动，不存在「等待消费侧产出」的自锁。

**等待条件只能是 ring 累计写入量**，不能是「设备首次 write」：后者由消费者产出，而消费者在 `_play()` 返回后才启动（#175 取证结论）。

### 2.3 超时与失败

- 超时：**照常 `start()`**，不清队列、不报错；解码器仪表 `prefillTimeouts` 加一（per-decoder，随 `start()` 清零）。同一任务内不再等第二次，不临时降低门槛。
- 等待期解码线程进入 `ERROR` / `IDLE`：立即退出等待；`ERROR` 走既有 `handleDecodeError`（设备不必 `start`），`IDLE` 不设失败分支——放行起播，由既有 EOS 汇总与切换分支接管（极短素材 / 空源会走到这里）。总等待仍 ≤ 300 ms。
- 网络源：`fetchHead()` 嗅探在提取器构造期完成，不在等待窗口；首包可能受 `rw_timeout=3 s` 支配，300 ms 到点放行（§2.5）。

### 2.4 豁免

- **pause → resume 不重复等待**：复用同一任务，ring 里的 PCM 还在，只需设备 `start`。
- **LIVE 重连不等待**：重连在解码线程内完成，不经过 `_play()`（§2.5）。
- **seek 不等待**：SEEKING 静默窗口负责空档，不延长既有的 12 ms 窗口。

### 2.5 与 LIVE / 网络源的接缝

- 首播门槛只在**播放会话冷启动**生效；LIVE 首次 open 走同一路径，重连重开不重复。
- 网络源首帧由网络支配，300 ms 上限会例行打满 ⇒ 到点放行是**设计行为**，不是降级缺陷。
- 不为网络源单独放宽上限或跳过等待：放宽会让本地文件重新背上长等待，跳过会让本地起播空洞回到现状。

## 3. 设备队列净空

### 3.1 范围

| 事件 | 净空设备队列 | 门闩 |
|---|---|---|
| `next` / `previous` / `setCurrentIndex`（用户发起） | **是** | `m_switching = true` → 切任务 → `clearQueue()`（仅在切换真的发生时） |
| `seek` | **是** | 不设 `m_switching`；SEEKING 窗口已存在 |
| EOS 自动跳轨 | **否** | `m_switching.exchange(true)` 唯一赢家 |

**EOS 例外的论证**：EOS 时 ring 已空，但设备队列仍可能有最多 256 KB 未播完的尾音。SDL 不暴露已播位置，`SDL_GetQueuedAudioSize` 不含在飞块（#174 实测少算约 20 ms），因此在此处净空只能**截断尾音**，违反 B4「上报位置必须是用户实际听到的位置」与「已解出 PCM 先播完再上报」。故 EOS 让尾音自然播完，新曲 PCM 按 FIFO 排在队尾。

### 3.2 出口与顺序

- 新增私有 `MusicPlayer::Impl::beginSegmentSwitch()`（`bumpTraceId` → `m_switching.store(true)`）与配对的 `endSegmentSwitch(bool switched)`（`switched == true` 时 `m_audioDev->clearQueue()`）；`_next` / `_previous` / `_setCurrentIndex` 三处改调这一对。净空放在切换真的发生之后：无下一首 / 索引越界时不再白白清掉正在播的 PCM。切换与控制事件同处一条 WorkQueue 线程，两者之间插不进写入，故与「切换前净空」对设备队列等价。
- `_setCurrentIndex` 的既有缺陷（当前漏调 `clearQueue`）随本规格修复。
- `seek` 在参数校验全部通过后、`seekToMs()` 之前调 `clearQueue()`；不设 `m_switching`。
- 切歌失败分支仍 `m_switching.store(false)`，不改变既有回滚语义。

### 3.3 位置语义

净空只**缩小**「上报位置 vs 实际听到」的领先量，不扩大。`positionMs` 公式、单调性、seek 立即上报一次的口径全部不动。

### 3.4 竞争与合并

- 连续 seek：后一次覆盖前一次（现状）。
- switching 期间 seek：显式丢弃（`_setPosition` 的无任务 / 跨曲检查）。
- EOS 跳轨与用户切歌竞争：`m_switching.exchange(true)` 决定唯一赢家（现状）。

## 4. 欠流与设备写失败的可观测

### 4.1 定义

| 计数器 | 定义 | 落点 |
|---|---|---|
| `underrunTicks` | `decoder->state() == DECODING` 且 `!shouldFillSilence()` 且 `ring->availableRead() == 0` 的 10 ms 消费拍数 | `StreamMetrics`；解码器 `noteUnderrun()` |
| `deviceWriteFailures` | `m_audioDev->write()` 返回 `< 0` 的次数 | `StreamMetrics`；解码器 `noteDeviceWriteFailure()` |
| `prefillTimeouts` | §2.3 的等待超时次数 | `StreamMetrics` |

只有 0 字节算欠流；`availableRead() > 0` 但不足 4096 B 时取到多少送多少，仍有声音，不计。

### 4.2 豁免清单

`SEEKING` / `m_switching` / `isShuttingDown()` / `task->trackIndex != cur->index` / `EOS` / `ERROR` / `IDLE` / 首播门槛等待期。前四类已有静默窗口；后三类不是欠流；等待期消费者尚未启动。

### 4.3 线程、内存序与生命周期

- 消费端（WorkQueue 定时任务）调用；`std::atomic<size_t>` + `fetch_add(relaxed)`，与 `m_silenceFills` 同款。
- 调用方持有 `PlaybackTaskPtr`（shared_ptr），退役期间仍保活；退役任务不被轮询，计数停在退役时刻。
- 清零随 `AudioStreamDecoder::start()`；暂停/恢复复用同一 decoder 不清零（与既有仪表口径一致）。

### 4.4 与 `silenceFills` 的关系

`silenceFills` 的语义、窗口判据与 `silenceFills == 0` 稳态判据**完全不动**。`underrunTicks` 必须由消费端在调用点判窗口，不能复用 `noteSilenceFill()` 的内部判据——解码器看不到 `m_switching` / `shuttingDown`（#175）。

### 4.5 契约

按 `doc/observability-contract.md` §3.2 与 #104：**只记录、不设任何数值阈值、不作为门禁**。§3.4 触发清单新增第 4 条：`underrunTicks` / `deviceWriteFailures` 的计数语义或豁免窗口变化时，回来重审稳态欠流判据。

`deviceWriteFailures` 只计数；失败后继续写（现状），不重开设备、不上报错误。恢复策略另立 effort。

## 5. 切歌软取消与切换窗口峰值豁免

### 5.1 所有权与生命周期

- 退役槽 `MusicPlayList::m_retiredTask`，与 `m_currentTask` 同受 `m_taskMutex` 保护。
- `replacePlaybackTask()`：`retireCurrentTask()`（移槽 + `requestStop()`）→ 建新 ring / decoder 并 `start()` → 发布新 current。
- 若退役槽已有旧任务：先 `joinRetiredTask()` 再移入新退役任务（单槽，最多一个待回收）。
- 非阻塞回收：消费拍每拍调 `reapRetiredTaskIfFinished()`，**只在 `decoder->isThreadFinished()` 为真时**才 join。旧线程已退出 ⇒ join 立即返回；旧线程还在跑（含卡在网络读）⇒ 这一拍什么都不做，绝不在消费拍上等。
- `stopCurrentTask()`：先 join retired，再 join current；`stop` 是终态，不走退役槽。
- 析构：`~MusicPlayList` join 两槽；`~MusicPlayer::Impl` 仍先 `m_workQueue.shutdown()` 再走 `_releaseAudio()`。

### 5.2 时序

1. 旧 decoder：`requestStop()` 只置 `m_stopRequest` / `m_abortDecode`，**不 join**；
2. 旧 task 移入退役槽（ring / decoder 保活）；
3. 新 decoder `start()`；
4. 旧线程最坏再跑一次 1 ms 背压睡眠 + 当前读阻塞，之后退出；不再写任何被消费的 ring。

新任务用新 ring，旧 ring 由退役 task 独占 ⇒ 旧 PCM 不可能流进新曲。

### 5.3 切换窗口与峰值豁免

- **窗口**：新任务 `start()` 起 → 旧任务 join 完成止。
- **额度**：窗口内允许 `2 × 262144(ring) + 2 × P + 2 × 64KB(AVIO) + O(1)`；窗口外回落到 `262144 + P + 64KB + O(1)`。
- `peakHeldBytes` 不含 ring 且 per-decoder（#175），判据拆两半：
  - 解码器口径：两个 decoder 并存时各自 `peakHeldBytes ≤ P`；
  - ring 口径：窗口内同时存在两个 `PlaybackTask`（`weak_ptr` 观察），每个 ring 固定 262144 B，按公式手算。
- `streaming-playback-spec.md` §5 常驻上界判据既有行一字不动；本条是**过渡态豁免**，不改变稳态上界。

### 5.4 失败路径

- 退役任务的 `ERROR` / `EOS` 不上报：消费端只轮询 current；`handleDecodeError` 只对 current 生效；退役任务不得触发 `onError` / 自动跳轨 / 写 `curPositionMs`。
- 新 decoder `start()` 失败：退役槽里的旧任务保留，下一次 replace / stop 回收；错误路径与现状一致。

### 5.5 不预解下一首（E4 的部分解冻）

只解冻「旧任务延迟退出」这一半；`prev/next` 预缓冲仍只留曲目元数据，**不建下一首的 decoder / ring**。自动跳轨既有的「join 可延后」例外与本次合并为同一条规则：旧任务只 abort 不 join，join 交给下一次控制事件。

### 5.6 已知代价

若旧任务卡在网络读，下一次切歌 / stop 会承担最多 `rw_timeout=3 s` 的 join；不新增后台线程、不 detach。这是用「一次性延迟后移」换「每次切歌都不被 join 顶住」。

## 6. 线程与所有权清单

| 对象 / 接口 | 允许线程 | 说明 |
|---|---|---|
| `ring->write()` / `nextPacket()` / demux | 解码线程 | 唯一生产者；不变 |
| `ring->read()` / 10 ms `consumeAudioData()` / 静音填充 / `noteUnderrun()` | WorkQueue 消费线程 | 唯一消费者；不变 |
| `ring->reset()` | 仅解码线程 | SEEKING / switching 静默窗口内；不变 |
| `requestStop()` | WorkQueue 线程 | 只置位，不 join |
| `stop()`（join） | WorkQueue 线程 | 新任务启动前 / 下一次控制事件 / 退役线程退出后的消费拍 / 析构 |
| 退役槽读写 | WorkQueue 线程 | 与 current 同锁 |
| 设备 `open` / `start` / `stop` / `close` / `clearQueue` | WorkQueue 线程 | 不变 |
| 首播门槛等待 | WorkQueue 控制线程 | 设备 open 后、start 前；≤300 ms |

## 7. 判据

| 判据 | 量测 / 注入 | 失败条件 | 落点 |
|---|---|---|---|
| 首播门槛生效 | `PrefillGate` 单测：写线程延迟 20 ms 写入 ≥ target | 返回 `true` 时 `availableRead() < target` | `sdk_unit_tests`（新 suite） |
| 门槛超时降级 | 写线程不写；断言 300 ms ±100 ms 返回 `false` | 挂死、或未 `start`、或 `prefillTimeouts != 1` | `sdk_playback_lifecycle_tests` |
| 欠流计数 | 慢提取器 + 快消费 | `underrunTicks == 0`（应 >0） | `sdk_streaming_budget_tests` |
| 静默窗口不计数 | SEEKING 窗口内 | `underrunTicks` 增加 | `sdk_streaming_budget_tests` |
| 切换窗口峰值 | `weak_ptr` 观察两 task 并存 | 窗口外旧 task 未过期 / 峰值未回落 | `sdk_playback_lifecycle_tests` |
| EOS 不净空 | 真实素材播到 EOS | `getQueuedBytes()` 瞬间归零 | `sdk_player_integration` |

**预算**：不新增 ctest 目标（保持 14 个），不放宽任何 `TIMEOUT`，BadInput 55 例 / 398 断言基线不动；新增用例 ≤6 条，等待类断言一律「事件 + 上限」。

### 7.1 覆盖状态（`2b4a26c82` 落地后核对）

| 判据 | 自动化落点 | 状态 |
|---|---|---|
| 首播门槛生效 | `PrefillGate`「reaches the target once the writer fills the ring」 | 已覆盖 |
| 门槛超时降级 | `PrefillGate`「times out when the writer never fills the ring」（短预算、断言返回 `false` 与实耗区间）；`DecoderInstrumentation`「underrun, write-failure and prefill counters are independent and reset on start」覆盖 `prefillTimeouts` 的记账与随 `start()` 清零 | 记账侧已覆盖；「`_play()` 在 `TimedOut` 时调 `notePrefillTimeout()`」这条接线无自动化入口 |
| 欠流计数 / 静默窗口不计数 | — | **未自动化**：`underrunTicks` 只由 `MusicPlayer::Impl::consumeAudioData()` 写，而 sdk 公开头没有 per-decoder 仪表读数（#175），`StreamingBudget` 的 decoder 级夹具够不到消费拍 |
| 切换窗口峰值 | `PlaybackLifecycle`「switching tracks retires the previous task and reaps it once its thread exits」 | 已覆盖「退役后保活 → 线程退出后消费拍回收」两端；窗口内外的字节峰值仍按 §5.3 公式手算 |
| EOS 不净空 | — | **未自动化**：需要真实素材播到 EOS 且能读到设备队列，`MusicPlayer` 级夹具未暴露 `getQueuedBytes()` |

两条未覆盖项的缺口不止「读数」一项：

- **读数**：`MusicPlayer` 没有对外暴露 per-decoder 仪表与设备队列读数。仓库里已有 `AudioDevice::setForceOpenFailureForTest()` 这类测试接缝先例，补读数不算破例，但仍是公开头新增接口。
- **场景注入**（真正的卡点）：`DecoderInstrumentation` 只能直接调 `noteXxx()`，等于自证；要造「300 ms 门槛超时」需要慢供给替身，要造「EOS 时设备队列仍有尾音」需要能控住队列的设备替身。#175 已确认 `DecodeSupplyObserver` 没有注入口，慢供给只能靠 `packetAt()` 替换，两者都是新接缝。

因此本轮**只登记不实现**：`underrunTicks` / `deviceWriteFailures` 的记账侧由 `DecoderInstrumentation` 覆盖，窗口判据在 `consumeAudioData()` 里由 `shouldFillSilence()` 的既有分支保护，落在代码评审可见范围。要闭这两条判据，得先立一个「可控供给替身 / 设备队列替身」的 effort，不在本规格的目的地内。

### 7.1.1 时点注记（2026-11-17，只新增、不改上表任何单元格）

上表的「未自动化」记录的是 `2b4a26c82` 时点的事实，已过期但不修改：改格会切断「哪一版规格对应哪一版代码」的线索。以下按时间顺序追加。

三条判据已可自动化，接缝与判别式的决策记录在 [ADR 0010](../../docs/adr/0010-test-injection-seams-and-criteria.md)，来源是[地图：三条待验证播放行为判据的可测试化](https://github.com/wt159/SoundBridge/issues/184)。落点名（套件 / 用例 / 文件，不写行号）：

| 判据 | 套件 | 用例 | 文件 |
|---|---|---|---|
| 欠流计数 / 静默窗口不计数 | `MusicPlayer Interactive Paths` | `steady-state underrun is counted` | `sdk/test/unit/test_player_integration.cpp` |
| EOS 不净空 | `MusicPlayer Interactive Paths` | `EOS keeps the device queue` | 同上 |
| 切换失败不净空 | `MusicPlayer Interactive Paths` | `switch failure keeps the device queue` | 同上 |
| 切换成功净空（对照） | `MusicPlayer Interactive Paths` | `successful switch clears the device queue` | 同上 |

四条用例经 `SB_TEST_INTERACTION_SUITES` 进 `sdk_interaction_tests`，不新增 ctest 目标、不动任何 `TIMEOUT`。

两处口径更正，原句保留、仅在此记明实际语义：

- `silenceFills` 只覆盖 SEEKING 窗口。上文第 209 行提到的「三类窗口」是记账侧的意图描述，不是计数器的实际覆盖范围——解码器看不到 `m_switching` / `shuttingDown`（`sdk/audio/decode/AudioStreamDecoder.cpp:346`）。
- 队列 256 KB 上限来自应用侧 `kQueueThreshold`（`sdk/MusicPlayer.cpp:648`），SDL 侧不设限。

一条范围声明：以上判据在 CI 的 dummy 驱动下成立（两个工作流都设 `SDL_AUDIODRIVER=dummy`），**真实设备行为仍只由人工冒烟覆盖**——§11 的「可测不等于已自动化」原句继续有效。

## 8. 不可推翻前提核对

| 前提（来源） | 本规格处置 |
|---|---|
| 10 ms 定时 push + 每拍 4096 B（#42 / 规格 §3） | **一字不动**；门槛只在其前面加一次准入 |
| ring 无 mutex、SPSC（规格 §4.4） | **一字不动**；不加锁、不加条件变量 |
| `positionMs` 口径（#55 / 规格 §3） | **一字不动**；净空只缩小领先量 |
| 三态、错误不进状态面、七个控制入口 `void`（#75 / ADR 0004） | **一字不动** |
| 不新增错误码 / 公开能力位 / 回调（#75 / #104） | **一字不动**；新增只有内部仪表 |
| `silenceFills` 稳态判据（#48 / #104） | **一字不动**；新增独立计数 |
| 常驻上界（#48 §5） | 稳态**一字不动**；新增切换窗口过渡态豁免 |
| E4「prev/next 只留元数据」（规格 §4.3） | **部分解冻**：只允许旧任务延迟退出，不预解下一首 |
| 自动跳轨「join 可延后」（规格 §4.2） | 与软取消合并为同一条规则 |
| 14 个 `sdk_*` / `TIMEOUT` / BadInput 基线 | **一字不动** |

## 9. 文档接缝与更正

| 位置 | 处置 |
|---|---|
| `doc/streaming-playback-spec.md` §3（首播阈值 4096 B 行） | 表下纯新增时点注记：阈值改为时间口径 150 ms + 4096 B 下限，指向本规格 §2 |
| `doc/streaming-playback-spec.md` §4.3（E4 行） | 表后纯新增时点注记：E4 部分解冻，只允许退役不预解，指向本规格 §5 |
| `doc/streaming-playback-spec.md` §5（欠流判据） | 表后纯新增时点注记：新增 `underrunTicks` 观测，`silenceFills` 判据不动，指向本规格 §4 |
| `doc/live-source-spec.md` 末尾 | 纯新增时点注记：门槛只作用于冷启动，重连不重复等待 |
| `doc/observability-contract.md` §3.4 | 触发清单新增第 4 条 |
| `GLOSSARY.md` | 新增「首播门槛」「欠流」两条，不含实现细节 |
| `StreamMetrics.h:20-23` | 既有误述只登记不修（#175），不属本规格既有行 |

## 10. 分阶段落地与图外执行票链

本图只产规格与决策，代码执行交图外 effort。建议两阶段：

| 阶段 | 内容 | 风险 |
|---|---|---|
| P1 | 首播门槛、净空出口、欠流与写失败计数、`PrefillGate` 与三组判据 | 低：不改任务所有权 |
| P2 | 切歌软取消、退役槽、切换窗口峰值豁免 | 中：改任务生命周期与线程时序，须整轮 `ctest -R sdk_ -j4` + 双端 CI |

P2 落地前必须完成 P1 的可观测性，否则无法判断软取消是否把欠流推高。

## 11. 验收矩阵

| 契约 | 判据 | 落点 |
|---|---|---|
| 首播门槛可判定 | 门槛生效 + 超时降级两条用例可红可绿 | §7 前两行 |
| 净空可判定 | seek / 切歌后 `getQueuedBytes()==0`，EOS 单调下降 | §3 / §7 |
| 欠流可观测 | `underrunTicks` 可读、可复现、静默窗口不计数 | §4 / §7 |
| 软取消可判定 | 旧 task 延迟存活、下一次控制事件回收、失败不上报 | §5 / §7 |
| 峰值可判定 | 窗口内 ≤2×ring、窗口外回落 | §5.3 / §7 |
| 边界守恒 | 10 ms push、`positionMs`、三态、公开面、14 目标、BadInput 基线 | §8 |

**落点口径**：本表的「判据」是可测的契约表述，不等于都已有自动化用例。截至 `300a1d5c5`，有自动化落点的只有首播门槛两条（`PrefillGate` + `DecoderInstrumentation` 记账侧）与退役槽两端（`PlaybackLifecycle`）；欠流计数 / 静默窗口、EOS 不净空、窗口内外字节峰值、切换失败不净空四项仍需读设备队列或可控供给替身，状态与缺口见 §7.1。

## 12. 明确否决的替代方案

- **事件 / 水位驱动消费**：省 5 ms 量级延迟，但要重开 `silenceFills` 判据与观测契约触发清单，收益不抵成本。
- **EOS 净空**：会截断最多 1.49 s 尾音，违反 B4。
- **prev/next 预解下一首**：把跨曲目预缓冲整块拉回来，内存与所有权复杂度不抵收益。
- **`positionMs` 听感补偿**：改公开契约与全部位置判据。
- **为欠流设数值门禁**：CI 的消费者不按实时墙钟节流，真实欠流不稳定；按 #104 只记录。
