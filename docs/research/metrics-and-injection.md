# #175 仪表通道与测试注入面：事实核查

> 事实票（`research` / AFK）。只读取证：不改任何源码 / 测试 / 构建 / issue，只新增本文件。
> 行号基于 `master` @ `be5d85365`（2026-11-16），全部为本机源码逐行核对结果；查不到的条目显式写「未找到」。
> **工作树注记**：取证期间 `sdk/audio/decode/AudioStreamDecoder.cpp` 出现另一处 #174 的未提交 `TEMP-DIAG` 改动（`+48` 行，`[PREFILL]` / `[FIRSTPCM]` printf，插入在文件头与 `writeDecodedBytes` 内）。本票未改该文件、也未还原它；本文中该文件的行号一律以 **HEAD 版本**为准（工作树版本对应行号偏移 +48）。
> 方法：先 `Grep` 全量引用点（限定 `sdk/`，排除 `3rdparty`），再逐文件 `Read` 取逐字片段；文档结论只用于交叉验证，与源码冲突时以源码为准。
> 术语沿用 `GLOSSARY.md`；判据口径沿用 `doc/streaming-playback-spec.md:101-114`（§5）。

---

## 1. 结论摘要

1. **仪表是「每 decoder 实例」口径，不是「每 play()」口径。** `AudioStreamDecoder::start()` 清零 6 个仪表原子量（`AudioStreamDecoder.cpp:114-125`，另清零 `m_packetHeldBytes` / `m_reconnectAttempts` / `m_firstPcmNoted`），新任务 = 新 `AudioStreamDecoder`（`MusicPlayList.cpp:474-480`），所以跨任务确实重置；但**暂停/恢复复用同一个 decoder**（`MusicPlayList.cpp:427-432` + `MusicPlayer.cpp:352-354`），恢复时不清零。另有两个字段根本不属于「本次播放任务」：`initScanUs` 是提取器构造期常量（`ExtractorFactory.cpp:192`），`packetBoundaryMicros` 读提取器 reader 的累计值、跨任务累加（`AudioStreamDecoder.cpp:280`）。
2. **`noteSilenceFill()` 的窗口判据只看解码器自己看得见的三件事**（`SEEKING` / `m_stopRequest` / `m_abortDecode`，`AudioStreamDecoder.cpp:318-321`）。生产端 `shouldFillSilence()` 的三类窗口里，**只有 SEEKING 能真正计数**：`m_switching` 与 `isShuttingDown()` 解码器看不到（`MusicPlayer.cpp:484-486` vs `:318-321`），所以计数在这里被静默丢弃。因此「欠流计数」不能建立在这个计数器上，这与 `StreamMetrics.h:20-23` 自己声明的「只覆盖 shuttingDown / switching / SEEKING 三类窗口」也不一致（见 §2.1(e)）。
3. **`DecodeSupplyObserver` 没有注入口。** 它是纯虚观测接口（`DecodeSupplyObserver.h:13-22`），由 `AudioStreamDecoder` 自己继承并回调自己（`AudioStreamDecoder.h:33-35`；`AudioStreamDecoder.cpp:1122-1125`、`:753-756`；FLAC 分支经 `FLACDecode` 的同一指针上报，`FLACDecode.cpp:249-253`），唯一的「可替换点」是 `FLACDecode::initFromPacketFactory(..., observer)` 的参数（`FLACDecode.h:41`），而唯一调用方传 `this`（`AudioStreamDecoder.cpp:566`）。它只决定「谁收事件」，不决定「产出节奏」，**无法注入「慢供给」**。现有制造时序差异的手段只有两类：消费端节流（`kPollMs=5` / `kChunkBytes=4096`，`test_streaming_budget.cpp:52-53`）与自写慢提取器——`packetAt()` 是虚函数且在解码线程上被逐包调用（`ExtractorHelper.hpp:88-95`、`260-285`；`AudioStreamDecoder.cpp:719`、`:1074`、`FLACDecode.cpp:242`），测试替身可在此 sleep；仓库现有 11 个 `ExtractorHelper` 子类替身可作模板（§2.3(c) 列 7 个代表）。
4. **`peakHeldBytes` 明确不含 ring、也不含 AVIO，且是 per-decoder 原子量。** `refreshHeldPeak()` 只累加「解码侧手上的包 + 提取器 `heldPayloadBytes()` + 解码器两块固定缓冲」（`AudioStreamDecoder.cpp:301-310`），`StreamMetrics.h:17-18` 把 ring（262144 B）与 AVIO（64 KB）写成判据公式的独立项。当前切歌路径是「先 `stopCurrentTask()`（join + 丢 ring），再建新 ring」（`MusicPlayList.cpp:471-480`），**两枚 ring 并存的窗口在今天不存在**；因此「2×ring 并存峰值」既量不到、现在也无对象可量（§2.4）。
5. **控制事件与 10 ms 消费任务确实共用同一个 `WorkQueue` 单线程**（成员 `MusicPlayer.cpp:107`；构造时同一实例传给播放列表 `:141`；消费任务是同队列的重复定时任务 `:652`；控制命令同样 `asyncRunTask` 投递 `:195-198`）。worker 在锁外串行执行 `task->func()`（`WorkQueue.hpp:256`），所以 `_play()` 里的有界等待会**同步阻塞消费**；若等待条件由消费侧产出（如「设备首次 write」），就是必然自锁。仓库里唯一有记录的 ctest 超时先例是 `sdk_unit_tests (Timeout) 180s 挂死`，根因是 `WorkQueue` 的 map/queue 记账分叉（`doc/player-state-contract.md:231`、`AGENTS.md:132`，已由 #142 修复），**不是**「`_play` 阻塞」导致的先例。

---

## 2. 逐问题回答

### 2.1 Q1 — `StreamMetrics` 的清零时机、读取路径、跨任务语义、`noteSilenceFill()` 窗口判据

#### (a) 清零点：在 `AudioStreamDecoder::start()`（6 个仪表原子量 + 3 个内部状态）

```cpp
// sdk/audio/decode/AudioStreamDecoder.cpp:111-125
sdk_utils::status_t AudioStreamDecoder::start(ExtractorHelper *extractor, bool startThread)
{
    stop();
    // 观测点从这个时刻起算（= start() 的入口）。放在 stop() 之后，
    // 免得把上一次任务的 join 时间算进首播延迟。
    m_startClock = std::chrono::steady_clock::now();
    m_peakHeldBytes.store(0);
    m_silenceFills.store(0);
    m_startToFirstPcmUs.store(0);
    m_initScanUs.store(0);
    m_packetHeldBytes = 0;
    m_packetsPulled.store(0);
    m_packetsPulledAtFirstPcm.store(0);
    m_reconnectAttempts.store(0);
    m_firstPcmNoted = false;
```

时钟起点在 `stop()` **之后**（`:113-116`），所以上一次任务的 join 时间不计入首播延迟；`m_initScanUs` 紧接着被提取器常量覆盖：

```cpp
// sdk/audio/decode/AudioStreamDecoder.cpp:143-148
    // 观测点：提取器 init（只读扫描）耗时由工厂量好，这里抄进本任务的仪表。
    m_initScanUs.store(m_extractor->initScanMicros());
    // 分配记账的第一个采样点：init 完成后提取器已经持有多少字节 …
    refreshHeldPeak();
```

`initScanUs` 的唯一写入点是工厂构造提取器时（`sdk/extractor/ExtractorFactory.cpp:192` → `ExtractorHelper.hpp:156`），所以对同一提取器重播同一曲目，这个值逐位相同。

#### (b) 读取路径：`decoder.metrics()`，且**当前生产链零调用**

```cpp
// sdk/audio/decode/AudioStreamDecoder.cpp:270-282
StreamMetrics AudioStreamDecoder::metrics() const
{
    StreamMetrics out;
    out.peakHeldBytes          = m_peakHeldBytes.load();
    out.silenceFills           = m_silenceFills.load();
    out.startToFirstPcmUs      = m_startToFirstPcmUs.load();
    out.initScanUs             = m_initScanUs.load();
    out.packetsPulled          = m_packetsPulled.load();
    out.packetsPulledAtFirstPcm = m_packetsPulledAtFirstPcm.load();
    // 现场重算包边界的 CPU 成本是提取器侧的独立指标（只记录、不设阈值）。
    out.packetBoundaryMicros = m_extractor != nullptr ? m_extractor->packetBoundaryMicros() : 0;
    return out;
}
```

