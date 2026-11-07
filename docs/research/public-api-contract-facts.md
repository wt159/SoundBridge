# #76 播放器状态与公开 API 契约：现状事实基线

> 事实票（`research` / AFK）。只给可引用的事实与证据，**不做任何设计决策**，不改动 master 上任何既有源码或文档。
> 行号基于 `master` @ `dc8c12229`（2026-11-07）；本文件是新增文档，沿用仓库 CRLF 行尾。

## 0. 范围、方法与引用约定

- 事实源：本仓库源码（`sdk/`、`app/`）与仓库文档（`doc/`、`docs/`）。唯一外部佐证是 Qt 5.15 官方 `Qt::ConnectionType` 文档（用于 §4.4 的 AutoConnection 语义），不引入任何二手分析。
- 方法（`research` skill）：每条结论追溯到 primary source（源码 / 规格原文），并以「Grep 全量引用点 ↔ 逐文件 Read」交叉验证；与地图 #75 Notes「现状快照」冲突处以**实测**为准（§7）。
- 检索纪律：Grep 一律限定 `path`，并以 `!**/3rdparty/**` 排除 `sdk/3rdparty`（8 万+ 已追踪文件）；未在仓库根做无范围搜索。
- 产出：五张表（线程 / 回调 / 错误 / 状态 / 断言）+ 文档偏差清单（§6）+ 与地图快照的更正点（§7）。
- 本票**不做**：不裁定三态 / 六态、不改 `PlayerCallbacks` 签名、不定义 seek 失败的表达、不建议是否给 3 个零上报码补上报点、不编辑 #75 body、不改 master 既有文件。

---

## 1. 线程表

| # | 事实 | 证据 |
|---|---|---|
| T1 | `WorkQueue` 构造即 `start()`，只创建**一条** `std::thread` 跑 `threadLoopFunc`；所有任务的唯一执行点是 `task->func()`，且在 `m_taskMtx` 锁外执行 | `sdk/cosmos/WorkQueue.hpp:46`（构造调 start）、`:142`（`m_thread = std::thread(&WorkQueue::threadLoopFunc, this)`）、`:153-201`（单循环）、`:196`（`task->func()` 在 `:190` 解锁之后） |
| T2 | 7 个公开控制命令全部经 `m_workQueue.asyncRunTask(...)` 投递，执行体是 `_play/_pause/_stop/_setPosition/_next/_previous/_setCurrentIndex` | `sdk/MusicPlayer.cpp:175,189,199,213,221,229,237`；命令入口 `:168-238` |
| T3 | 「10 ms 消费循环」不是独立线程：`startConsumerThread()` 用 `m_workQueue.startTimerTask(true, 10, ...)` 在**同一个** `m_workQueue` 注册重复定时任务；函数名 `startConsumerThread` 名不副实 | `sdk/MusicPlayer.cpp:617-624`（尤其 `:622`）；`WorkQueue::startTimerTask` `sdk/cosmos/WorkQueue.hpp:56-78` |
| T4 | `MusicPlayList` 与 `MusicPlayer` 共用**同一个** `WorkQueue` 实例（构造时传入 `&m_workQueue`） | `sdk/MusicPlayer.cpp:92`（成员 `WorkQueue m_workQueue`）、`:121`（`std::make_shared<MusicPlayList>(this, &m_workQueue, m_devSpec)`）；`sdk/MusicPlayList.cpp:19-24`（`m_workQueue(wq)`） |
| T5 | `putMusicPlayListCurBuf` / `updateMusicList` / `onMusicPlayListError` 的全部调用点都在投递到该 `m_workQueue` 的任务里 → 与 T2/T3 同线程 | `sdk/MusicPlayList.cpp:43,48,57,61,65,69`（各 `asyncRunTask`）；回调触发点 `:191,303,323`（putCurBuf）、`:342`（updateList）、`:242,463`（error） |
| T6 | 六个 `MusicPlayerListener` 回调全部在上述 WorkQueue 任务体内直接调用；解码线程不直接发回调（由 10 ms 任务轮询 `decoder->state()` 后经 `handleDecodeError` 上报） | `sdk/MusicPlayer.cpp:287,392,536,590,653-655,671,687,702,726`；`consumeAudioData :482-563`；`handleDecodeError :565-615` |
| T7 | `PlayerCallbacksAdapter` 六个回调**同步直接转发**，无 marshal / 无队列 / 无跨线程投递；调用线程即回调线程（= WorkQueue 线程） | `sdk/player_impl.cpp:9-101`（每个 `onMusicPlayerXxx` 方法体内直接调 `m_callbacks->onXxx`） |
| T8 | 停机期静默：控制命令先查 `isShuttingDown()`；`asyncRunTask` / `syncRunTask` 在 `!m_running` 时静默返回；回调触发点普遍查 `isShuttingDown()` | `sdk/MusicPlayer.cpp:171-173,185-187,195-197,210-212`；`sdk/cosmos/WorkQueue.hpp:93-95,116-118`；`sdk/MusicPlayer.cpp:638,650,660,677,722`；析构 `:128-140` |
| T9 | 其它真实线程（非回调线程）：`AudioStreamDecoder::start()` 每次新建解码线程；SDL 音频设备回调由 SDL 管理 | `sdk/audio/decode/AudioStreamDecoder.h:39-41`；`sdk/MusicPlayList.cpp:413-415`；`doc/streaming-playback-spec.md:89-95`（线程约束清单） |

推论：T3 + T4 意味着 `doc/SoundBridge_Complete_Analysis.md:164-165,186-191` 的「控制流线程 (WorkQueue) + 数据流线程 (Dedicated Audio Thread)」双线程描述与现状不符（见 §6 D1）。

