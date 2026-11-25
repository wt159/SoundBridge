# 0021 扫描：专用线程 + 请求级状态，控制面零占用

- 状态：已接受
- 日期：2026-11-25
- 来源：[地图：扫描不阻塞控制面](https://github.com/wt159/SoundBridge/issues/313)、[扫描链路取证](https://github.com/wt159/SoundBridge/issues/314)、[线程模型裁决](https://github.com/wt159/SoundBridge/issues/315)、[顺序·失败汇总·取消语义](https://github.com/wt159/SoundBridge/issues/316)、[句柄上限与判据](https://github.com/wt159/SoundBridge/issues/317)、[扫描期播放语义](https://github.com/wt159/SoundBridge/issues/319)。规格落点 [`doc/streaming-playback-spec.md`](../../doc/streaming-playback-spec.md) §4.5、[`doc/observability-contract.md`](../../doc/observability-contract.md) §5.1、[`doc/player-state-contract.md`](../../doc/player-state-contract.md) §4.1。

## 背景

`addMusicDir` 的扫描由两段组成，两段都在阻塞别的东西：

1. **目录遍历**（`recursiveFileSearch`，`sdk/extractor/FileSearch.cpp`）在**调用线程**同步跑完。生产路径的调用线程就是 UI 线程，且发生在 `MainWindow` 构造期、`show()` 之前——慢盘上冻结的是**首帧**，用户看不到窗口（`app/mainwindow.cpp:182`、`:847-858`；`app/main.cpp:128-131`）。
2. **逐文件探测**经 `addMusicWithNotify` 投到控制用 `WorkQueue`（`sdk/MusicPlayList.cpp:288-297`）。该队列是**按 `runTimePointUs` 排序的时间戳优先队列**（`sdk/cosmos/WorkQueue.hpp:39-46`、`:113`），扫描期发出的 `stop` / `pause` / `seek` 时间戳必然晚于已在队的探测 ⇒ 排在最后。同一队列上还跑着 10 ms 音频消费节拍（`sdk/MusicPlayer.cpp:1086`）⇒ **边播边扫会欠流**。

单次探测的阻塞点在系统调用：`_wfopen` / `fseek(END)` / `ftell` / `fread` 读头 / `fclose`（`sdk/extractor/FileSource.cpp:88`、`:96-99`、`:289-292`、`:134`）。这些调用在 SMB / NFS 上可能以十秒计，且不可取消（[#286](https://github.com/wt159/SoundBridge/issues/286)）。

既有的两条相关裁决都只解决了切歌路径：[ADR 0019](0019-datasource-open-close-lease.md) 让条目只留数据源对象与元数据、探测后立即归还句柄（扫描期峰值 1 依赖「WorkQueue 串行」这一前提）；[ADR 0020](0020-switch-open-bounded-wait.md) 让切歌重开走专用 IO 执行器 + 50 ms 有界等待。**扫描路径一个字都没动。**

## 决策

**七条，必须同时成立。**

### 1. 扫描 = 一条专用线程，遍历与探测都在它上面

新增 `ScanExecutor`（1 条线程）。目录遍历与逐文件探测都在这条线程上**顺序**执行。调用线程提交一次扫描请求即返回。

**不引入并行探测池。** 慢盘上扫描总时长不变，这是显式接受的代价：目的地是「不顶住控制面」，串行搬线程已完全满足；并行收益（延迟受限介质上数倍）**在本机不可证**（两块 NVMe，探测亚毫秒级），而代价（序号重排、句柄判据拆分、取消代际）是常驻的。与 [ADR 0020 决策 6](0020-switch-open-bounded-wait.md) 拒绝预取用的是同一条推理。

### 2. 探测不进控制队列：形态是「不排队」，不是「有界等待」

`WorkQueue` 在扫描期只承担两类极小任务：扫描线程回投的「单条插入」与原有的控制命令。因此**不需要**新增有界等待常量，也**不需要**给 `WorkQueue` 加优先级或插队机制——探测一旦不入队，控制命令前面就没有东西可排。

这一条同时消掉「边播边扫欠流」：10 ms 消费节拍不再与探测共享队列。

### 3. 顺序：串行产出 + 按到达序插入，配背压

扫描线程**串行**产出结果 ⇒ 到达控制队列的顺序 == 扫描顺序 ⇒ `doc/app-ui-spec.md:196`「扫描顺序即列表顺序」保住，**不需要 seq 重排槽**。

但回投必须**有界**：控制队列中「已回投未插入」的条数上限 `kScanInsertOutstandingMax = 8`，达到上限扫描线程等待（在请求对象上等条件变量）。目的不是防阻塞（单条插入是 O(1)），而是防 1000 条一次性涌入把 10 ms 消费节拍挤在后面。

### 4. 不复用 `SwitchOpenExecutor`

四条事实缺口：①它是单线程，复用后探测仍串行却要与切歌抢同一条线程；②`runBounded` 的语义是「等不到就放弃」，与扫描「每条都必须有结果并递减计数」相反；③`shutdown` 会静默丢弃排队 job 且不给完成信号，直接套用会让扫描计数永久不为 0；④队列无界，并发上限需外部另设。

扫描线程与切歌 IO 执行器**物理隔离**。[ADR 0020](0020-switch-open-bounded-wait.md) 反悔条件③（「扫描并行化后 IO 执行器是否复用给探测路径」）由此得到答复：**不复用**。

### 5. 探测与入列分离，扫描线程不碰播放列表状态

扫描线程**只产出自包含结果**（路径 + 已填好 `processProperties.extractor` / `signalProperties.spec` / `durationMs` 的 `MusicPropertiesPtr` + 成功/失败与 `ErrorCode` + `detail` + `openMs`）。以下全部只在控制线程做：`index = size()` 赋值、`uid` 赋值、`push_back`、`m_count` 更新、首曲登记、拒绝记账、汇总触发、**任何 `m_callback->*` 调用**。

可行性已核验：`ExtractorFactory` 注册表有全局互斥量（`sdk/extractor/ExtractorFactory.cpp:135-146`）、`Metrics` 只做格式化 + g3log（`sdk/Metrics.cpp:23-49`）、`deriveMusicId` 是纯函数（`sdk/MusicPlayList.cpp:198-202`）、`FileSource` / extractor 实例各自独占；而 `m_musicListProperties` / `m_count` / `m_selectMusicProperties` / `m_nextUid` 是无互斥量的裸成员（`sdk/MusicPlayList.h:199`、`:201-205`），只能在控制线程写。

### 6. 请求级状态：`ScanRequest`

每次 `addMusicDir` 生成一个自包含请求对象，持有：`generation`、`rootPath`、`cancelled`（原子）、`pending`（原子）、`outstandingInserts`（原子）、以及只在控制线程读写的 `rejects` / `causes`。

- 取消位由**控制线程**置（`clearPlaylist` / 析构 / 新的 `addMusicDir`），扫描线程在**每个文件边界**检查一次；卡在 `_wfopen` 里的那一次不可取消，最多延迟到它返回。
- 代际校验发生在**插入时**：回投只携带请求的 `shared_ptr` 与结果，控制线程在插入前校验「仍是当前请求」且「未取消」，否则丢弃结果、只递减 `pending`，**不产生任何回调**。
- **第二次 `addMusicDir` = 取消旧请求、开新请求（后者胜）**：与 ADR 0020 决策 3 对 `next` / `previous` / `setCurrentTrack` 的处置同形；排队会让用户看到「第二次选目录没反应」。
- `m_scanRejects` / `m_scanRejectCauses` 与 `m_pendingAdd` **下沉为请求成员**。这消掉两条既有缺陷：前者只在 `reportScanSummary()` 的 `swap` 里清空（`sdk/MusicPlayList.cpp:556-557`），扫描被废弃时拒绝计数会串到下一批；后者是 `store` 语义（`sdk/MusicPlayer.cpp:243`），连续两次 `addMusicDir` 会互相打断、计数可能打成负数。
- **被取消的请求不产出汇总**（清空是终态命令，与 `clearPlaylistSync` 的「幂等、零回调」一致）。

### 7. 生命周期：有界等待后 detach，且这需要扩展 ADR 0020 决策 5 的适用范围

析构时置 `cancelled` → 最多等 `kScanShutdownGraceMs = 200` → 仍未返回则 `detach`。依据与 [ADR 0020 决策 5](0020-switch-open-bounded-wait.md) 相同：`open` 不可取消，无限 join 会让退出被 SMB 60 s / NFS 无限重试顶住；扫描线程只碰自有的请求状态，结果经弱引用投递，目标已析构则丢弃。

**必须显式记录**：ADR 0020 决策 5 的原文写「此条**只**适用于 IO 执行器」。本 ADR 把该让步的适用范围扩展为**两类 IO 线程**（切歌执行器、扫描执行器），理由不变。这是一处对既有措辞的扩展，不是静默改写——ADR 0020 正文不动，由本 ADR 承接。

## 考虑过的替代方案

**(a) 并行探测池（4 路）。** 否决：收益在本机不可证（NVMe 上探测亚毫秒级），代价是常驻复杂度——序号重排槽、`datasource_open_peak` 判据拆成稳态/扫描两条、取消代际要处理乱序完成。若将来真机慢盘实测证明千曲扫描不可接受，再按反悔条件重开。

**(b) 复用 `SwitchOpenExecutor`。** 否决：见决策 4 的四条缺口，其中「`shutdown` 静默丢弃」会直接让扫描计数永久不为 0。

**(c) 给 `WorkQueue` 加优先级或让 `stop` 插队。** 否决：单线程无法打断正在执行的系统调用；把 `stop` 提到队首仍要等当前探测返回。不换线程就没有解——这与 [ADR 0020 考虑过的替代方案 (d)](0020-switch-open-bounded-wait.md) 是同一条判断。

**(d) 探测仍进控制队列，只降低优先级。** 否决：仍然占用同一条队列带宽，10 ms 消费节拍照样被挤；「优先级」只改变相对次序，不改变「探测在队里」这一事实。

**(e) 惰性打开 / 延迟探测（入列不探测）。** 否决：[ADR 0019 决策 1](0019-datasource-open-close-lease.md) 已裁「探测后关闭」，`addTrack` 的失败即报契约与 `durationMs` / `AudioSpec` 的来源都依赖入列期探测；改成惰性等于重写契约。

## 后果

**正向**

- 首帧不再被目录遍历占住：UI 线程提交请求即返回。
- 扫描期控制命令与 10 ms 消费节拍**零延迟**（探测不在队列里），且这一保证与介质无关。
- 顺序语义、`index = size()`、首曲登记全部保住——不需要重排机制。
- 请求级状态把「连续两次 `addMusicDir` 互相打断」与「拒绝计数跨批串台」两条既有缺陷一起修掉。
- 公开面零改动：`ErrorCode` 不新增、`PlayerState` 三态不变、无新增能力位、ABI 保持 2。

**负向 / 已知代价**

- 扫描总时长不变（串行）。慢盘上千曲仍是逐条探测。
- 新增一条线程与一套请求状态（`ScanRequest` 的生命周期需要与扫描线程的 detach 共存）。
- 第二次 `addMusicDir` 丢弃第一次的成果（后者胜）。若用户误触，第一次已扫出的条目不会入列。
- 退出时可能 detach 一条仍在 `open()` 里的扫描线程；进程退出由 OS 回收。
- 扫描期「此刻已入列」的列表在变长，`next` 的「末条」是移动目标——这是语义选择而非缺陷，已由地图的播放语义裁决固定。

**反悔条件**

- 真机网盘 / 机械盘上实测「千曲扫描耗时不可接受」且用户可感知 ⇒ 重新评估并行池，并把句柄上限与判据一并重裁（届时 `kScanProbeConcurrency + 1` 成为新上界，既有 `peak == 1` 判据才需要作废重写）。
- 扫描线程与控制线程并发探测被证明常态化（扫描窗口峰值长期 > 1 且伴随句柄失败）⇒ 需要把两条探测路径合并到单一执行器。
- 若 `WorkQueue` 改成 FIFO 或引入优先级（推翻本 ADR 背景段的时间戳优先事实），决策 2 的「不需插队机制」需重新论证。
- 若用户实测反馈「不希望扫描中途能播」（认为半截列表不可信）⇒ 改回「等扫描完起播」，需重开票。