`metrics()` 的全部调用点（全仓，排除 `3rdparty`）：

| 调用点 | 用途 |
|---|---|
| `sdk/test/unit/test_streaming_budget.cpp:152` | `start()` 失败分支取快照 |
| `sdk/test/unit/test_streaming_budget.cpp:208` | 每轮播放结束后取快照 |

`MusicPlayer.*` / `MusicPlayList.*` / `sdk/include/soundbridge/player.h` 中 **`metrics` 零命中**（`grep -n "StreamMetrics\|metrics"` 无输出）。即：**生产层没有任何读出面**，仪表只对「持有 `AudioStreamDecoder*` 的代码」可见。若要给 app / CI 读，需要新增访问器，属新增能力，不在本票范围。

声明本身承诺「可从消费线程读取」：

```cpp
// sdk/audio/decode/AudioStreamDecoder.h:73-76
    // 四个观测点的快照（规格第 5 节 / 票面 #56）。它们是仪表，不是判据：
    // 本票只要求可读、可复现，不在这里对任何数值判红绿。
    // 线程安全：可从消费线程读取，不影响解码线程。
    StreamMetrics metrics() const;
```

这句话只对原子字段成立；`:280` 触到提取器状态，`:301-310` 也触到（`refreshHeldPeak` 里的 `heldPayloadBytes()`），**不是**线程安全的，见 §2.2(d) 与 §4。

#### (c) 是否跨任务重置

- **跨任务（换曲 / stop 后再 play）**：重置。新任务必建新 `AudioStreamDecoder`：

```cpp
// sdk/MusicPlayList.cpp:471-480
    // 顺序不可交换：先 stop+join 旧任务、旧 ring 引用计数归零，再建新任务。
    stopCurrentTask();

    std::shared_ptr<AudioRingBuffer> ring
        = std::make_shared<AudioRingBuffer>(AudioRingBuffer::kDefaultCapacity);
    std::shared_ptr<AudioStreamDecoder> decoder
        = std::make_shared<AudioStreamDecoder>(ring.get(), m_devSpec);
    // start() 内部先 stop/join 自己的旧线程（本实例刚建，无旧线程），再起一个新解码线程。
    // ring 的 reset() 在新解码线程的第一件事里做（规格 4.4：只有解码线程能 reset）。
    sdk_utils::status_t ret = decoder->start(processProperties.extractor.get());
```

- **同任务暂停/恢复**：**不重置**。`_pause()` 只停消费与设备，不碰 decoder：

```cpp
// sdk/MusicPlayer.cpp:352-354
    stopConsumerThread();
    m_audioDev->stop();
    updatePlayState(MusicPlayerState::PausedState);
```

```cpp
// sdk/MusicPlayList.cpp:423-432
    {
        std::lock_guard<std::mutex> lock(m_taskMutex);
        // 暂停后恢复、重复 play：同一个任务还活着就沿用，别把已经缓冲好的 PCM 扔掉。
        // 判据只认 MusicPlayerState 的投影（decoder 状态），不新增状态机。
        if (m_currentTask != nullptr && m_currentTask->trackIndex == current->index
            && m_currentTask->decoder != nullptr
            && m_currentTask->decoder->state() != StreamDecoderState::IDLE
            && m_currentTask->decoder->state() != StreamDecoderState::ERROR) {
            return true;
        }
    }
```

- `stop()` 本身**不清仪表**（只置位、join、解绑、状态归 IDLE）：

```cpp
// sdk/audio/decode/AudioStreamDecoder.cpp:221-236
void AudioStreamDecoder::stop()
{
    m_stopRequest.store(true);
    m_abortDecode.store(true);
    if (m_thread.joinable()) {
        m_thread.join();
    }
    // P1：解码线程已退出，解绑裸指针，避免 decoder 析构后数据源持有悬垂 flag。
    if (m_abortSource != nullptr) {
        m_abortSource->setAbortFlag(nullptr);
        m_abortSource = nullptr;
    }
    m_stopRequest.store(false);
    m_abortDecode.store(false);
    m_state.store(StreamDecoderState::IDLE);
}
```

#### (d) 两个「不是每任务口径」的字段

- `initScanUs`：提取器构造期常量（`ExtractorFactory.cpp:192`、`ExtractorHelper.hpp:156,238`）。重播同曲不变。
- `packetBoundaryMicros`：直接从提取器读（`AudioStreamDecoder.cpp:280`），而提取器 reader 在任务之间不复位（`MKVExtractor.cpp:50-51` 只在 `m_reader == nullptr` 时创建；`MKVPacketReader.cpp:246` 用 `fetch_add` 累计）。所以它是**提取器生命周期累计值**，`StreamMetrics.h:12`「全部字段都是本次播放任务的口径」对这个字段不成立。字段本身是原子量（`MKVPacketReader.h:81`），跨线程读不会撕值。

#### (e) `noteSilenceFill()` 的窗口判据（逐字）

```cpp
// sdk/audio/decode/AudioStreamDecoder.cpp:312-323
void AudioStreamDecoder::noteSilenceFill()
{
    // 只统计**静默窗口内**的静音填充（规格第 5 节 / 票面 #56）。
    // 窗口的判据来自解码器自己看得见的三类信号：SEEKING、已请求停止、已置取消位。
    // 窗口之外的记账一律丢弃——否则计数器会退化成「消费端调了几次接口」，
    // 而判据要的「稳态窗口静音填充为零」就再也判不出来了。
    const StreamDecoderState current = m_state.load();
    if (current != StreamDecoderState::SEEKING && !m_stopRequest.load() && !m_abortDecode.load()) {
        return;
    }
    m_silenceFills.fetch_add(1, std::memory_order_relaxed);
}
```

判据是**三选一或**：`state == SEEKING` **或** `m_stopRequest` **或** `m_abortDecode`。注意 `stop()` 在 `join()` 之后会把这两个标志清回 `false` 并把状态置 `IDLE`（`:233-235`），所以「停止窗口」只在 stop 进行中、且消费者恰好还在跑的那一小段才可能计数。

---

### 2.2 Q2 — 消费端能否安全调用解码器计数器：调用点、线程归属、原子性、指针生命周期

#### (a) 现有调用点（全仓 5 处）

| # | 位置 | 线程 | 说明 |
|---|---|---|---|
| 1 | `sdk/MusicPlayer.cpp:523` | **WorkQueue 线程**（生产唯一调用） | `consumeAudioData()` 静音分支 |
| 2 | `sdk/test/unit/test_streaming_budget.cpp:114` | 测试主线程 | `seekAndCatchSilenceWindow()` 紧轮询命中窗口后补记一次 |
| 3 | `sdk/test/unit/test_streaming_budget.cpp:177` | 测试主线程 | seek 未落地阶段 |
| 4 | `sdk/test/unit/test_streaming_budget.cpp:187` | 测试主线程 | 主循环内静默窗口 |
| 5 | `sdk/test/unit/test_streaming_budget.cpp:191` | 测试主线程 | `probeOutsideWindow` 反例探针 |

生产调用点逐字：

```cpp
// sdk/MusicPlayer.cpp:509-525
    if (shouldFillSilence()) {
        const size_t len = 4096;
        std::vector<char> silence(len, 0);
        m_audioDev->write(silence.data(), len);
        // 观测点（规格第 5 节 / 票面 #56）：静音填充次数记在当前曲目的解码器上。
        // 只记账、不改任何分支；解码器只统计静默窗口内的调用，
        // 所以稳态播放期间这个数恒为 0（判据的口径是「稳态窗口静音填充为零」）。
        MusicPropertiesPtr cur;
        {
            std::lock_guard<std::mutex> lock(m_curMusicMutex);
            cur = m_curMusicProperties;
        }
        PlaybackTaskPtr task = m_musicList->currentTask();
        if (task != nullptr && task->decoder != nullptr) {
            task->decoder->noteSilenceFill();
        }
        return;
    }
```

#### (b) 线程归属：消费端 = WorkQueue 线程，**不是**独立线程

```cpp
// sdk/MusicPlayer.cpp:647-653
void MusicPlayer::Impl::startConsumerThread()
{
    if (m_consumerTaskID != WorkQueue::TaskID_Error) {
        return;
    }
    m_consumerTaskID = m_workQueue.startTimerTask(true, 10, [this]() { this->consumeAudioData(); });
    LOG_INFO(LOG_TAG, "Consumer thread started, taskID=%zu", m_consumerTaskID);
}
```