---

## 2. 回报表

回调链：`MusicPlayer::Impl` → `sdk::MusicPlayerListener`（`sdk/MusicPlayer.h:33-44`，六虚方法）→ `PlayerCallbacksAdapter`（`sdk/player_impl.cpp:9-129`，同步转发）→ `soundbridge::PlayerCallbacks`（`sdk/include/soundbridge/player.h:25-36`，六纯虚）→ `PlayerController`（`app/playercontroller.cpp:168-228`）。

| 公开回调 / 内部方法 | 线程 | 触发点（`sdk/MusicPlayer.cpp`） | 频率 | 先后 / 备注 |
|---|---|---|---|---|
| `onTrackChanged` / `onMusicPlayerListCurrentIndexChanged` | WorkQueue 单线程 | `:653`（`putMusicPlayListCurBuf`） | 每次切歌 / 首次选中 1 次 | 切歌三连发第 1 个；来源 `MusicPlayList.cpp:191,303,323` |
| `onDurationChanged` / `onMusicPlayerDurationChanged` | 同上 | `:654` | 同上，每首 1 次 | 三连发第 2 个；值取 `property->signalProperties.durationMs`（`MusicPlayList.cpp:294`） |
| `onPositionChanged` / `onMusicPlayerPositionChanged` | 同上 | `:655`（切歌 `curPositionMs` 初值）、`:392`（seek 后同步）、`:536`（常规播放，二次 `asyncRunTask` 内） | 切歌 1 次 + 每次 seek 1 次 + 每 10 ms 拍 `got>0` 时投递 1 次（经二次入队，不保证逐拍可见 / 不保证顺序） | 三连发第 3 个；详见下方「插队」段 |
| `onPlaylistChanged` / `onMusicPlayerMusicListChanged` | 同上 | `:671`（`updateMusicList`） | 列表更新时；仅 `m_musicListProperties.size()>0` 才发（空列表不发） | `MusicPlayList.cpp:339-344` |
| `onError` / `onMusicPlayerError` | 同上 | `:287`（PlayerNoCurrent）、`:590`（解码错误）、`:687`（扫描汇总 index<0）、`:702`（播放列表错误 index>=0） | 按需 | 两条错误路径顺序见 §3.2 |
| `onStateChanged` / `onMusicPlayerStateChanged` | 同上 | `:726`（`updatePlayState`）经 6 处调用：`:314`(Playing)、`:331`(Paused)、`:348`(Stopped)、`:601`(Paused)、`:612`(Paused)、`:700`(Stopped) | 每次状态迁移 1 次（停机期被 `:722` 拦下） | adapter 把 3 态映射为公开 3 态（`player_impl.cpp:16-33`） |

**切歌三连发（严格、同步、同一任务体内）**：`putMusicPlayListCurBuf`（`:636-656`）内 `m_switching.store(false)`（`:649`）→ `isShuttingDown` 检查（`:650`）→ `onTrackChanged`（`:653`）→ `onDurationChanged`（`:654`）→ `onPositionChanged`（`:655`）。即 `index → duration → position`。

### position 二次 `asyncRunTask` 的插队与 `priority_queue` 同刻无序（票面重点）

- `consumeAudioData`（10 ms 定时任务，已在 WorkQueue 线程里执行）在 `got>0` 时**不直接发**回调，而是 `m_workQueue.asyncRunTask([...]{ ... onMusicPlayerPositionChanged(curPositionMs); })`（`sdk/MusicPlayer.cpp:533-539`）——把回调再排一个「现在执行」的一次性任务到**同一个队列**。
- `asyncRunTask` 把 `runTimePointUs` 设为 `Timer::getCurrentTimePoint()`（`sdk/cosmos/WorkQueue.hpp:100`）；队列是 `std::priority_queue`（`:41-42`），比较器 `CompareTasks` 只比较 `runTimePointUs`（`:35-40`），**对相等时刻不保证 FIFO**（`std::priority_queue` 不保证等价元素稳定序）。
- 同刻还有其它 `asyncRunTask`：7 个控制命令（`:175-237`）、EOS 切歌（`:553`）、两处自动跳轨（`:606`、`:706`）。因此 position 回调与这些任务、以及多个 position 任务彼此之间的相对顺序**不确定**。
- `task->func()` 在 `m_taskMtx` 锁外执行（`WorkQueue.hpp:190-196`），执行期间其它线程可继续 `asyncRunTask` 入队；下一轮 `threadLoopFunc` 可能先取到新入队任务 → **插队**。
- 事实结论：position 常规上报名义频率为「每 10 ms 拍 `got>0` 一次」，但因二次入队，实际执行时刻、与其它回调 / 控制任务的先后顺序、以及是否逐拍可见均**无保证**。这不是「position 与其它回调严格有序」的模型。

---

## 3. 错误表

### 3.1 12 段 `ErrorCode` 逐码上报点

公开 `soundbridge::ErrorCode`（`sdk/include/soundbridge/error.h:31-46`）与内部 `sdk::ErrorCode`（`sdk/ErrorCode.hpp:32-45`）取值一一对应，共 14 个枚举值 = `Ok(0)` + **12 个实际错误码** + `Unknown(0x7fffffff)`；`0x030002` 缺号（`DataTruncated=0x030003` 紧接 `DecodeInitFailed=0x030001`）。`GetErrorInfo`（`sdk/ErrorCode.cpp:19-90`）对 12 码逐个返回静态 `ErrorCodeInfo`（`:26-85`），`Unknown/default` 返回 `kUnknownInfo`（`:86-88`）。

