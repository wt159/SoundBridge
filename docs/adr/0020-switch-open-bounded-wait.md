# 0020 切歌重开：有界等待 + 后台续做，控制面不阻塞在 open 上

- 状态：已接受
- 日期：2026-11-23
- 来源：[地图：切歌路径的阻塞 IO 与控制面响应预算](https://github.com/wt159/SoundBridge/issues/285)、[线程模型契约票](https://github.com/wt159/SoundBridge/issues/287)、[阻塞上限与可取消性研究](https://github.com/wt159/SoundBridge/issues/286)。规格落点 [`doc/player-state-contract.md`](../../doc/player-state-contract.md) §4.1、[`doc/buffering-switching-spec.md`](../../doc/buffering-switching-spec.md) §5、[`doc/streaming-playback-spec.md`](../../doc/streaming-playback-spec.md) §4.5。

## 背景

探针后关闭（ADR [0019](0019-datasource-open-close-lease.md)）把本地文件的 `open()` 从入列推迟到每次切歌前。`replacePlaybackTask()` 在控制线程（`WorkQueue` 串行线程）上同步调用 `FileSource::acquire()`，后者执行 `_wfopen` + `_fseeki64(SEEK_END)`（`sdk/extractor/FileSource.cpp:84-94`），**没有任何超时参数**。

研究（[#286](https://github.com/wt159/SoundBridge/issues/286)，证据 `research/switch-open-blocking-facts` `17ee918ba`）给出三条硬事实：

1. **不存在「单次 open 超时」**：`CreateFileW` / `open(2)` 都没有超时参数；阻塞上限只由挂载 / 注册表层决定，且可能是无上限（NFS `hard` 无限重试）。
2. **不能可靠取消**：Windows `CancelSynchronousIo` 可对「尚未返回句柄的 `CreateFileW`」发取消请求，但只是标记取消、异步生效、依赖驱动；POSIX 上普通信号 / `EINTR` / `pthread_cancel` 均无效，只有 `SIGKILL`（杀进程）。
3. **能放弃的只有等待者**：把 open 放在独立线程、不再 join，线程滞留到系统调用返回；Windows 期间不占文件句柄、保留 1 MB 栈、几乎不耗 CPU。

后果是结构性的：控制线程被阻塞多久，`stop` / `pause` / `seek` 就排多久。网络盘上 `stop` 可能等 60 s（`SessTimeout`），NFS `hard` 上可能永不返回——UI 表现为「按停止没反应」，且没有任何错误可看。

`stop` 的响应性与切歌的出声延迟是两个不同的预算：前者是硬保证，后者允许超窗但必须可见。本 ADR 只解决前者；出声预算、预取与可见性由同图其余票裁决。

## 决策

**五条，必须同时成立。**

### 1. 所有切歌重开一律经专用 IO 执行器，控制线程有界等待 50 ms

`acquire()` 不再在控制线程直接执行，而是作为一次「重开请求」提交给一条专用的 IO 执行器线程。控制线程在请求上最多等 `kSwitchOpenWaitMs = 50`：

- 50 ms 内完成 ⇒ 走同步提交路径，后续切换步骤当场做完。本地 SSD 上 open 是亚毫秒级，**行为与今天完全一致**。
- 超时 ⇒ 返回 `Pending`，控制线程立即交还；IO 执行器继续等系统调用返回，完成后把结果投回控制队列。

50 ms 而不是 100 ms：100 ms 是控制命令的硬预算，必须给「退役旧任务 + 建 decoder + 清设备队列」留余量；stop 恰好在等待开始时到达时，最坏 50 ms 后就能执行。

### 2. 切歌拆成 begin / commit 两阶段，净空只在 commit

`replacePlaybackTask()` 拆为：

- **begin**：校验条目与 extractor、提交重开请求、有界等待；失败则按 #276 的语义上报并返回，旧任务不动。
- **commit**：退役旧任务（只置取消位）、建新 ring / decoder、`decoder->start()`、发布选中项、清设备队列。

Pending 时 `m_switching` 保持 true（seek 仍按既有行为丢弃），旧任务**继续出声**——这是 #276「失败时旧任务不得被退役」的自然延伸：等待期用户听到的仍是上一首，而不是静默。设备队列的净空（`endSegmentSwitch` 的 `clearQueue`）延后到 commit：提前清空会截断正在播放的旧曲。

`nextSync()` / `previousSync()` / `setCurrentIndexSync()` / `skipToNextPlayable()` 的 `bool` 返回值语义改为「**是否接受了切换意图**」（Pending 也算 true），另加 `switchPending()` 供调用方判断是否已 commit。公开面不变。

### 3. 代际 + uid 双守卫，过期结果静默丢弃

每次切换意图递增 `m_switchGeneration`。IO 结果回投时校验两件事：代际仍是当前值、目标 `uid` 仍在列表中且仍是目标。任一不成立即丢弃，`release()` 租约，**不产生任何回调**。

由此得到命令语义：

| 等待期收到的命令 | 处置 |
|---|---|
| `stop` / `clearPlaylist` | 递增代际，立即执行既有停止路径；晚到的结果丢弃 |
| `removeTrack`（目标那条） | 递增代际；commit 前校验 uid 已不在列表则丢弃 |
| 再次 `next` / `previous` / `setCurrentTrack` | 递增代际，只保留最新目标；IO 线程上的在途请求无法取消，其返回后若已过期则直接丢弃并开始服务最新目标 |
| `play` | 只记「就绪后起播」意图，不取消切换；commit 时若有意图则走 `_play()` 既有路径 |
| `seek` | 维持既有 `m_switching` 丢弃语义，不新增分支 |

`play` 的意图在重开失败时**丢弃并只报一次错**，不偷偷回退去播旧曲——否则「点了下一首、又点了播放」会变成继续播上一首，属于静默违背用户意图。

### 4. 失败语义逐字沿用 #276

Pending 重开失败：一次 `onError(FileOpenFailed, index≥0, path)`、`markTrackFailed`、不重试、**播放状态不变**、旧任务继续播、设备队列不清空；只是额外清掉 pending 标记与 play 意图。自动跳轨的候选循环在 Pending 失败后从下一个候选继续，一次用户命令最多产生一个最终结果（切到某曲，或候选耗尽失败）。

### 5. 退场：标记失效，限时 200 ms，未返回则 detach

`MusicPlayList` 析构时先把执行器标记为停止，最多等 200 ms；仍未返回就 detach。执行器只持有请求值（`MusicPropertiesPtr`）与自有的共享状态，**不引用** `MusicPlayer::Impl` / `MusicPlayList` 的裸指针；结果经弱引用投递，目标已析构则丢弃。进程退出时由操作系统回收滞留线程。

这是对「open 不可取消」的显式让步，必须写进规格而不是留作实现细节：**不允许**为了「无线程泄漏」而无限 join，否则退出会被 SMB 60 s / NFS 无限重试顶住。此条**只**适用于 IO 执行器；解码线程的 join 策略（`buffering-switching-spec.md` §5.6「不新增后台线程、不 detach」）一字不动。

## 考虑过的替代方案

**(a) 纯异步零等待：控制线程永不等待，每次切歌都两阶段。** 否决：`nextSync()` 与 `skipToNextPlayable()` 的同步发布（`putMusicPlayListCurBuf`、`lastCurrentIndex` 立即更新）是既有内部契约与大量判据的前提（`test_playlist.cpp` / `test_playlist_advance.cpp`）；纯异步会把「是否已切」变成时序问题，测试与调用方全部要改写，而收益只是省掉 SSD 上 50 ms 以内的等待。

**(b) 同步切歌 + 仅预取下一条。** 否决：首次切到慢介质、跳播、列表变更都会未命中预取槽，控制线程照样阻塞。100 ms 的 stop 预算无法由「命中率」保证——预取是延迟优化，不是可响应性保证。预取本身仍可在本图后续票里作为独立优化裁决。

**(c) 无限等待：控制线程一直等到 open 返回。** 否决：等于现状，NFS `hard` 上永不返回。

**(d) 只把 `stop` 做成插队命令（优先级队列）。** 否决：单线程无法打断正在执行的系统调用；把 `stop` 提到队首仍然要等当前调用返回。不换线程就没有解。

## 后果

**正向**

- 控制命令的响应性与介质无关：最坏 50 ms 交还控制线程（`stop` 恰逢等待起点）。
- 本地 SSD 上零行为变化：open 在 50 ms 内完成，commit 同步发生，`nextSync()` 的返回值与回调时序与今天一致，既有判据不需要改写。
- 失败语义、状态机、设备净空时点都复用 #276 / #173 的既有裁决，不新增错误码、不改 ABI（全部改动在 `sdk/` 内部头与 .cpp）。
- 「等待期旧曲继续播」把慢盘的代价从「静音/假死」降为「晚几秒换歌」，且用户能看到「切换中」（UI 票）。

**负向 / 已知代价**

- 切歌路径分成同步 commit 与 pending commit 两条出口，必须共用同一个 commit 函数，否则两条路径会漂移；这是实现期最需要变异验证的地方。
- 每次切歌多一次线程投递（微秒级）；Pending 路径多一次控制队列回投。
- IO 执行器串行：在途 open 无法取消，最新目标要等在途请求返回才能开始。这是单线程的取舍——用「最多等一个系统调用超时」换「同时至多一个重开、句柄预算推理简单」。若将来证明连续跳播在慢盘上不可接受，再评估小型并行池。
- 退出时可能 detach 一条仍在 `open()` 里的线程；它只碰 shared_ptr 状态，进程退出由 OS 回收。

**反悔条件**

- 若真机实测显示 50 ms 等待在主流 SSD 上频繁超时（例如杀软实时扫描常态 >50 ms），应重新评估阈值，而不是放宽控制预算。
- 若连续跳播在慢介质上的等待被证明不可接受，另开 ADR 评估并行 IO 池与请求取消语义（后者仍受「open 不可取消」约束）。
- 若将来 `addMusicDir` 的扫描并行化落地（ADR 0019 反悔条件 ③），IO 执行器是否复用给探测路径，需另裁。