`startConsumerThread()` 名不副实：它注册的是**同一个 `m_workQueue`** 上的 10 ms 重复任务（`m_workQueue` 是 `Impl` 成员，`:107`；构造 `:128`；与播放列表共享同一实例 `:141`）。因此：

- 生产链里 `noteSilenceFill()` 的读写双方（`m_state` / `m_stopRequest` / `m_abortDecode` 由解码线程与 stop 写，`m_silenceFills` 由 WorkQueue 线程写）跨两条线程；
- `AudioStreamDecoder.h:75` / `:78` 注释里的「消费线程」在本仓库指的是**这条 WorkQueue 线程**，不存在第三条音频消费线程。

#### (c) 原子性与内存序

```cpp
// sdk/audio/decode/AudioStreamDecoder.h:213-223
    // 四个观测点：全部是「本次播放任务」的口径，start() 清零后重新累积。
    // 消费线程会写 silenceFills，所以用原子量；其余只在解码线程写、任意线程读。
    std::atomic<size_t> m_peakHeldBytes;
    std::atomic<size_t> m_silenceFills;
    std::atomic<uint64_t> m_startToFirstPcmUs;
    std::atomic<uint64_t> m_initScanUs;
    // 解码侧此刻手上的包字节数（alloc/free 增量，只在解码线程读写）。
    size_t m_packetHeldBytes;
    // 本任务已拉到的包数，与「首帧那一刻」的快照（并行判据的结构证据）。
    std::atomic<size_t> m_packetsPulled;
    std::atomic<size_t> m_packetsPulledAtFirstPcm;
```

| 字段 | 类型 | 写 | 读 | 内存序 |
|---|---|---|---|---|
| `m_silenceFills` | `atomic<size_t>` | 消费（WorkQueue）线程 | 任意 | 写 `fetch_add(relaxed)`（`:322`）、读 `load()` 默认 seq_cst（`:274`） |
| `m_peakHeldBytes` | `atomic<size_t>` | 解码线程（另 `start()` 在控制线程写 0，此时线程未起） | 任意 | CAS 环 + `relaxed`（`:325-331`）、`load` 默认 seq_cst（`:273`） |
| `m_startToFirstPcmUs` / `m_initScanUs` | `atomic<uint64_t>` | 解码 / 控制线程 | 任意 | 默认 seq_cst |
| `m_packetsPulled*` | `atomic<size_t>` | 解码线程 | 任意 | 默认 seq_cst |
| `m_packetHeldBytes` | **非原子** `size_t` | 解码线程 | 解码线程 | —（`metrics()` 不读它，只经 `m_peakHeldBytes` 间接读） |

结论：`silenceFills` 的计数本身是**安全的**（原子 RMW，`relaxed` 对独立计数器足够；不承担发布其他数据的职责）。但它与 `m_state` 的窗口判定之间存在天然 TOCTOU（先读状态、后自增，窗口可能在两步之间关闭），这是仪表固有的近似，不影响「稳态为 0」的方向性判据。

#### (d) 退役 / 析构后指针是否悬空

任务对象与所有权：

```cpp
// sdk/MusicPlayList.h:87-93
// 一次播放任务：ring 与 decoder 由**当前曲目独占**，跨曲目不复用（规格 #55 第 4.3 节）。
//
// - 创建者：播放列表在做「这次要播哪一首」的决定时造；消费者只借指针，不持有所有权。
// - 销毁时机：新任务 start() **之前** —— stop / 切歌 / 停止播放都把旧任务整个丢掉，
//   旧 ring 的引用计数归零，旧 decoder 析构时 join 掉自己的解码线程。
// - 成员声明顺序决定析构顺序：decoder 声明在 ring 之后，所以先析构 decoder（其析构函数
//   join 掉自己的解码线程），再释放它只持有裸指针的 ring。
```

```cpp
// sdk/MusicPlayList.cpp:411-415
PlaybackTaskPtr MusicPlayList::currentTask() const
{
    std::lock_guard<std::mutex> lock(m_taskMutex);
    return m_currentTask;
}
```

```cpp
// sdk/MusicPlayList.cpp:437-453
void MusicPlayList::stopCurrentTask()
{
    PlaybackTaskPtr task;
    {
        std::lock_guard<std::mutex> lock(m_taskMutex);
        task.swap(m_currentTask);
    }
    if (task == nullptr) {
        return;
    }
    // 在这里 join：发起任务的控制线程自己收尾，禁止 detach / 后台 join。
    // 已写进 ring 的 PCM 不 drain——任务整体销毁，旧数据随 ring 一起消失。
    if (task->decoder != nullptr) {
        task->decoder->stop();
    }
    task->decoder.reset();
    task->ring.reset();
}
```

再结合消费端**在同一个作用域内先取快照、再用**：

```cpp
// sdk/MusicPlayer.cpp:533-538
    PlaybackTaskPtr task = m_musicList->currentTask();
    if (cur == nullptr || task == nullptr || task->ring == nullptr || task->decoder == nullptr) {
        return;
    }

    AudioRingBuffer *ring = task->ring.get();
```

判定：

- **当前设计下不会悬空**。三条独立保护同时成立：① `currentTask()` 返回 `shared_ptr` 快照，持有着就有所有权（`MusicPlayList.cpp:411-415`）；② 消费与全部控制/停机路径跑在**同一条 WorkQueue 线程**（§2.2(b)、`MusicPlayer.cpp:652`、`:195-198`），不可能与 `stopCurrentTask()` 并发；③ `~Impl` 先 `m_workQueue.shutdown()`（`MusicPlayer.cpp:158`）再 `_releaseAudio()`，把「队列还在跑 `_stop`」的窗口关掉（`:148-160`）。
- **但有两处结构性隐患**（当前不可达、条件一变就可达）：
  1. `stopCurrentTask()` **就地改写任务对象的成员**（`task->decoder.reset()`、`task->ring.reset()`，`:452-453`），而不是丢弃对象。任何**别的线程**持有同一 `PlaybackTaskPtr` 时，会看到 `decoder` 突然变 `nullptr`；`MusicPlayer.cpp:522` 的 `task->decoder != nullptr` 检查与 `:523` 的调用之间是 TOCTOU —— 只因为同线程串行才安全。
  2. `task->decoder.reset()` 之后对象并不立即销毁：其他快照仍持有引用。所以「退役后指针悬空」在当前代码里不会发生，但「退役后指针变成 nullptr」会发生（同线程即时可见）。若将来把仪表抽样搬到独立线程（例如 gRPC/监控泵），必须先改这条就地改写语义。

---

### 2.3 Q3 — `DecodeSupplyObserver` 的能力边界；不能注入时，现有测试怎么造时序差异

#### (a) 它是纯观测出口，不是注入口

```cpp
// sdk/audio/decode/DecodeSupplyObserver.h:13-22
class DecodeSupplyObserver {
public:
    virtual ~DecodeSupplyObserver() { }

    // 解码侧拿走一包（已计入常驻）。在包被丢弃时必须配对调用 onPacketReleased。
    virtual void onPacketAcquired(size_t bytes) = 0;
    virtual void onPacketReleased(size_t bytes) = 0;
    // 本任务累计已从包工厂拉到的包数（含被跳过的空包之外的所有产出）。
    virtual void onPacketPulled(size_t totalPulled) = 0;
};
```

解码器**自己就是**这个接口的实现者，并且只回调自己：

```cpp
// sdk/audio/decode/AudioStreamDecoder.h:33-35
class AudioStreamDecoder : public NonCopyable,
                            public AudioDecodeCallback,
                            public DecodeSupplyObserver {
```

```cpp
// sdk/audio/decode/AudioStreamDecoder.cpp:1121-1125
        // 分配记账挂在 alloc/free 事件上（规格第 5 节）：同一时刻只为一首曲目持一包。
        onPacketAcquired(packet->size());
        onPacketPulled(cursor->packetsProduced());
        const int ret = decode->decodePacket(packet);
        onPacketReleased(packet->size());
```

（另一条逐包分支 `wav/aiff` 同形：`AudioStreamDecoder.cpp:752-757`。）