| 码 | 值 | 生产链上报点 | 证据 | 判定 |
|---|---|---|---|---|
| `FileOpenFailed` | 0x010001 | `MusicPlayList::_addMusic` 两处 `reportError` → 过滤器 → `onMusicPlayListError` → `onError` | `sdk/MusicPlayList.cpp:260-261,265-266`；`:448-465`；`sdk/MusicPlayer.cpp:702` | 有 |
| `ExtractorUnsupported` | 0x020001 | `createExtractor==nullptr` 且扩展名未注册 → `reportError` | `sdk/MusicPlayList.cpp:272-283`（码选择 `:277-278`） | 有 |
| `ExtractorInitFailed` | 0x020002 | 扩展名已注册但工厂拒绝；`replacePlaybackTask` 中 extractor 为空 | `sdk/MusicPlayList.cpp:277-282,398` | 有 |
| `DecodeInitFailed` | 0x030001 | `startCurrentTask()` 失败直接 `onMusicPlayListError`；解码器内大量 `setError` 经 `handleDecodeError` → `onError` | `sdk/MusicPlayer.cpp:295-299`；`sdk/audio/decode/AudioStreamDecoder.cpp:126,163,488,508,516,522,594,600,792,955`；`sdk/MusicPlayer.cpp:590` | 有（面最广） |
| `DataTruncated` | 0x030003 | 解码器 EOS 汇总 `setError` → `handleDecodeError` → `onError` | `sdk/audio/decode/AudioStreamDecoder.cpp:339,373` | 有 |
| `DataCorrupted` | 0x030004 | 解码器多处 `setError` → `handleDecodeError` → `onError` | `sdk/audio/decode/AudioStreamDecoder.cpp:334,358,555,577,623,643,710,739,985,1020,1041` | 有 |
| `ResampleInitFailed` | 0x040001 | **无** | 仅 `sdk/ErrorCode.hpp:39`、`sdk/include/soundbridge/error.h:39`、`sdk/ErrorCode.cpp:56-60`（表项）、`sdk/test/unit/test_error.cpp:70`（断言） | **零上报点（确认）** |
| `ResampleFailed` | 0x040002 | **无** | 仅 `ErrorCode.hpp:40`、`error.h:40`、`ErrorCode.cpp:61-65`、`test_error.cpp:79` | **零上报点（确认）** |
| `PlaylistEmpty` | 0x050001 | **无** | 仅 `ErrorCode.hpp:41`、`error.h:41`、`ErrorCode.cpp:66-70`、`test_error.cpp:88` | **零上报点（确认）** |
| `PlayerNoCurrent` | 0x060001 | `_play()` 中 `m_curMusicProperties==nullptr` 直接 `onMusicPlayerError` | `sdk/MusicPlayer.cpp:284-289`（`:287`） | 有（唯一直接 listener 调用） |
| `AudioDeviceOpenFailed` | 0x070001 | `_play()` `open()` 失败 → `onMusicPlayListError` | `sdk/MusicPlayer.cpp:303-306`（`:304`） | 有 |
| `AudioDeviceStartFailed` | 0x070002 | `_play()` `start()` 失败 → `onMusicPlayListError` | `sdk/MusicPlayer.cpp:308-311`（`:309`） | 有 |

补充（交叉验证）：
- `AudioStreamDecoder.cpp:163` 与 `:792` 的失败文案是 `"resample init failed"`，但上报码是 **`DecodeInitFailed`**（0x030001，Decode 模块），不是 `ResampleInitFailed`。
- `MusicPlayList::replacePlaybackTask`（`:415-419`）在 `decoder->start()` 失败时只 `decoder->stop(); return false;`，**不 reportError**；失败若走到 `_play` 由 `MusicPlayer.cpp:295-299` 以 `DecodeInitFailed("failed to start playback task")` 上报。
- `PlaylistEmpty` 对应场景（空列表）在 `MusicPlayList::_updateList`（`:339-344`）是「size==0 就不发 `updateMusicList`」，无任何 error 上报。
- app `errorReason`（`app/playercontroller.cpp:18-36`）只显式处理 6 个码，其余（含上述三码）走 `default:` → `GetErrorInfo(code).message`（`:33-34`）。

### 3.2 两条错误路径 state / error 先后不一致

| 路径 | 实际顺序 | 证据 |
|---|---|---|
| 播放列表错误 `onMusicPlayListError`（index>=0） | `updatePlayState(Stopped)` → `onError` →（若 `shouldAutoSkipOnError()`）`asyncRunTask{ skipToNextPlayable() ? _play() : 仅日志 }` | `sdk/MusicPlayer.cpp:700`（state）、`:702`（error）、`:704-716`（skip）。skip 成功经 `_play():314` 再发 Playing；**skip 失败只写 warning 日志 `:713-714`，不发第二个 state**，状态停在 Stopped |
| 解码错误 `handleDecodeError` | `onError` → 不可跳时 `updatePlayState(Paused)`；可跳时 `asyncRunTask{ skipToNextPlayable() ? _play() : updatePlayState(Paused) }` | `sdk/MusicPlayer.cpp:590`（error）、`:599-603`（`!canAutoSkip` → Paused `:601`）、`:605-614`（skip；失败 → Paused `:612`，成功 → `_play():314` Playing） |
| 扫描汇总 `onMusicPlayListError`（index<0） | 只 `onError`，不改状态 | `sdk/MusicPlayer.cpp:680-689`（`:687`）；来源 `MusicPlayList::reportScanSummary :223-245` |

