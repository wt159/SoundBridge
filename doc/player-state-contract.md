# 播放器状态与公开 API 契约规格

> 来源：wayfinder 地图 [#75「地图：播放器状态与公开 API 契约」](https://github.com/wt159/SoundBridge/issues/75)，
> 由 #76–#84 九张票（一张事实票 + 七张契约票 + 一张汇总票）的决策折叠而成。2026-11-07 收口。
>
> **本文件是规格，不是实现。** 代码、头文件注释与用例改动走图外执行票；实现清单见第 10 章。
> 术语以 [`GLOSSARY.md`](../GLOSSARY.md) 为准；难逆转的架构决策见
> [`docs/adr/0004`](../docs/adr/0004-player-state-three-values.md)。
> 事实基线见 [#76](https://github.com/wt159/SoundBridge/issues/76) 与
> [`research/public-api-contract-facts`](https://github.com/wt159/SoundBridge/blob/research/public-api-contract-facts/docs/research/public-api-contract-facts.md)。

## 1. 目的地与范围

把 `soundbridge::Player` / `PlayerCallbacks` 的对外承诺钉成可判定的契约：状态取值、回调线程与顺序、控制命令的可见语义、查询与事件的真相源、错误与状态的先后、标称时长为 0 时 seek 的表达。走到这里，实现用例的人不需要再做任何决定。

- **只含 sdk 公开契约**。`app/` 只作事实来源与验收场景；不重设计 UI、不重构 `PlayerViewState`。
- 错误码体系沿用 [ADR 0001](../docs/adr/0001-error-code-expresses-cause.md)；本规格只裁上报范围与先后，**不新增错误码、不改 `onError` 签名**。
- API 文档 / 时序图 / CLI 接入示例、代码与用例实现都不在图内。
- 沿用 [本地文件边解边播规格](streaming-playback-spec.md) §4.1 / §4.4 / §6 的**不可推翻前提**：状态基准是 `MusicPlayerState`、`PlayerState` 与 `StreamDecoderState` 只作投影 / 诊断、不新增状态机；控制事件在 WorkQueue 单线程串行；`m_abortDecode` 是唯一取消信号；错误码只表达原因。
- [#8 support stream programme](https://github.com/wt159/SoundBridge/issues/8) 保持 OPEN，是上一张图的锚点，与本图无关。

### 1.1 一次性批量授权

2026-11-07 用户授权「一次性完成这些工单，需要我拍板的问题都选择你的推荐」，覆盖 #75 Notes 的「一个会话最多解决一张票」约定。八张契约票各自的 resolution comment 保留完整决策与依据，本文件只做折叠，不代替票里的依据。

## 2. 术语与状态取值（对应「播放状态集合与术语边界」）

### 2.1 术语

- **播放状态**：播放器对外呈现的三种状态之一：正在播放、已暂停、已停止。它只回答「此刻是否在消费音频」，不回答解码进度、错误原因或音频来源。`_Avoid_`：播放任务状态、播放模式、流式状态。
- **播放任务状态**：一次播放任务内部的进度状态（空闲 / 解码中 / 定位中 / 已到末尾 / 出错），只供 SDK 内部判断静默窗口、取消时机与 EOS 汇总，不对外呈现。`_Avoid_`：播放状态、解码状态。

两条已就地写入 `GLOSSARY.md`。「边解边播」是数据供给契约、不是播放模式，不进播放状态（既有词条不变）。

### 2.2 状态取值表

对外**保持三态** `Playing / Paused / Stopped`（`sdk/include/soundbridge/player.h:11`），**不扩到六态**。

| 对外播放状态 | 触发场景 | 内部投影来源 |
|---|---|---|
| Playing | `play()` 成功、从暂停恢复、自动跳轨落到可播曲目 | `MusicPlayerState::PlayingState` |
| Paused | `pause()`、autoSkip 关闭时解码错误停在坏处 | `MusicPlayerState::PausedState` |
| Stopped | 初值、`stop()`、播放列表错误 `index>=0` 且未跳过、停机 | `MusicPlayerState::StoppedState` |

边界场景一律按上表投影，**不新增状态值**：

- 无曲目 / 扫描中 / 准备中 / 停机 → Stopped；「没有曲目」由 `trackCount()==0` 与 `onError(PlayerNoCurrent)` 表达。
- seek 的 SEEKING 静默窗口、EOS 自动跳轨的两曲之间、切歌 `m_switching`、坏文件被自动跳过 → 属于**播放任务状态**，对外状态不变（切歌前后都是 Playing，不闪 Stopped）。
- 错误不进状态面：`onError` 表达原因，播放状态表达处置结果；autoSkip 跳轨成功时可能完全没有 `onStateChanged`。

**为何不采用六态**（`doc/开发方向建议.md:126` 的 Idle/Preparing/Playing/Paused/Stopped/Error）：Idle 与 Stopped 同义；Preparing 是 init 期只读扫描（ADR 0003 已允许，且不改变「是否在消费音频」）；Error 与 `onError` 重复表达。扩到六态要么新增第二套状态机，要么把 `MusicPlayerState` 升格为对外基准，两者都违反「不新增状态机」。完整权衡见 [ADR 0004](../docs/adr/0004-player-state-three-values.md) —— 本图唯一开的一条 ADR。

## 3. 事件回调契约（对应「事件回调契约：线程、顺序、重入与生命周期」）

现状：公开头 `player.h:25-36` 对线程 / 顺序 / 重入零注释；六个回调全在 SDK 内**同一条 WorkQueue 线程同步发出**（#76 实测），「10 ms 消费循环」其实是同队列 `startTimerTask` 定时任务、不是独立线程；`PlayerCallbacksAdapter` 不做 marshal；app 靠 Qt QueuedConnection 按值拷贝跨到 UI 线程。

### 3.1 承诺表

| 维度 | 契约 |
|---|---|
| 线程 | 六个回调都在 SDK 内部**单一串行线程**上同步发出；不在调用者线程、不并发、不重叠。调用方自行 marshal；Qt AutoConnection 属 app 责任，不写进 SDK 契约。 |
| 顺序（曲目切换） | `onTrackChanged → onDurationChanged → onPositionChanged` 固定三连，每曲一次。 |
| 顺序（错误） | **原因先于结果**：`onError → onStateChanged`（仅当状态真的改变）；autoSkip 跳轨成功时可以不发 `onStateChanged`。 |
| 顺序（其他） | `onPlaylistChanged` 先于由该次扫描引发的 `onTrackChanged`；`onStateChanged` 在内部权威状态写入之后立即发。 |
| 频率 | 不承诺固定频率。`onPositionChanged` 的 10 ms / seek 后一次是实现细节；承诺「同一曲目会话内单调不回退、新曲归零」。`onDurationChanged` 每曲恰好一次且与 `duration()` 一致。 |
| 状态事件 | `onStateChanged` 无相邻重复（既有断言升格为契约）。 |
| 重入 | **回调内不得调用任何控制命令，也不得析构 `Player`**；同步转发在单线程上会自锁 / 乱序。回调只记录后返回。 |
| 异常 | 回调不得抛异常；异常跨越 SDK 边界的行为未定义。 |
| 所有权 | `PlayerCallbacks*` 由调用方持有，必须活过 `Player` 析构；停机期（`isShuttingDown`）一律不发回调。 |
| 构造期 | 构造到首次命令之间不主动发回调；查询返回各自初值（Stopped / 0 / 空）。 |

**为何错误顺序选 error→state 而不是 state→error**：autoSkip 开启时状态可能根本不变（解码错误路径是 `error→(async)skip→play`），先发 state 会撒谎；`onError` 是原因、`onStateChanged` 是处置结果，因果顺序也要求原因在先。代价是播放列表错误路径要改现有 `state→error` 顺序，属图外实现项。

### 3.2 落点分工

- `sdk/include/soundbridge/player.h` 注释：线程、重入、异常、所有权、停机期（**建议文本见 §10.2，交图外实现票落地；本图不改代码**）。
- 本文件 §3.1：顺序、频率、状态事件、构造期。
- 不开 ADR：除线程契约外的条目改文档即可反悔；线程契约是现状写实，不是新的架构选择。

### 3.3 与现状的 gap（交实现票，本图不改）

- 两条错误路径顺序不一致（`MusicPlayer.cpp:700,702` vs `:590,601`）→ 统一为 error→state；skip 失败不得静默停在 Stopped 而不给任何通知（保持无相邻重复即可）。
- `position` 二次 `asyncRunTask`（`:534-538`）回投 `priority_queue`、同刻不保证 FIFO → 实现须保证「同曲目会话内单调不回退」，不能让旧值排到新值之后。
- `onStateChanged` 在 `updatePlayState()` 里发且停机期已静默（`:722-724`），符合契约，不用改。

## 4. 控制命令语义（对应「控制命令的可见语义与幂等性」）

| 项 | 决策 |
|---|---|
| 返回值 | **七个控制入口全部保持 `void`**。命令是 `asyncRunTask` 排队，返回时结果尚未生效；加返回值会制造一个假的同步点，还会触发「公开 API 兼容 / 演进策略」这个尚未毕业的 fog。结果经回调与查询可见。 |
| 幂等性 | 重复 `play()` / `pause()` / `stop()` 无副作用、**不重复发 `onStateChanged`**；Stopped 下 `pause()` / `stop()` 幂等（既有断言升格为契约）。 |
| 命令门槛 | 不新增「已接受 / 被丢弃 / 排队中」的可见通道（WorkQueue 串行是不可推翻前提）。规定**被丢弃的命令不产生任何回调、不改变任何查询值**；「排队中」对外不可见。 |
| 四类门槛 | 连续 seek 后者覆盖前者；stop / 切歌窗口 seek 丢弃；stop 期间 `play()` 排队到 stop 完成；停机期一切命令静默丢弃。沿用 `streaming-playback-spec.md` §4.1 的状态迁移表，一字不改。 |
| 无效输入 | `setCurrentTrack()` 非法索引、`seek()` 超界、空目录扫描 → 空操作或既有扫描汇总，不新增错误码；`play()` 无曲目沿用 `onError(PlayerNoCurrent)`。 |
| 播放模式 | 五种 `PlaybackMode` 语义沿用现状映射（`player_impl.cpp:185-221` 的 `SingleOnce → CurrentItemOnce`、`SingleLoop → CurrentItemInLoop`）；`setCurrentTrack` 与自动跳轨冲突时以显式命令为准；播放列表变更后索引以 `onTrackChanged` 为准。只裁契约，不重设计列表逻辑。 |

**fog 收敛**：因不改任何签名，「公开 API 的兼容 / 演进策略」在本 effort 内无需决定，从 Not yet specified 删除；若将来要加返回值或改 `PlayerCallbacks`，须重开一张新 effort 的票，不许在本规格上打补丁。

## 5. 查询与事件的真相源（对应「单一真相源：查询与事件的一致性」）

- **唯一权威**是内部 `MusicPlayerState`（状态）与消费端累计值 `segmentBaseBytes + m_readPos`（位置），标称时长由提取器 init 期给出。对外 `state()` / `duration()` / `position()` 是它们的实时投影；`onStateChanged` 等事件是**变更通知**，允许滞后于查询但不得丢失（停机期除外）。
- `PlayerCallbacksAdapter` 自缓存的 duration / position / trackName / index（`player_impl.cpp:124-128`）只是 getter 的快照，**不是第二真相源**；`app/PlayerViewState` 是 UI 缓存（app 界外）。SDK 侧契约要求：回调按值发出、不共享内部缓存指针，让调用方不必猜。
- 调用方**以查询做决策、以回调刷新视图**；回调滞后时以最新查询为准。
- 一致性窗口：一个曲目会话内，`duration()` 不变（既有 `test_playback_position.cpp:216-226` 断言）、`position()` 单调不回退、新曲归零；切歌瞬间允许「旧曲 position 与新曲 duration 交错」，但 `onTrackChanged` 之后的第一个 `onPositionChanged` 必须属于新曲。
- 既有判据**升格为契约、数值一字不动**：position getter 与回调差 ≤50 ms（`TestSdkSuite.cpp:531-532`）、`onDurationChanged` 每曲一次（`:527`）、getter 与回调一致（`:529-530`）、状态序列无相邻重复（`:540-588`）。不新造容差。
- 事实基线更正：`m_viewState` 在回调线程写、UI 线程读，但 Qt QueuedConnection 按值拷贝 emit，实际竞争只在直接读 getter 的那条路径；属 app 实现细节，SDK 契约不为此增加锁。

## 6. 错误与状态（对应「错误与状态的先后及上报范围」）

### 6.1 先后

统一 **error → state**（理由见 §3.1）。autoSkip 开启且跳轨成功时状态可能不变，此时只有 `onError`；skip 失败时补一次状态事件（遵守无相邻重复）。与 §3.1 的顺序表是同一条契约，不另立规则。

### 6.2 必须上报清单

| 类别 | 错误码 | 说明 |
|---|---|---|
| 无曲目 / 设备 | `PlayerNoCurrent`、`AudioDeviceOpenFailed`、`AudioDeviceStartFailed` | 沿用 `MusicPlayer.cpp:287-309` |
| 解码汇总 | `DecodeInitFailed`、`DataTruncated`、`DataCorrupted`、`Resample*` 等 | 播放期 EOS 汇总（`:576-591`） |
| 扫描 | `FileOpenFailed`、`ExtractorUnsupported`、`ExtractorInitFailed` 及 `index=-1` 批量汇总 | `MusicPlayList.cpp:242,260-283` |

**有码但不承诺上报**：`PlaylistEmpty`、`ResampleInitFailed`、`ResampleFailed` 在 `sdk/ErrorCode.cpp` 有查表项，#76 实测生产链**零上报点**（resample 初始化失败实际报 `DecodeInitFailed`，`AudioStreamDecoder.cpp:163,792`）。本规格把它们明确标为「内部查表项，调用方不得等待其回调」，**不新增上报点、不删码**（错误码体系界外）。

`traceId`：一次播放会话一个 id，`play`（Stopped 时）/ `next` / `previous` / `setCurrentTrack` 刷新（`MusicPlayer.cpp:745-761`）；随 `onError` 透传。`detail` 与 `path` 在批量汇总时可为空；`trackIndex` 在批量汇总时为 `-1`，单曲错误时为具体索引。

### 6.3 自动跳轨口径

契约以 `streaming-playback-spec.md` §4.1 的 **`ErrorAction::SkipTrack`（不看 recoverable）+ `shouldAutoSkipOnError()`** 为准。事实基线更正：`count<=1` / `failures<count` 门槛是 `shouldAutoSkipOnError()` **两处共用**（`:729-743`），并非播放列表错误路径独有；唯一差别是 `handleDecodeError` 额外要求 `action == SkipTrack`。是否把 `action` 检查补到播放列表路径属实现细节，**登记为图外实现项**，本图不改列表逻辑（`doc/bad-input-spec.md` §12 界外）、不改 `m_autoSkipOnError`。

## 7. 标称时长为 0 的场景（对应「标称时长为 0 时的公开契约」）

三处口径互斥（规格 §6 要求显式 `INVALID_OPERATION` / `Player::seek()` 返回 `void` 且静默丢弃 / app 靠 `setRange(0,0)` 隐式退化）裁定如下：

- **保持 `void` 签名，不新增错误码、不改 `onError` 签名**。`INVALID_OPERATION` 实为 `sdk_utils::status_t = -ENOSYS`（#76 实测），是**内部降级码，不对外暴露**；`streaming-playback-spec.md` §6 的措辞按此回填。
- `durationMs == 0 && pos > 0` 时 `seek()` 为**空操作**：不回退 `position()`、不发 `onPositionChanged`、不改状态、不发 `onError`；`pos == 0` 例外保留（归零）。`pos > durationMs` 的静默丢弃与之一致，统一按空操作处理。
- 标称时长为 0 时，`position()` / `onPositionChanged` 仍按已消费 PCM 时间照常推进（ADR 0003）；`duration()` 返回 0 表示**标称时长未知**，SDK 不区分「未知时长」与「零长度素材」。
- UI 从 `duration() == 0` 这一个信号得知不可 seek；**不引入能力位 / `isSeekable()`**。app 作为验收场景，只验证该信号可用，不写 UI 规范。
- **fog 收敛**：「能力查询是否独立成 API」裁定为不需要，从 Not yet specified 删除；「显式拒绝」的判定落在可红可绿的空操作判据上（§8），不是对调用方的返回码。

## 8. 验收判据与回归基线（对应「契约的验收判据与回归基线」）

| 契约 | 判据 | 落点 |
|---|---|---|
| 三态取值与边界场景 | 状态序列、切歌不闪 Stopped、错误不进状态面 | `sdk_player_tests` / `sdk_interaction_tests` |
| 回调三连、error→state、无相邻重复 | 回调顺序记录 + 计数 | `sdk_player_tests`（`PlayerMocks.hpp`） |
| 幂等、无效输入、门槛丢弃 | 不重复发状态、被丢弃命令不改变查询值 | `sdk_unit_tests` / `sdk_interaction_tests` |
| position 单调 / 新曲归零 / getter 与回调 ≤50ms / duration 每曲一次 | 既有断言，**数值一字不动** | `sdk_player_tests` / `test_playback_position.cpp` |
| 必须上报清单、三码不承诺上报 | 错误码 × 回调计数 | `sdk_unit_tests`（`test_error.cpp`） |
| `durationMs==0` 的 seek 空操作 | 位置不回退、无新回调、无状态变化 | `sdk_interaction_tests` |

- **判据不新造**：沿用 `max(1%,150ms)`、`r × good`、5pp、零容差差分与上述既有数值；坏输入 55 例 / 覆盖度 15 行是不可动基线。
- 线程契约用 `promise/future` 或轮询验证状态迁移（`sdk/test/unit/AGENTS.md:34`），不裸 sleep；重入 / 所有权 / ABI 类只写头文件注释 + 本规格，**不做最小可测替身**（回调内发命令会自锁，测不出有效结论）。
- 落点分工：头文件注释 = 线程 / 重入 / 异常 / 所有权；本规格 = 全部契约表；ADR 0004 = 仅「三态」这一条。
- **CI 不动**：14 个 `sdk_*` 目标、TIMEOUT、快集 / 全量集划分、耗时 gate 全部沿用 #52 决策；StreamingBudget 仍是「只记录不设门槛」的先例。新增用例若写文件必须带 pid（`#76` 与 `sdk-windows.yml -j4` 的教训）。

## 9. 与既有文档的接缝

| 文档 | 处置 |
|---|---|
| `doc/streaming-playback-spec.md` §1 / §10.4 | 界外声明「留给独立地图」改为指向本规格 |
| 同文件 §4.1 | `ERROR` / `SWITCHING` 措辞改为「播放任务状态」，不新增状态机 |
| 同文件 §6 | `INVALID_OPERATION` 改为「内部降级码，不对外暴露」，见本规格 §7 |
| `GLOSSARY.md` | 增收「播放状态」「播放任务状态」两条（已就地写入） |
| `doc/开发方向建议.md` 方向六 | 加注：六态设想已裁定为保持三态，见 ADR 0004 |
| `doc/SoundBridge_Complete_Analysis.md` | 「双线程」描述按 #76 实测更正为单 WorkQueue 线程 |
| `doc/bad-input-spec.md` §2.1 / §12、覆盖度矩阵 15 行 | **一字不动** |

## 10. 从决策到实现

### 10.1 实现清单（图外执行票，本图不建票、不落代码）

| 编号 | 内容 |
|---|---|
| C1 | 两条错误路径统一为 error→state；skip 失败补一次状态事件 |
| C2 | `position` 二次 `asyncRunTask` 保证同曲目会话内单调不回退 |
| C3 | `player.h` 加注释：线程 / 重入 / 异常 / 所有权 / 停机期（建议文本见 §10.2） |
| C4 | `durationMs==0` 的 seek 空操作与「无新回调」判据用例 |
| C5 | 播放列表错误路径是否补 `ErrorAction::SkipTrack` 检查（§6.3 登记项） |
| C6 | 回调顺序 / 幂等 / 必须上报清单的补强用例（不改既有数值） |

实现路线沿用 #53 第 10 章：`/to-spec` 折叠 linked decisions → `/to-tickets` 拆执行票 + 原生阻塞边 → `/implement-spec` 在 frontier 上并行 implementer 子 agent、integration branch、一次 `/code-review`。**本图收口时 spec 刚产出，尚无可执行的执行票，故本轮不启动 `/implement-spec`。**

### 10.2 `player.h` 建议注释文本（交 C3 落地，本图不改代码）

```
// 回调在 SDK 内部单一串行线程上同步发出，不在调用者线程、不并发。
// 调用方自行 marshal 到自己的线程。回调内禁止调用 Player 的任何控制命令，
// 禁止析构 Player，禁止抛异常。callbacks 由调用方持有，须活过 Player 析构；
// 停机期间不保证也不发送任何回调。
```

### 10.3 fog 清理与已知代价

- fog「公开 API 的兼容 / 演进策略」→ 因不改签名而闭合，删除（§4）。
- fog「能力查询是否独立成 API」→ 裁定不需要，删除（§7）。
- fog「契约漂移的长期观测口径」→ 不在本图收口，作为后续执行 effort 的输入；若像 #68 的 StreamingBudget 一样只记录不设门槛，须写明「不作为失败门禁」。
- **已知代价**：三态无法区分「准备中」与「停止」，调用方只能靠 `trackCount()` / 回调组合判断；error→state 的顺序调整会让播放列表错误路径的 UI 提示相对状态晚一步，属可接受代价（避免 autoSkip 场景下先发一个不存在的状态）。
- 端到端 drift / 切换开销 / 观测指标判据仍是 #42 残留 fog 与 #68 交棒的另一 effort，与本图无关。