**能力边界（核实结论）**：
- **没有注册 / 设置口**：`DecodeSupplyObserver` 只出现在 4 个文件——声明（`DecodeSupplyObserver.h`）、`AudioStreamDecoder` 的基类列表（`AudioStreamDecoder.h:33-35`）、`FLACDecode` 的成员与参数（`FLACDecode.h:27,41`；`FLACDecode.cpp:88`）。
- **唯一的「可替换点」是一个参数，而不是供给控制面**：`FLACDecode::initFromPacketFactory(ExtractorHelper *extractor, DecodeSupplyObserver *observer)`（`FLACDecode.h:41`），全仓唯一调用方传的是 `this`：

```cpp
// sdk/audio/decode/AudioStreamDecoder.cpp:557-569（节选）
    if (m_codecID == AUDIO_CODEC_ID_FLAC) {
        FLACDecode flac(this);
        ...
        if (m_extractor->packetCount() != 0) {
            if (!flac.initFromPacketFactory(m_extractor, this)) {
```

  换掉这个指针只改变「谁收到事件」，不改变「何时 / 产出多少」——FLAC 分支的实际上报点也在 `FLACDecode` 里：

```cpp
// sdk/audio/decode/FLACDecode.cpp:249-253（节选）
        if (m_observer != nullptr) {
            m_observer->onPacketAcquired(m_packet->size());
            m_observer->onPacketPulled(m_cursor->packetsProduced());
        }
```

  （配对释放：`FLACDecode.cpp:109-112`，`releasePacket()` 里调 `onPacketReleased`。）
- 三个回调只上报「字节数 / 包数」，不含时间、不含 ring 水位、不含背压状态；无法表达「现在慢一点」。
- 因此：**不能用于注入慢供给**，也不能用它观测消费端（它是生产者侧仪表）；「替换 observer」这条路只能改记账落点，不产生时序差异。

#### (b) 现有测试制造时序差异的两条既有手段

**(1) 消费端节流（改变消费速率，从而改变 ring 背压）**

```cpp
// sdk/test/unit/test_streaming_budget.cpp:48-53
        // 消费端每次最多取走的字节数与轮询间隔。
        // 与生产链的 10 ms / 4096 B 节流不同：判据套件要把整首曲子跑完，
        // 按生产速率消费一首 259 s 素材需要 110 s 以上（规格第 5 节已注明
        // 「稳态欠流判据假定测试环境消费端不按实时墙钟节流」）。
        const size_t kChunkBytes = 4096;
        const int kPollMs        = 5;
```

```cpp
// sdk/test/unit/test_streaming_budget.cpp:70-86
        // 把 ring 里可读的 PCM 全部取走；counter 非空时累加取走的字节数。
        size_t drainRing(AudioRingBuffer &ring, std::vector<char> &buf, size_t *counter)
        {
            size_t total = 0;
            while (ring.availableRead() > 0) {
                size_t once = std::min(buf.size(), ring.availableRead());
                size_t got  = ring.read(buf.data(), once);
                if (got == 0) {
                    break;
                }
                total += got;
            }
            if (counter != nullptr) {
                *counter += total;
            }
            return total;
        }
```

消费主循环：

```cpp
// sdk/test/unit/test_streaming_budget.cpp:167-206（节选）
            while (elapsed <= timeoutMs) {
                ...
                } else if (isSilenceWindow(decoder)) {
                    // 静默窗口里消费端交付静音：交给解码器记账（与生产同一个计数入口）。
                    decoder.noteSilenceFill();
                } else {
                    ...
                    observation.bytes += drainRing(ring, buf, afterSeek);
                }

                StreamDecoderState state = decoder.state();
                if (state == StreamDecoderState::ERROR || state == StreamDecoderState::EOS) {
                    observation.endState = state;
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(kPollMs));
                elapsed += kPollMs;
            }
```

配套的「短窗口紧轮询」手法（12 ms SEEKING 窗口 + 5 ms 睡眠粒度不足的正解）：

```cpp
// sdk/test/unit/test_streaming_budget.cpp:96-102
        // 为什么要紧轮询：SEEKING 窗口的长度是固定的——解码线程进入 SEEKING 后会
        // sleep 12 ms（AudioStreamDecoder.cpp 的 seek 分支），窗口就这么长。而主循环
        // 是 `sleep_for(5ms)` 一次采样，Windows 上这个睡眠可能落到 15.6 ms 的计时器粒度，
        // 于是 12 ms 的窗口可以整个落在两次采样之间，「窗口内计数」就变成了依赖机器
        // 定时器行为的断言（CI 上实测漏过一次：silenceFills == 0，而 seek 本身是成功的）。
```

**(2) 通用「等状态 + 边排空」辅助（解码/seek 用例）**

```cpp
// sdk/test/unit/test_decode.cpp:224-249
bool waitForDecoderStateWithDrain(AudioStreamDecoder &decoder, AudioRingBuffer &ring,
                                  StreamDecoderState expectedState, int timeoutMs)
{
    const int pollMs = 5;
    int elapsed      = 0;
    std::vector<char> drain(4096);
    while (elapsed <= timeoutMs) {
        while (ring.availableRead() > 0) {
            const size_t once = std::min(drain.size(), ring.availableRead());
            if (ring.read(drain.data(), once) == 0) {
                break;
            }
        }

        const StreamDecoderState state = decoder.state();
        if (state == expectedState) {
            return true;
        }
        if (state == StreamDecoderState::ERROR) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(pollMs));
        elapsed += pollMs;
    }
    return false;
}
```

同类辅助还有 `test_playback_lifecycle.cpp:82-104`（`waitUntil` 1 ms 粒度、`drainBytes` 2 ms 粒度 + 5000 ms 上限），并在注释里记录过同类 12 ms 窗口的坑（`:174-177`）。

**这两条改的都是「消费速率」，不是「供给速率」**：`kPollMs` / `kChunkBytes` 只改变 ring 被抽干的速度，从而间接改变解码线程在 `writeDecodedBytes` 里撞背压的次数（`AudioStreamDecoder.cpp:950-967`）；`waitForDecoderStateWithDrain` 更只是「边排空边等状态」。想造「供给跟不上」的时序差异，必须换供给侧（下一个子节）。

#### (c) 真正可用的「慢供给」注入点（现状可做，无需改生产代码）

包工厂的虚边界在解码线程上被逐包调用：

```cpp
// sdk/extractor/ExtractorHelper.hpp:88-95
    virtual size_t packetCount() { return getMetaData() == nullptr ? 0 : 1; }
    virtual AudioBuffer::AudioBufferPtr packetAt(size_t index)
    {
        if (index != 0) {
            return nullptr;
        }
        return getMetaData();
    }
```

```cpp
// sdk/extractor/ExtractorHelper.hpp:267-283（PacketCursor::nextPacket 节选）
        const size_t count    = m_extractor->packetCount();
        const bool countKnown = count != ExtractorHelper::kUnknownPacketCount;
        while (!countKnown || m_nextIndex < count) {
            AudioBuffer::AudioBufferPtr current = m_extractor->packetAt(m_nextIndex);
            ...
```

调用点（全部在解码线程内，三条逐包分支）：

```cpp
// sdk/audio/decode/AudioStreamDecoder.cpp:719（wav/aiff 原始 PCM 逐包）
            if (!cursor.nextPacket(packet)) {
// sdk/audio/decode/AudioStreamDecoder.cpp:1074（ffmpeg 逐包：mp3/aac/mkv/m4a/ape/asf…）
        if (!cursor->nextPacket(packet)) {
// sdk/audio/decode/FLACDecode.cpp:241-242（FLAC native，游标在 FLACDecode 里）
        AudioBuffer::AudioBufferPtr next;
        if (!m_cursor->nextPacket(next)) {
```

仓库现有 11 个 `ExtractorHelper` 子类测试替身（都靠 override 这几个虚函数实现，无需碰生产代码）；下表列 7 个代表，另有 `TestOnlyExtractor`（`test_extractor.cpp:355`）、`PcmExtractor`（`test_playback_position.cpp:52`）、`MockExtractor`（`test_stream_decoder.cpp:90`）、`ContractExtractor`（`test_stream_decoder_contract.cpp:24`）四个：