事实结论：播放列表错误 **state 先 / error 后**；解码错误 **error 先 / state 后**；且解码路径的 skip 失败分支额外发一次 Paused（`:612`）。两条路径的「第二个 state」都不是必然出现。

### 3.3 `reportError` 过滤器与停机期静默

- 过滤器：`MusicPlayList::reportError`（`:448-465`）`bool shouldNotify = (m_selectMusicProperties == nullptr || musicProperties == m_selectMusicProperties);`（`:460-461`），只有「报错文件是当前选中曲目 / 尚无选中曲目」才 `onMusicPlayListError`（`:462-464`）。非当前曲目的扫描错误被过滤，但仍由 `recordScanRejection`（`:217-221`）记账，最终 `reportScanSummary`（`:223-245`）以 `index=-1` 汇总上报一条（按码分桶，detail 形如 `"N files cannot be played"`）。
- 停机期静默：`updatePlayState :722-724`、`putMusicPlayListCurBuf :638,650`、`updateMusicList :660`、`onMusicPlayListError :677` 均在 `isShuttingDown()` 时直接返回；析构 `~Impl`（`:128-140`）先置 `m_shuttingDown=true`（`:130`）再 `m_workQueue.shutdown()`（`:138`）；`WorkQueue::asyncRunTask/syncRunTask` 在 `!m_running` 时静默返回（`WorkQueue.hpp:93-95,116-118`）。

### 3.4 自动跳轨判据（两处）

- `handleDecodeError`：`(info.action == ErrorAction::SkipTrack) && shouldAutoSkipOnError()`（`sdk/MusicPlayer.cpp:580`）。
- `onMusicPlayListError`：仅 `shouldAutoSkipOnError()`（`:704`）。
- `shouldAutoSkipOnError()`（`:729-743`）是**两处共用**的函数：`isShuttingDown` → false（`:731-732`）、`!m_autoSkipOnError` → false（`:734-736`）、`count <= 1` → false（`:737-739`）、`failures < count`（`:741-742`）。因此 `count<=1` / `failures<count` 门槛**对两条路径都生效**（见 §7 C1）。

### 3.5 `traceId` 生成与刷新

- `newTraceId()`（`:745-753`）：`<epoch 毫秒>` + `-` + `<m_traceSeq 自增序号>`，`m_traceSeq` 初值 0（`:102`）。
- 生成：构造 `Impl` 时 `m_traceId = newTraceId()`（`:123`）并 `m_musicList->setTraceId(m_traceId)`（`:124`）。
- 刷新 `bumpTraceId(reason)`（`:755-761`，重新生成 + setTraceId + 日志）：`_play` **仅当** `m_state == StoppedState`（`:276-278`，"play"）、`_next`（`:398`，"next"）、`_previous`（`:419`，"previous"）、`_setCurrentIndex`（`:440`，"setIndex"）。
- **不刷新**：EOS 自动切歌（`advanceToNextTrack` `:555`）、两处自动跳轨异步任务（`:606` / `:706`）、pause / stop。
- 透传：所有 `onError` 都带当前 `m_traceId`（`:288,591,687,702`），adapter 原样转发（`player_impl.cpp:95-100`）到公开 `onError(..., traceId)`（`player.h:33-35`）。`MusicPlayList` 侧另存一份（`setTraceId :114-118` / `currentTraceId :467-471`）供日志用。

### 3.6 `INVALID_OPERATION` 的真实归属

- 定义：`sdk_utils::INVALID_OPERATION = -ENOSYS`，属 `sdk_utils::status_t` 内部负值状态码枚举（`sdk/utils/ErrorUtils.h:18-49`，`:23`），与 `soundbridge::ErrorCode` / `sdk::ErrorCode` 是**两套体系**。
- 生产链用途：解码内部函数返回值（`sdk/audio/decode/AudioStreamDecoder.cpp:127,164`；`sdk/audio/decode/AudioDecodeProcess.cpp:76,87,100,109,116,122,142,151,158,164,189,208,215`；`sdk/extractor/ExtractorHelper.hpp:146`），测试大量断言（`test_decode.cpp:692,708,724,740,875,892`、`test_stream_decoder.cpp:134-186`）。
- **不在公开 `ErrorCode` 里、不经 `onError`、不出现在 `Player` 公开 API**（`sdk/include/soundbridge/player.h:38-74` 无此符号）。
- 与规格的关系：spec §4.1（`doc/streaming-playback-spec.md:61`）与 §6（`:122`）要求「`durationMs == 0` → 显式返回现有 `INVALID_OPERATION`（`pos == 0` 例外）」——这是**规格（目标态）**，文件自述「本文件是规格，不是实现」（`:6`）；当前实现见 §4.3。

---

## 4. 状态表

### 4.1 三套状态与隐式判据

| 状态 | 取值 | 定义 | 写入口 | 读取口 |
|---|---|---|---|---|
| `soundbridge::PlayerState`（公开） | `Playing/Paused/Stopped`（3） | `sdk/include/soundbridge/player.h:11` | 由 adapter 从 `MusicPlayerState` 映射（`player_impl.cpp:19-30`），不落盘 | `Player::state()`（`player_impl.cpp:165-177`，直查 `m_player->state()`） |
| `sdk::MusicPlayerState`（内部权威） | `PlayingState/PausedState/StoppedState`（3） | `sdk/MusicPlayer.h:22-26` | `std::atomic<MusicPlayerState> m_state`（`MusicPlayer.cpp:79`），构造置 Stopped（`:100`），**唯一运行期写入口** `updatePlayState` → `m_state.store`（`:725`） | `state()`（`:202-205`）；`_play` 条件（`:276`） |
| `sdk::StreamDecoderState`（播放任务 / 诊断） | `IDLE/DECODING/SEEKING/EOS/ERROR`（5） | `sdk/audio/decode/AudioStreamDecoder.h:30` | `AudioStreamDecoder` 自身状态机维护 | `shouldFillSilence`（`MusicPlayer.cpp:467`）、`consumeAudioData`（`:542-543,551`）、`startCurrentTask`（`MusicPlayList.cpp:362-365`） |

- 公开 `PlayerState` 与内部 `MusicPlayerState` 均**三态**，无 Error / Switching / Buffering 态。adapter 的 `switch`（`player_impl.cpp:19-30`）只有 3 个 case、**无 default**；`Player::state()`（`:165-177`）同理 3 case + 兜底 `Stopped`。
- 隐式判据（均非枚举态，参与分支）：`m_switching`（`MusicPlayer.cpp:88`，置 / 清于 `:399,420,441,552,559,605,608,649,691` 等）、`PlaybackTask` 存在性（`m_musicList->currentTask()`，`:373,462,496,508,567`）、`m_failedIndices`（`MusicPlayList.cpp:197-215`）、`m_skipFailures`（`MusicPlayer.cpp:87,643,705`）、`m_decodeErrorHandled`（`:91,275,572,644`）、`m_shuttingDown`（`:89,130,763-766`）。
- `shouldFillSilence()`（`MusicPlayer.cpp:457-480`）的 5 条判据：`isShuttingDown()` 或 `m_switching`（`:459`）、`currentTask()==nullptr` 或 ring/decoder 为空（`:462-466`）、`decoder->state()==SEEKING`（`:467-469`）、`cur==nullptr` 或 `task->trackIndex != cur->index`（`:470-478`）。**完全不读 `m_state`**（实测确认地图快照）。

### 4.2 真相源不止一处（实测）

| 源 | 内容 | 证据 |
|---|---|---|
| `Player::state()` | 实时查 `m_player->state()`（atomic `m_state`），不经回调、不经缓存 | `player_impl.cpp:165-177`；`MusicPlayer.cpp:202-205` |
| `onStateChanged` 事件 | `updatePlayState` 里 `m_state.store` **之后**同步发（`:725`→`:726`） | `MusicPlayer.cpp:720-727` |
| `PlayerCallbacksAdapter` 自缓存 | `m_duration/m_position/m_currentTrackName/m_currentIndex/m_trackNames`，`m_mutex` 保护，**在回调触发时写入** | `player_impl.cpp:121-128`；写入 `:39-42,52-53,63-64,83-89` |
| `Player::duration()/position()/currentTrackName()` | 读**adapter 缓存**（「最后一次回调携带的值」），不是 `MusicPlayer` 内部状态 | `player_impl.cpp:179-183,290-303` |
| `PlayerViewState`（app） | 由六个回调写入的结构体 | `app/playercontroller.h:8-22`；写 `app/playercontroller.cpp:168-228` |

事实结论：`state()` 与 `duration()/position()` 走**不同路径**——前者实时查内部 atomic，后者读回调缓存；缓存只在对应回调触发时更新（position 初值来自 `putMusicPlayListCurBuf:655`、seek 来自 `:392`、常规来自二次入队的 `:536`）。

### 4.3 `durationMs == 0` 三处口径

| 处 | 实测 | 证据 |
|---|---|---|
| 规格 §6 | 要求「`durationMs == 0` → 显式返回 `INVALID_OPERATION`（`pos==0` 例外）」；§4.1 seek 行同款；§1 / §10.4 又把「`durationMs==0` 的显示与 seek 禁用」划界外（见 §6 D3/D5） | `doc/streaming-playback-spec.md:122`、`:61`、`:15`、`:211` |
| SDK seek 实现 | `MusicPlayer::Impl::_setPosition`：`if (pos > cur->signalProperties.durationMs) return;` → durationMs==0 且 pos>0 时**静默丢弃**（无 onError、无返回值、无 INVALID_OPERATION）；pos==0 时 `0>0` 为假，继续 `seekToMs(0)` + `onMusicPlayerPositionChanged(0)` | `sdk/MusicPlayer.cpp:384-386`、`:390-392`；入口链 `Player::seek`→`player_impl.cpp:260-263`→`MusicPlayer.cpp:207-214`→`_setPosition` |
| app | `mDurationSlider->setRange(0, durationMs / 1000)`，durationMs==0 → `setRange(0,0)`；无显式「禁用 seek」分支，`durationSliderReleased()` 无条件调 `seekSeconds` | `app/mainwindow.cpp:699-700`、`:571-576`；`formatTime(0)="00:00"`（`playercontroller.cpp:4-14`） |

补充：`AudioStreamDecoder.cpp:351` 注释「`durationMs == 0` 时」指 EOS 汇总 `haveNominal=false` 不报 `DataTruncated`（spec `:124`）；`sdk/test/unit/test_playback_position.cpp:199-254` 有 "unknown duration keeps position on the consumed pcm track" 用例覆盖。

### 4.4 `PlayerController::m_viewState` 跨线程读写与 Qt AutoConnection 实际效果