| 替身 | 位置 | 形态 |
|---|---|---|
| `WholeViewExtractor` | `test_streaming_budget.cpp:429-482` | 整首一包（反例） |
| `PacketTableExtractor` / `SingleBlockExtractor` | `test_packet_factory.cpp:59` / `:97` | 包表 / 整块 |
| `FixedSpecProbeExtractor` / `LyingSpecExtractor` | `test_bad_input.cpp:327` / `:729` | 规格伪装 |
| `PatternPcmExtractor` | `test_playback_lifecycle.cpp:57` | 合成 PCM |
| `TestProcessExtractor` | `test_decode.cpp:142` | 整段处理面 |

即：**「慢供给」今天可以用一个 `packetAt()` 里 `sleep_for` 的测试提取器实现**，代价是回调发生在解码线程上（与生产同构，不影响判据主循环）。副作用：「慢」会被 `writeDecodedBytes` 的 ring 背压吞掉（`AudioStreamDecoder.cpp:950-967`），要观察到差异需同时控制消费速率。

#### (d) 补充：无法用「小 ring」制造差异

`playWithExtractor()` 把 ring 容量写死为默认值：

```cpp
// sdk/test/unit/test_streaming_budget.cpp:148-150
            AudioRingBuffer ring(AudioRingBuffer::kDefaultCapacity);
            AudioStreamDecoder decoder(&ring, devSpec);
            if (decoder.start(extractor) != sdk_utils::OK) {
```

要按用例缩小 ring（256 KB → 更小，逼出背压/欠流）需要改这个辅助函数的签名或新增一个变体（只给方案，见 §3）。

---

### 2.4 Q4 — 分配记账（`peakHeldBytes`）的实现位置与口径

#### (a) 实现位置与公式

```cpp
// sdk/audio/decode/AudioStreamDecoder.cpp:284-310
void AudioStreamDecoder::onPacketAcquired(size_t bytes)
{
    m_packetHeldBytes += bytes;
    refreshHeldPeak();
}

void AudioStreamDecoder::onPacketReleased(size_t bytes)
{
    m_packetHeldBytes = m_packetHeldBytes > bytes ? m_packetHeldBytes - bytes : 0;
    refreshHeldPeak();
}

void AudioStreamDecoder::onPacketPulled(size_t totalPulled)
{
    m_packetsPulled.store(totalPulled);
}

void AudioStreamDecoder::refreshHeldPeak()
{
    size_t held = m_packetHeldBytes;
    if (m_extractor != nullptr) {
        held += m_extractor->heldPayloadBytes();
    }
    // 解码器自己的两块固定缓冲：只随单帧大小变化，不随素材时长增长（判据里的 O(1) 项）。
    held += m_interleaveBuf.capacity() + m_resampleBuf.capacity();
    noteHeldBytes(held);
}
```

峰值写入是 CAS 环：

```cpp
// sdk/audio/decode/AudioStreamDecoder.cpp:325-331
void AudioStreamDecoder::noteHeldBytes(size_t bytes)
{
    size_t peak = m_peakHeldBytes.load(std::memory_order_relaxed);
    while (bytes > peak
           && !m_peakHeldBytes.compare_exchange_weak(peak, bytes, std::memory_order_relaxed)) {
    }
}
```

采样点（共 5 类）：`start()` 首采 `:148`；每次 `onPacketAcquired` / `onPacketReleased` `:287`/`:293`；首帧写入成功 `:979`；EOS 汇总 `:336`。

#### (b) ring 是否被排除：**是**（明示）

```cpp
// sdk/audio/decode/StreamMetrics.h:14-19
    // 1) 分配记账峰值：本次播放为当前曲目持有的音频数据字节数。
    //    必须覆盖**一切随包数增长的结构**（payload、包索引、页表、seek 表），
    //    只算 payload 会让常驻上界判据形同虚设（规格第 3 节 / ADR 0003）。
    //    不含量测对象之外的固定开销：ring（262144 B）与 ffmpeg AVIO 缓冲（64 KB）
    //    在判据公式里是独立项，不混进这个数。
    size_t peakHeldBytes = 0;
```

代码侧也无 ring 项（`refreshHeldPeak` 全文见上）。测试侧把 ring 加在**不等式右边**，进一步确认口径：

```cpp
// sdk/test/unit/test_streaming_budget.cpp:504-508
        const size_t packetBudget = profile.maxPacketBytes * 2;
        const size_t upper        = AudioRingBuffer::kDefaultCapacity + packetBudget
            + kAvioHeadroomBytes;
        CHECK_MESSAGE(observation.metrics.peakHeldBytes <= upper,
                      "常驻峰值 " << observation.metrics.peakHeldBytes << " 超过上界 " << upper);
```

#### (c) 能否在同一套记账里量到「2×ring 并存」窗口峰值：**不能**

三条独立理由，任一条即足够：

1. **口径排除**：`peakHeldBytes` 不含 ring（上引 `StreamMetrics.h:17-18`）。
2. **作用域是 per-decoder**：字段是 `AudioStreamDecoder` 的成员原子量（`AudioStreamDecoder.h:215`），没有全局/进程级累加；两枚 ring 若分属两个 decoder，各自峰值互不相见。
3. **当前窗口不存在**：切歌是「先 stop+join 旧任务（连同 `ring.reset()` 与引用计数归零），再 `make_shared<AudioRingBuffer>`」（`MusicPlayList.cpp:471-480`；`stopCurrentTask` 见 §2.2(d)）。任何时刻最多一枚 ring 存活。

`AudioRingBuffer` 自身也**没有任何全局/静态记账**：

```cpp
// sdk/utils/AudioRingBuffer.h:37-43
private:
    const size_t m_capacity;
    const size_t m_mask;
    std::unique_ptr<char[]> m_buf;
    std::atomic<uint64_t> m_writePos;
    std::atomic<uint64_t> m_readPos;
```

现有「切换窗口」仪表只有一个**时间**指标，没有字节：

```cpp
// sdk/MusicPlayList.cpp:492-498
    const auto switchEnd = std::chrono::steady_clock::now();
    const auto switchMs  = std::chrono::duration_cast<std::chrono::milliseconds>(
                              switchEnd - switchStart)
                              .count();
    Metrics::RecordTiming("track_switch_ms", static_cast<uint64_t>(switchMs),
                          MetricTags { currentTraceId(), musicProperties->fileProperties.fullPath,
                                       static_cast<int>(musicProperties->index) });
    return true;
```

---

### 2.5 Q5 — `AudioRingBuffer` / `AudioDevice` 可供测试观察的只读接口及线程安全前提

#### (a) ring 的读接口

```cpp
// sdk/utils/AudioRingBuffer.h:23-35
    size_t write(const char *data, size_t len);
    size_t availableWrite() const;

    size_t read(char *data, size_t len);
    size_t availableRead() const;

    // 本段内已被消费端取走的字节数（从 reset() 起算）。
    // 播放位置 = 消费端段基准 + 本值；段基准不在 ring 里，因为 reset() 会把它归零。
    uint64_t readPos() const { return m_readPos.load(std::memory_order_acquire); }

    void reset();

    size_t capacity() const { return m_capacity; }
```

```cpp
// sdk/utils/AudioRingBuffer.cpp:62-68
size_t AudioRingBuffer::availableRead() const
{
    // Consumer reads m_writePos to know how much data is available.
    uint64_t w = m_writePos.load(std::memory_order_acquire);
    uint64_t r = m_readPos.load(std::memory_order_relaxed);
    return static_cast<size_t>(w - r);
}
```

线程安全前提（文件头声明，且与 §2.2 的实际线程一致）：

```cpp
// sdk/utils/AudioRingBuffer.h:7-15
// PCM 用的无锁 SPSC ring（规格 #55 第 4.4 节线程约束）：
//   write()  -> 解码线程，唯一生产者
//   read()   -> 消费线程（10 ms push），唯一消费者
//   reset()  -> **仅解码线程**发起，且只在 SEEKING / 切歌的静默窗口里做——此刻消费者
//               正在交付静音、不读 ring。与段基准更新同一窗口完成，先 applySegmentBase()
//               再 reset()，否则位置会先掉回 0 再跳上去。
// ring 本身不加 mutex（SPSC 靠读写指针的 acquire / release 排序），也不持有段基准：
// 段基准存在解码器里，因为 reset() 会把 m_readPos 归零。
```