- 写线程：六个回调 + `publishViewState()` 在 **WorkQueue 线程**（同步转发链，见 §1/§2），直接写 `m_viewState`（`app/playercontroller.cpp:170,197,203,213,221,232-241`）与 `m_currentTrackIndex`（`:190,234`）；`PlayerViewState m_viewState`（`app/playercontroller.h:70`）**无 mutex**。
- 读线程：Qt GUI 线程。两条路径：
  1. **主路径（值拷贝）**：`publishViewState()` 末尾 `emit viewStateChanged(m_viewState)`（`playercontroller.cpp:242`），`connect(mController.get(), SIGNAL(viewStateChanged(PlayerViewState)), this, SLOT(renderPlayerViewState(PlayerViewState)))`（`app/mainwindow.cpp:141-142`）用字符串宏、**未指定连接类型 → 默认 `Qt::AutoConnection`**（`errorOccurred` 同理 `:143-144`）。因 emit 发生在 WorkQueue 线程、receiver `MainWindow` 依附 GUI 线程，按 Qt 官方 `Qt::AutoConnection` 语义（**连接类型在信号发射时按线程关系判定**）解析为 **`Qt::QueuedConnection`**：`PlayerViewState` 被**按值拷贝**进 GUI 事件队列，`renderPlayerViewState(PlayerViewState viewState)`（`mainwindow.cpp:618-708`）读的是**拷贝**，不是 `m_viewState` 成员。跨线程 queued 传递自定义类型依赖 `Q_DECLARE_METATYPE(PlayerViewState)`（`playercontroller.h:24`）+ `qRegisterMetaType<PlayerViewState>("PlayerViewState")`（`playercontroller.cpp:66`，构造期注册、早于 connect）；否则 Qt 报 `Cannot queue arguments of type 'PlayerViewState'`。
  2. **直接 getter（无锁跨线程读）**：`viewState()` 返回 `const PlayerViewState &`（`playercontroller.cpp:163-166`）、`currentTrackIndex()`（`:158-161`）返回值；`mainwindow.cpp:148` 在**构造期**同步 `renderPlayerViewState(mController->viewState())` 一次，`:557` 在 UI 事件里读 `currentTrackIndex()`。这些与 WorkQueue 线程的写构成潜在数据竞争。
- 事实结论：信号路径靠值拷贝规避了成员竞争；但 `m_viewState` / `m_currentTrackIndex` / `m_errorSubtitleActive`（`playercontroller.h:68-70`）本身无锁，getter 路径的跨线程读写竞争真实存在。见 §7 C4 对地图措辞的精确化。
- 外部佐证：Qt 5.15 `Qt::ConnectionType` 官方文档 `https://doc.qt.io/qt-5/qt.html#ConnectionType-enum` —— `Qt::AutoConnection`（默认）在 emit 时判定；receiver 不活在发射线程时用 `QueuedConnection`；queued 自定义参数须在 connect 前 `qRegisterMetaType()`。

---

## 5. 断言表

判据与数字，**分层**（公开 API 回调层 / 播放任务解码层）。

### 5.1 公开 API 回调层 `sdk/test/TestSdkSuite.cpp:408-620`

| 判据 | 数字 / 表达式 | 证据 |
|---|---|---|
| 无当前曲目 play 必须报错 | `wait_for_condition(error_count>0, 3000)` | `:427-429`（内部 listener）、`:454-456`（公开层） |
| `onError` 带 traceId | `!traceId.empty()` | `:430-435,457-462` |
| playbackMode 存取 | Sequential / SingleOnce / SingleLoop / Random | `:416-426,443-453` |
| 加载后 getter | `trackCount()>0`(3s)、`currentTrackName()` 非空(3s)、`duration()>0`(3s)、`position()==0` | `:467-474` |
| 切歌后回调 / getter | `last_track_index==1`、`currentTrackName()` 与回调 playlist 一致、`duration()>0`、`position()==0` | `:477-492` |
| duration 回调恰好 1 次 | `duration_count == 1` | `:527-528` |
| duration 回调 == getter | `last_duration == player.duration()` | `:529-530` |
| position 回调次数 | `position_samples >= 2`（3s），另 `callbackPosition > 0` | `:517-519,524-526` |
| 采样等待 | `sleep_for(1200ms)` | `:520` |
| **position getter 与回调一致** | `apiPosition >= callbackPosition && apiPosition - callbackPosition <= 50`（ms） | `:531-532` |
| 状态序列 | 首项 `Playing`、末项 `Stopped`、**无相邻重复**（循环 `:582-586`）；4 次 `state()` wait 各 3000ms | `:540-588`（`:577,579,587`） |
| playlist 回调覆盖 | `playlist_change_count>0`、`track_names.size()==trackCount()`、索引 `0..n-1` 连续 | `:590-625` |
| stop 后状态 | `state()==Stopped`(3s) | `:534-537` |

### 5.2 公开 API 回调层（unit / doctest，`sdk/test/unit/test_player_integration.cpp` #72 P2-10/11/12）

与 5.1 一一对应、数字一致（同一组判据的 doctest 版）：

| 判据 | 数字 | 证据 |
|---|---|---|
| duration 回调 | `duration_change_count > 0`(3s)、`== 1`（「每曲只应报一次」） | `:1161,1185-1187` |
| position 回调 | `position_change_count >= 2`（3s，文案「1.2s 内」）、`last_position > 0`、`sleep 1200ms` | `:1173-1175,1183-1184` |
| getter 与回调 | `apiPosition >= callbackPosition`、`apiPosition - callbackPosition <= 50` | `:1190-1194` |
| getter 与 duration | `last_duration == player.duration()` | `:1188-1189` |
| 状态序列 | 无相邻重复、首 Playing、末 Stopped、4 次 wait(3000) | `:1199-1255`（`:1236-1254`） |
| playlist 覆盖 | `trackCount()==3`(5s)、`playlist_change_count>0`(3s)、`snapshot.size()==trackCount`、索引连续 | `:1258-1303` |
| 计数载体 | `MockPlayerCallbacks` 的 6 个 atomic 计数 + `stateSequence()` | `sdk/test/mocks/PlayerMocks.hpp:13-21,53,59,73-102` |

其它 unit：`sdk/test/unit/test_player.cpp:13-148` —— `state_change_count` / `track_change_count` / `error_count` 初值 0、reset 归零、play 无曲目报错。

### 5.3 播放任务 / 解码层（非公开回调）

| 判据 | 数字 | 证据 |
|---|---|---|
| position 从 0 起算、单调不回退 | `position == step*25`；25 / 500 / 1900 / 2000 / 2025 ms | `sdk/test/unit/test_playback_position.cpp:100-194` |
| seek / 停机窗口 | 停机期 seek 不把解码器拉进 SEEKING、位置保持 | `sdk/test/unit/test_playback_lifecycle.cpp:313-327` |
| unknown duration | `haveNominal=false`，位置随消费 PCM 推进、不报 `DataTruncated` | `sdk/test/unit/test_playback_position.cpp:199-254` |
| ErrorCode 查表 | 12 码逐个 `GetErrorInfo`；`DataTruncated==0x030003`、`DataCorrupted==0x030004` | `sdk/test/unit/test_error.cpp:16-117`（`:58,67`） |
| 坏输入错误码 | `errors.size()==1` + `ExtractorInitFailed` / `ExtractorUnsupported` | `sdk/test/unit/test_bad_input.cpp:1360-1380` |

### 5.4 回归基线（文档口径，供引用）

- 全量 `492 用例 / 50078 断言 / 0 failed / exit 0`：`doc/streaming-playback-spec.md:255`（并注明 Linux `sdk-coverage` 本机未验证、CI 观测未在真实 CI 验证）。
- 14 个 `sdk_*` CTest 目标 / 分组：`doc/streaming-playback-spec.md:152`（7 组 `TestSdkSuite` + 7 组 `UnitTests` 分组，本地 CTest 注册）。

---

## 6. 文档偏差清单

| # | 文档 | 声明 | 实测 / 互斥 | 证据 |
|---|---|---|---|---|
| D1 | `doc/SoundBridge_Complete_Analysis.md:158-194` | 「引入专门的音频消费线程」；职责表 `:164` 控制流线程 (WorkQueue) / `:165` 数据流线程 (Dedicated Audio Thread)；`:169`「数据线程根据状态决定写音频、填充静音还是休眠」；`:171` position 由数据线程经 `asyncRunTask` 发回控制线程；架构图 `:177-191` 双 subgraph | 现状只有**一个 WorkQueue 单线程**：10 ms `consumeAudioData` 是同队列 `startTimerTask`；`shouldFillSilence()` 完全不读 `m_state`；position 二次 `asyncRunTask` 是同队列内自投递，不是跨线程回传 | `sdk/cosmos/WorkQueue.hpp:142,196`；`sdk/MusicPlayer.cpp:92,121,622,457-480,534-538` |
| D2 | `doc/开发方向建议.md:115-129` | 「分阶段落地」第 1 步「定义播放器状态机（Idle/Preparing/Playing/Paused/Stopped/Error）」六态，第 2 步统一事件回调语义，第 3/4 步 API 文档与接入示例 | 这是**将来建议**，不是现状描述；现状 `PlayerState` / `MusicPlayerState` 均三态，无 Idle/Preparing/Error 态 | `sdk/include/soundbridge/player.h:11`；`sdk/MusicPlayer.h:22-26`；文档 `:121-129` 自述「建议目标 / 分阶段落地」 |
| D3 | `doc/streaming-playback-spec.md` §1 / §10.4 **vs** §4 / §6 | §1 `:15`「播放器状态机与 app UI（`PlayerState` 三态、`durationMs == 0` 的显示与 seek 禁用、事件回调语义）不在范围内，留给独立地图」；§10.4 `:211` 同款界外声明 | 但 §4.1（`:53-64`）整张状态迁移表在规定状态名，§4.4（`:85-95`）在规定回调 / 控制线程，§6（`:114-124`）在规定 seek 策略含 `durationMs==0` 降级，§5（`:99-112`）在规定静音 / 回调观测 → **正文覆盖了被声明为界外的公开 API 语义** | `doc/streaming-playback-spec.md:15,53-64,85-97,114-124,211` |
| D4 | `doc/streaming-playback-spec.md` §4.1 `:64` vs 同表状态名 | `:64`「基准 = `MusicPlayerState`；`PlayerState` 与 `StreamDecoderState` 只作投影 / 诊断，不新增状态机」 | 表 `:57-61` 使用 `IDLE/STOPPED/PLAYING/PAUSED/SEEKING/SWITCHING/ERROR` 等名，其中 SEEKING / SWITCHING / ERROR 不是 `MusicPlayerState` 取值（仅 3 态），公开 `PlayerState` 无 Error 态；`m_switching` 是独立 atomic 标志而非状态机状态 | `sdk/MusicPlayer.h:22-26`；`sdk/audio/decode/AudioStreamDecoder.h:30`；`sdk/MusicPlayer.cpp:88,457-480` |
| D5 | `doc/streaming-playback-spec.md` §4.1 `:61` / §6 `:122` | 「`durationMs==0` → 显式 `INVALID_OPERATION`（`pos==0` 例外）」 | 实现是 `_setPosition` 静默 `return`，`Player::seek()` 返回 `void`，`INVALID_OPERATION` 是 sdk_utils 内部 status 量、不经 `onError`；且文件自述「本文件是规格，不是实现」（`:6`） | `sdk/MusicPlayer.cpp:384-386,390-392`；`sdk/utils/ErrorUtils.h:23`；`doc/streaming-playback-spec.md:6,61,122` |
| D6 | `doc/streaming-playback-spec.md` §4.4 `:90,92` | 线程约束清单把「10 ms `consumeAudioData`」标为「WorkQueue 消费线程」、控制事件标「WorkQueue 线程」，措辞像两条线程 | 实测二者是**同一个 `WorkQueue` 实例的单线程**（`:92` 与 `:121` 共享，`startTimerTask` 注册 10 ms 任务）；`:68`「控制事件经 WorkQueue 单线程串行」本身正确 | `sdk/MusicPlayer.cpp:92,121,622`；`sdk/cosmos/WorkQueue.hpp:142,196` |