**关键限制（对「第三方观察者」而言）**：
- `availableRead()` / `readPos()` 都是单字原子读（`capacity()` 是不可变常量），**从任意线程读不会崩、不会撕值**，但只是**快照**，且只保证与「一生产者 + 一消费者」的相对顺序。
- `reset()` 是「先 store writePos，再 store readPos，再 memset」，其中两次 store 均为 `relaxed`：

```cpp
// sdk/utils/AudioRingBuffer.cpp:92-97
void AudioRingBuffer::reset()
{
    m_writePos.store(0, std::memory_order_relaxed);
    m_readPos.store(0, std::memory_order_relaxed);
    memset(m_buf.get(), 0, m_capacity);
}
```

  第三方观察者可能读到 `w == 0` 而 `r == 旧值`，此时 `availableRead()` 的 `static_cast<size_t>(w - r)` **无符号下溢**成天文数字。任何新增的采样线程都必须对「`w < r` 即视为复位中、丢弃本次采样」做防护，且不能在 `reset()` 期间 `read()`（memset 与 memcpy 并发是真实数据冲突）。
- ring 的生命周期由其 `shared_ptr` 决定（`PlaybackTask::ring`，`MusicPlayList.h:103`）；测试里用栈对象（`test_streaming_budget.cpp:148`）时，必须保证 `decoder.stop()` 早于 ring 析构（该用例 `:217` 显式 `decoder.stop()`）。

#### (b) 设备的读接口

```cpp
// sdk/audio/device/AudioDevice.h:40-43
    // Push mode interfaces
    int write(const void *data, size_t len);
    size_t getQueuedBytes() const;
    void clearQueue();
```

```cpp
// sdk/audio/device/AudioDevice.cpp:177-183
size_t AudioDevice::Impl::getQueuedBytes() const
{
    if (!m_isOpen || m_deviceID == 0) {
        return 0;
    }
    return SDL_GetQueuedAudioSize(m_deviceID);
}
```

线程安全前提：
- `m_isOpen` / `m_deviceID` 是**普通成员**（无锁、非原子），只在 WorkQueue 线程被 `open/close` 改写（`MusicPlayer.cpp:326`、`:384`、`:627`）。所以 `getQueuedBytes()` 必须与设备生命周期**同线程**调用，否则是数据竞争 —— 且 `open()` 之前恒返回 0，拿不到「设备未开」与「队列真空」的区别。
- `SDL_GetQueuedAudioSize` 本身是 SDL 的查询接口，可并发于 `SDL_QueueAudio`，但它读到的同样是瞬时值；生产用它做水位门限：`if (m_audioDev->getQueuedBytes() > kQueueThreshold)`（`MusicPlayer.cpp:540-543`，`kQueueThreshold = 256 * 1024`）。
- `write()` 只有返回值（`SDL_QueueAudio` 的 ret）可观察，不排队、不阻塞、不回报实际入队字节；**没有任何「已播字节 / 欠流事件」计数**（`AudioDevice.cpp:165-175`）。

---

### 2.6 Q6 — 「控制事件与消费定时任务共用同一 WorkQueue」：`_play()` 里有界等待会不会阻塞定时任务

#### (a) 共用是事实

```cpp
// sdk/MusicPlayer.cpp:107-108
    WorkQueue m_workQueue;
    WorkQueue::TaskID m_consumerTaskID;
```

```cpp
// sdk/MusicPlayer.cpp:139-141
    m_audioDev = std::make_shared<AudioDevice>();
    m_audioDev->getDeviceSpec(m_devSpec);
    m_musicList = std::make_shared<MusicPlayList>(this, &m_workQueue, m_devSpec);
```

```cpp
// sdk/MusicPlayer.cpp:195-199
    m_workQueue.asyncRunTask([this]() {
        // 用户主动播放 = 新的播放会话：上次因坏数据失败的曲目可以重新尝试
        m_musicList->clearFailedTracks();
        _play();
    });
```

（`pause/stop/next/previous/setCurrentIndex/setPosition` 同样 `asyncRunTask`：`:209`、`:219`、`:233`、`:241`、`:249`、`:257`。）

#### (b) WorkQueue 的执行语义：单线程、串行、锁外执行

```cpp
// sdk/cosmos/WorkQueue.hpp:162-168
    void start()
    {
        // 先置位再起线程：旧顺序（先起线程后置 true）下工作线程可能在
        // threadLoopFunc 首行读到未初始化的 m_running（UB，issue #142 可疑点 3）。
        m_running.store(true);
        m_thread = std::thread(&WorkQueue::threadLoopFunc, this);
    }
```

```cpp
// sdk/cosmos/WorkQueue.hpp:236-259（节选）
                if (!task->isRepeat.load()) {
                    m_taskQueue.pop();
                    m_taskMap.erase(task->id);
                } else {
                    // 先 pop 再改 key：在堆内修改参与堆序比较的 key 违反
                    // priority_queue 的前提（issue #142 可疑点 4）。
                    m_taskQueue.pop();
                    task->runTimePointUs += task->intervalTimeUs;
                    m_taskQueue.push(task);
                }
            }
            ...
            task->func();
```

**定时器语义（重要，容易被误读）**：重复任务按「上次计划时刻 + 间隔」重新入队（`:243`），且 `runTimePointUs` 在**执行前**就先更新。因此：

- 若某个任务占住 worker 超过一个周期，`difftime <= 0`，下一轮会**立刻**再执行一次（`:215-234` 的 `difftime > 0` 才等待）→ **不丢 tick，而是阻塞结束后「补跑」连发**。
- 10 ms 消费任务被 `_play()` 阻塞 200 ms，恢复后会**连续补跑约 20 次** `consumeAudioData()`，每次最多写 4096 B 静音/数据。设备队列有 256 KB 门限（`MusicPlayer.cpp:540-543`）兜底，不会无限堆积，但会形成一次突发。
- 同一队列里的 `asyncRunTask`（如位置上报 `:561-568`、EOS 自动跳轨 `:583-590`）同样被压后，且 `std::priority_queue` 对相同时刻不保证 FIFO（同队列内自投递会被插队：`consumeAudioData` 的 position 回调就是这种形态，`:561`）。

#### (c) 在 `_play()` 里加有界等待会阻塞什么

`_play()` 主路径 4 步（`:292-338`）：取当前曲目 → `startCurrentTask()` → `open()/start()` → `startConsumerThread()`。**今天它已经含阻塞调用**（间接 join）：

```cpp
// sdk/MusicPlayer.cpp:315-322
    // play 负责备好本次播放任务：stop 之后 ring / decoder 已被丢弃，这里必须建新的；
    // 暂停后恢复则沿用还活着的那个任务（缓冲不白扔）。顺序遵循规格 4.1：
    // 任务先就绪，再开设备、再起消费线程。
    if (!m_musicList->startCurrentTask()) {
```

→ `MusicPlayList::startCurrentTask()`（`:417-435`）→ `replacePlaybackTask()`（`:456-500`）→ `stopCurrentTask()`（`:472`）→ `task->decoder->stop()`（`:449-451`）→ `m_thread.join()`（`AudioStreamDecoder.cpp:225-227`）。所以「`_play` 会阻塞」是既存事实；新增等待是**量级**问题，不是性质问题。

但新增有界等待有两个新风险，且都会**必然**发生：

1. **阻塞消费任务**：`startConsumerThread()` 在 `_play` 尾部（`:336`）。若等待点放在它之后（例如「等设备首次 write」），消费者是同一线程 → **等待条件永远不会被满足**，有界等待退化为「每次都跑满超时」。这是设计性自锁，不是竞态。
2. **拖长 destructor**：`~Impl` 先 `m_workQueue.shutdown()`（`:158`）再关设备；`shutdown()` 内部 `m_thread.join()`（`WorkQueue.hpp:169-192`）。若 `_play` 正在有界等待，析构的最坏延迟 = 该等待的上界。CI 侧对应的是 ctest `TIMEOUT`。

现有阻塞的上界（供对照，不是本票要解决的问题）：网络源的 `rw_timeout=3s`、重连总预算 ≤12 s 都写在解码线程注释里（`AudioStreamDecoder.cpp:1148-1154`），因此 `_play` 的 join 最坏可达数秒；`sdk_interaction_tests` 常态 11.8–14.2 s / `TIMEOUT 180`（`sdk/test/CMakeLists.txt:256-261`）有足够余量。

#### (d) `TIMEOUT` 现状（逐条）

| ctest 目标 | TIMEOUT | 位置 |
|---|---|---|
| `sdk_unit_tests`（快集，排除三组） | 180 | `sdk/test/CMakeLists.txt:213-222` |
| `sdk_badinput_{rejection,frames,duration,tags}_tests` | 120 ×4 | `:227-238` |
| `sdk_streaming_tests` | 180 | `:240-249` |
| `sdk_interaction_tests` | 180 | `:251-261` |
| 7 个 `TestSdkSuite` 老目标 | 1500 ×7 | `:74-138` |

注释明确「套件分组：单个 ctest 目标的墙钟时间必须给 TIMEOUT 留出真实余量，否则『挂死检测』会退化成『正常跑完也超时』」，并给了实测（StreamingBudget 59 s / 24 例，全量 184 s，`:194-201`）。

#### (e) 有没有「因此超时」的先例：**未找到；唯一超时先例的根因不是这个**

仓库里有记录的 ctest 超时只有一处，且已归因到别处：

```text
// doc/player-state-contract.md:231（同一事实亦见 AGENTS.md:132）
同 run 的 sdk_unit_tests (Timeout) 180s 挂死是同一条记账不一致的另一面。
```

即 #141/#142 期间 `sdk_unit_tests` 180 s 挂死，根因是 `WorkQueue` 的 `m_taskMap` / `m_taskQueue` 记账分叉（worker 在空队列上 `top()/pop()`），由 `bc0fb24f4` 修复。**没有任何记录显示「`_play` 内的阻塞 / 有界等待」造成过 ctest 超时**。反过来说：这也不构成「可以随便加等待」的证据——只是先例缺失。

---

## 3. 三组判据（首播门槛 / 欠流计数 / 切换窗口峰值）的可行性判断

| 判据 | 现有注入面 / 仪表 | 今天能否落地 | 缺口 |
|---|---|---|---|
| **首播门槛**（start → 首帧交付） | `startToFirstPcmUs`（`AudioStreamDecoder.cpp:969-980`）+ `packetsPulledAtFirstPcm`（`:978`）+ 现成断言形制（`test_streaming_budget.cpp:302-313`） | **能，但只能落在「ring 写入」粒度，且只能在测试进程里判** | ① 规格写的是「设备首次 `write()`」（`doc/streaming-playback-spec.md:107`），当前时间戳打在 `m_ring->write()` 成功处（`:969-976`）；② 生产层无读出面（§2.1(b)），`Player` 拿不到这个数 |
| **欠流计数**（稳态窗口静音填充 == 0） | `silenceFills`（`StreamMetrics.h:20-25`，`AudioStreamDecoder.cpp:312-323`） | **不能。它是「静默窗口内的填充次数」，不是欠流次数** | ① 窗口判据与生产窗口不重合：`m_switching` / `isShuttingDown()` 解码器看不见（`MusicPlayer.cpp:484-486` vs `AudioStreamDecoder.cpp:318-321`），这两类窗口的填充记不进去；② 真正的欠流路径 `got == 0` 既不写静音也不计数（`MusicPlayer.cpp:545-570` 的 `if (got > 0)` 分支；规格自认这条限制：`doc/streaming-playback-spec.md:113`） |
| **切换窗口峰值**（2×ring 并存） | 无。`peakHeldBytes` 排除 ring（`StreamMetrics.h:17-18`）、per-decoder（`AudioStreamDecoder.h:215`）；切换路径先 join 再建 ring（`MusicPlayList.cpp:471-480`）；`AudioRingBuffer` 无全局记账（`AudioRingBuffer.h:37-43`） | **不能，而且当前窗口不存在**（最多 1 枚 ring 存活） | 需要先有「软取消」（旧任务保留到新任务首帧），才存在 2×ring 窗口；在此之前这条判据无对象可判 |

### 3.1 需要新增测试钩子的部分（只给方案，不改代码）

**A. 首播门槛若要判到「设备首次 write」**

方案 A1（推荐，改动最小、写入点每任务一次）：在 `StreamMetrics` 增 `firstDeviceWriteUs`；由 `consumeAudioData()` 在首次 `m_audioDev->write(...)` 成功后写一次（`std::atomic<uint64_t>` + `exchange` 防重复），起点仍用 decoder 的 `m_startClock` 语义（需要 decoder 暴露 `startClock` 或让 `startToFirstPcmUs` 的起点在 `MusicPlayer` 侧复制一份）。
风险：写入点在**消费主循环**，命中 `AGENTS.md:124` 的禁令条款（「绝不落解码线程 / 消费主循环 / 判据主循环」），放行必须做同轮数 `ctest -j4` 对照（该条记录：这类额外写会把 `-j4` 假红率从 0 抬到约 30%）。缓解：只在「首次」写一次（用 `exchange` 保证后续全是读），不引入每拍写。

方案 A2（零生产改动的近似）：维持 `startToFirstPcmUs`（ring 写入）作为**上界代理**，并在报告里显式标注「ring 粒度 ≤ 设备粒度（相差一次 10 ms 消费节拍 + 设备队列启动）」。判据阈值需把这段差值算进容差。

**B. 欠流计数**

方案 B1（判据侧，最小侵入）：不新增生产钩子，改由测试夹具做「欠流采样」——在消费循环里，当 `!isSilenceWindow(decoder)` 且 `ring.availableRead() == 0` 且 `audioDev.getQueuedBytes()` 低于一个水位时记一次 `underrunSample`。`playWithExtractor` 目前不接 `AudioDevice`，需要新增一个不依赖真实设备的「节拍器」版本（按 `bytesPerMs` 虚拟推进消费时钟）。
风险：这是**判据主循环**里的写，同样命中 `AGENTS.md:124`；且采样粒度受 `kPollMs` 影响，会变成机器速度相关量（`test_streaming_budget.cpp:96-102` 已经踩过同类坑）。

方案 B2（生产侧，口径更正）：把窗口判据改成「解码器自己知道的事实」，即新增 `StreamMetrics::underrunSamples`，由消费端在 `got == 0 && !shouldFillSilence()` 时自增（`MusicPlayer.cpp:547-570` 之间），并把 `silenceFills` 的文档口径从「三类窗口」改成「实际可计数的窗口」。这修正的是**口径 bug**（文档与实现不一致），但同样要在消费主循环加写。

方案 B3（只读推导，零新增写）：用 `readPos()` 的时间序列离线推导欠流——测试侧每拍记录 `(t, readPos, availableRead)`，用「readPos 增长斜率 < bytesPerMs」判定欠流。全部写落在测试侧，但仍在判据主循环。

三者都必须过 `AGENTS.md:124` 的稳定性闸门；若不能过，欠流判据只能降级为「稳态窗口静音填充 == 0」（注意：受 §2.1(e) 的窗口错位影响，这条几乎是**恒真**的空判据）。

**C. 切换窗口峰值（2×ring）**

方案 C1（先决条件）：先实现「切歌软取消」——旧任务不立即 `stopCurrentTask()`，而是保留到新任务首帧交付后再 join（`MusicPlayList.cpp:471-480` 的顺序要改）。这条本身是产品行为改动，不是测试钩子。

方案 C2（记账）：在 `AudioRingBuffer` 增加进程级原子量（ctor `fetch_add(capacity)` / dtor `fetch_sub`）与峰值 CAS，暴露 `static size_t peakConcurrentBytes()`。写入点在 **ring 构造/析构**（控制线程），不在解码线程、不在消费主循环 → 是三个方案里唯一不碰 `AGENTS.md:124` 禁令区的。缺点：这是生产类的静态状态；同一进程内 doctest 串行执行，故无并发写冲突，但跨进程互不可见（各 ctest 进程独立统计，恰好符合隔离口径）。

方案 C3（切换路径显式记账，不引入静态量）：在 `replacePlaybackTask()` 里，软取消后同时持有 old/new 两枚 `shared_ptr<AudioRingBuffer>`，用 `old->capacity() + new->capacity()` 记一次 `Metrics::RecordTiming`/`RecordGauge` 型指标（`Metrics.cpp` 已有 `RecordTiming` 的用法可循，`MusicPlayList.cpp:496-498`）。写入点同样在控制线程。

方案 C4（纯测试侧，零生产改动）：不可行——测试拿不到 `replacePlaybackTask` 内部的两枚 ring；`currentTask()` 在切换完成后只暴露新任务（`MusicPlayList.cpp:486-490`）。