---

## 7. 与地图 #75 Notes「现状快照」的更正点（以实测为准）

> C1–C5 为**实测与地图快照措辞冲突或地图快照不完整**之处；其余条目经实测印证，列于本节末。

- **C1（更正）两处自动跳轨的门槛是共用的，不是「后者独有」。** 地图写「`handleDecodeError` 看 `action == SkipTrack && shouldAutoSkipOnError()`（:580），`onMusicPlayListError` 只看后者（:704），**后者还隐含** `count<=1` / `failures<count` 门槛（:729-743）」。实测 `shouldAutoSkipOnError()`（`sdk/MusicPlayer.cpp:729-743`）是**同一函数**，被 `:580` 与 `:704` **两处都调用**：因此 `count<=1`（`:738-739`）与 `failures<count`（`:741-742`）对**两条路径都生效**。两条路径唯一差别是 `handleDecodeError` 额外要求 `info.action == SkipTrack`（`:580`）。
- **C2（更正）错误路径的「第二个 state」不是必然，且解码路径还有 skip 失败→Paused 分支。** 地图「播放列表错误 state→error→(async)skip→**state**」——实测 skip 成功才经 `_play():314` 发 Playing，skip 失败只写 warning 日志（`:713-714`）、**状态停在 Stopped**；地图「解码错误 error→state 或 error→(async)skip→**play**」——实测可跳分支失败时是 `updatePlayState(Paused)`（`:612`），不是 play。
- **C3（更正）「`Player::seek()` 静默丢弃（MusicPlayer.cpp:384-386）」的行号归属是内部 `_setPosition`。** `Player::seek()` 在 `sdk/player_impl.cpp:260-263`，只做转发、无任何检查；`:384-386` 是 `MusicPlayer::Impl::_setPosition`（WorkQueue 任务体内）的 `pos > durationMs` 静默 return。地图「`Player::seek()` 返回 void」这点成立，但丢弃动作发生在内部任务。
- **C4（精确化）`m_viewState` 的 UI 侧读以信号值拷贝为主，直接 getter 仅构造期 / 交互时偶发。** 地图写「无锁被回调线程与 UI 线程同时读写（`playercontroller.cpp:70` / `mainwindow.cpp:148`）」字面成立，但 UI 运行期实际经 AutoConnection → QueuedConnection 的 `PlayerViewState` **按值拷贝**读（`mainwindow.cpp:141-142,618`），并非持续直接读成员；`mainwindow.cpp:148` 是构造期一次性 `viewState()` 直接读。无锁成员竞争真实存在，但只在 getter 路径（`playercontroller.cpp:163-166,158-161`）。
- **C5（更正）现有断言远不止「≤50ms + 无相邻重复」两条。** 地图「现有断言**只要求** position getter 与回调差 ≤50ms（:531-532）、状态序列无相邻重复（:540-588）」。实测同段还有 `duration_count==1`（`:527`）、`last_duration == duration()`（`:529`）、`position_samples>=2`（`:517,524`）、`apiPosition >= callbackPosition`（`:531`）、`callbackPosition>0`（`:526`）、playlist 覆盖与索引连续（`:601-625`）、状态序列首 Playing / 末 Stopped（`:577,579`）、traceId 非空（`:435,462`）、各 wait 3000 / 5000ms 与 sleep 1200ms；unit 侧对应 `sdk/test/unit/test_player_integration.cpp:1161-1303`（§5.1 / §5.2）。

实测**印证**地图快照（非冲突，供引用）：六回调确在 WorkQueue 单线程同步发出、adapter 不 marshal（§1/§2）；`PlaylistEmpty` / `ResampleInitFailed` / `ResampleFailed` **确认零上报点**（§3.1）；`shouldFillSilence()` 完全不读 `m_state`（§4.1）；spec `:15 / :64 / :122 / :211 / :255` 与地图标注行号逐一吻合（§6）；`INVALID_OPERATION` 确为 sdk_utils 内部量、不在公开 `ErrorCode`、不经 `onError`（§3.6）；`m_state` 唯一写入口 `updatePlayState()`（§4.1）；`traceId` = 毫秒时间戳-自增序号（§3.5）。

---

## 8. 未决 / 边界声明

- 本票**不做任何设计决策**：不裁定三态 / 六态、不改 `PlayerCallbacks` 签名、不定义 seek 失败如何表达、不建议是否让 3 个零上报码上线。上述「偏差 / 互斥 / 更正」仅记录事实，供 #75 后续 G 票取用。
- 未验证项（如实登记）：未运行 ctest（票面禁止全套）；Qt 语义取官方文档文字，未在本机跑 GUI 事件循环实测线程切换；`492 / 50078` 为规格文档口径（`doc/streaming-playback-spec.md:255`），非本会话重跑。
- 行号基于 `dc8c12229`；若 master 后续变动需重新定位。