---

## 4. 风险清单

### 4.1 指针生命周期

| # | 风险 | 证据 | 触发条件 |
|---|---|---|---|
| L1 | `stopCurrentTask()` **就地置空**共享任务对象的成员，任何别的 `PlaybackTaskPtr` 持有者会看到 `decoder == nullptr` | `MusicPlayList.cpp:452-453`；`MusicPlayList.h:103-104` | 今天不可达（同线程串行）；一旦仪表抽样 / 监控泵另起线程即成立 |
| L2 | `task->decoder != nullptr` 检查与使用之间的 TOCTOU | `MusicPlayer.cpp:521-523`、`:533-538` | 同上；同线程时无窗口 |
| L3 | `metrics()` 内部触到提取器（`m_extractor->packetBoundaryMicros()`、`heldPayloadBytes()`），而提取器 reader 的非原子字段（`m_buf` 的 `capacity()`、`m_reader` 指针）由解码线程改写 | `AudioStreamDecoder.cpp:280`、`:301-310`；`FLACExtractor.h:241-244`；`FLACFrameReader.h:38`（`return m_buf.capacity();`）；`FLACFrameReader.cpp:142,148`（`clear`）与 `:256-264`（`reserve` / `resize`）；`MKVExtractor.cpp:50-51` | 任何跨线程 `metrics()` 调用。**`AudioStreamDecoder.h:75` 的「可从消费线程读取」对这两个字段不成立** |
| L4 | 析构顺序依赖：`PlaybackTask` 成员声明顺序保证 decoder 先于 ring 析构；测试里用栈 ring 时必须先 `stop()` | `MusicPlayList.h:92-93`、`:103-104`；`test_streaming_budget.cpp:148,217` | 手工构造 `ring + decoder` 的新测试若忘记 `stop()`，`decoder` 析构时 `join` 仍会跑（`AudioStreamDecoder.cpp:86-89`），但那时 ring 已亡 → 解码线程写已释放 ring |

### 4.2 线程归属

| # | 风险 | 证据 |
|---|---|---|
| T1 | 控制事件、消费（10 ms）、位置回调、EOS 自动跳轨全在**同一条** WorkQueue 线程；任何一处阻塞会同时阻塞其余全部 | `MusicPlayer.cpp:107,141,652,195-198,561-568,583-590`；`WorkQueue.hpp:256` |
| T2 | `_play()` 里「等设备首写 / 等消费产物」是设计性自锁（消费者同线程且尚未启动或已被阻塞） | `MusicPlayer.cpp:336`（消费者在 `_play` 尾部才注册） |
| T3 | 重复定时任务按定点 + **追平**执行：阻塞 200 ms 后连发约 20 次 4096 B 写 | `WorkQueue.hpp:215-234,236-245` |
| T4 | `AudioDevice::getQueuedBytes()` 读普通成员 `m_isOpen`，与 `open/close` 必须同线程 | `AudioDevice.cpp:177-183`；`MusicPlayer.cpp:326,384,627` |
| T5 | ring 的 `reset()` 用两次 `relaxed` store，第三方观察者可能读到 `w < r` → `availableRead()` 无符号下溢 | `AudioRingBuffer.cpp:92-97`、`:62-68` |
| T6 | 测试侧「消费端」是测试主线程，与生产不同线程；同一函数（`noteSilenceFill`）在两处被不同线程调用，语义靠原子量兜底但不保证窗口时序一致 | `test_streaming_budget.cpp:114,177,187,191` vs `MusicPlayer.cpp:523` |

### 4.3 并发 `-j4` 下的共享状态

| # | 风险 | 证据 |
|---|---|---|
| P1 | **新增钩子的写入位置是本票最大风险**：仓库明令「绝不落解码线程 / 消费主循环 / 判据主循环」，落在那里会把 `-j4` 假红率从 0 抬到约 30%，且放行要「同轮数 `ctest -j4` 对照」，10 轮分辨不出 2.5% 与 0%（至少 40 轮） | `AGENTS.md:124`、`:129`（稳定性闸门样本量条） |
| P2 | 12 ms SEEKING 窗口 + 5 ms / 1 ms 轮询在负载下会漏采样，已有 CI 实测漏过一次 | `test_streaming_budget.cpp:96-102`；`test_playback_lifecycle.cpp:174-177` |
| P3 | 判据套件自身耗时长（StreamingBudget 59 s / 24 例），新增用例会挤压 `TIMEOUT 180` 的余量 | `sdk/test/CMakeLists.txt:194-201,240-249` |
| P4 | 跨进程共享状态**已隔离**：日志按进程号分文件、测试临时文件按 `processId()` 命名 | `UnitTestsMain.cpp:14-16`（`"unittest_" + processId()`）；`sdk/test/fixtures/TestProcess.hpp:9`（「凡是测试进程写出去的路径，都必须带上 processId()」） |
| P5 | 进程内静态测试钩子（如 `AudioDevice::setForceOpenFailureForTest`）是**进程级**开关，doctest 串行执行时仍是全局状态，用例顺序敏感 | `AudioDevice.h:30-34` |
| P6 | 取证时点上工作树还存在另一处未提交的 `TEMP-DIAG`（#174）：它在 `writeDecodedBytes` 的 ring 写循环里加了 `prefillNoteWritten()` 与 `std::printf` —— 正好落在**解码线程**，与本条禁令同类。任何在此工作树上取的时序/水位数值都会被它污染，且它通过 `printf` 引入额外的锁与 IO 抖动 | `git diff sdk/audio/decode/AudioStreamDecoder.cpp`（+48 行，未提交）；`AGENTS.md:124` |

### 4.4 口径漂移（会直接让判据假绿）

| # | 问题 | 证据 |
|---|---|---|
| M1 | `silenceFills` 文档称覆盖「shuttingDown / switching / SEEKING 三类窗口」，实现只可能覆盖 SEEKING（另两个标志只在 `stop()` 进行中被置位，且 `stop()` 末尾会清回 `false`） | `StreamMetrics.h:20-23` vs `AudioStreamDecoder.cpp:318-321,233-235`；`MusicPlayer.cpp:484-486` |
| M2 | `startToFirstPcmUs` 量的是「ring 写入成功」，规格要求「设备首次 `write()`」 | `AudioStreamDecoder.cpp:969-976` vs `doc/streaming-playback-spec.md:107` |
| M3 | `packetBoundaryMicros` 是提取器生命周期累计值，不是「本次播放任务」 | `AudioStreamDecoder.cpp:280`；`MKVExtractor.cpp:50-51`；`MKVPacketReader.cpp:246` |
| M4 | `initScanUs` 是提取器构造期常量，重播同曲不变 | `ExtractorFactory.cpp:192`；`ExtractorHelper.hpp:156` |
| M5 | `peakHeldBytes` 与判据右式的口径不同（左不含 ring/AVIO，右含），注释已写明但很容易在新增判据里写错边 | `StreamMetrics.h:17-18`；`test_streaming_budget.cpp:504-506` |

---

## 5. 附：本票未确认项（明确「未找到」）

1. **生产层读取 `metrics()` 的路径**：未找到（`MusicPlayer.*` / `MusicPlayList.*` / `player.h` 零命中）。
2. **`DecodeSupplyObserver` 的外部注入 / 注册口**：未找到（无 `setObserver()` / 注册表 / 工厂；唯一「可替换点」是 `FLACDecode::initFromPacketFactory` 的 observer 参数，`FLACDecode.h:41`，唯一调用方传 `this`，`AudioStreamDecoder.cpp:566`）。
3. **「慢供给」测试替身的既有实现**：未找到（§2.3(c) 的替身均不含 sleep；唯一既有 delay 在 HTTP mock：`sdk/test/mocks/HttpMocks.hpp:608`、`:751-760`，属网络源路径）。
4. **「2×ring 并存」的任何已有量测**：未找到（`AudioRingBuffer` 无静态记账；`peakHeldBytes` 排除 ring；切换路径无重叠）。
5. **「`_play` 内阻塞导致 ctest 超时」的先例**：未找到（唯一超时先例归因 `WorkQueue` 记账分叉，#142 已修）。
6. **设备侧欠流事件 / 已播字节计数**：未找到（`AudioDevice` 只有 `getQueuedBytes()` 与 `write()` 返回值）。
