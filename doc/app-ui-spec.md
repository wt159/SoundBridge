# app 层界面契约

- 状态：定稿（2026-11-21）
- 来源：[地图：app 层 UI 契约](https://github.com/wt159/SoundBridge/issues/216)；决策票 [#217](https://github.com/wt159/SoundBridge/issues/217) / [#218](https://github.com/wt159/SoundBridge/issues/218) / [#219](https://github.com/wt159/SoundBridge/issues/219) / [#220](https://github.com/wt159/SoundBridge/issues/220) / [#221](https://github.com/wt159/SoundBridge/issues/221) / [#222](https://github.com/wt159/SoundBridge/issues/222)；判据票 [#223](https://github.com/wt159/SoundBridge/issues/223)；研究 [#224](https://github.com/wt159/SoundBridge/issues/224)
- 事实基线：`master @ b0543e3ce` + 2026-11-21 实跑截图（1200×720，SDL dummy 设备）
- 关系：SDK 公开契约仍以 [`player-state-contract.md`](player-state-contract.md) 为准；本文只覆盖 `app/`。既有 SDK 规格的既有行一字不动，只在该文件 §1 加一行纯新增指针。
- 术语：**界面状态**（`PlayerViewState` 的值）、**可见状态**（截图/控件属性可观察到的结果）、**判据锚点**（被本文锁定的 `objectName`）、**占位控件**（已存在但功能未实现的控件）。
- 增补（2026-11-21）：三控件转正来自地图 [占位控件转正](https://github.com/wt159/SoundBridge/issues/235) 下的决策票 [#238](https://github.com/wt159/SoundBridge/issues/238) / [#239](https://github.com/wt159/SoundBridge/issues/239) / [#240](https://github.com/wt159/SoundBridge/issues/240) / [#241](https://github.com/wt159/SoundBridge/issues/241) / [#242](https://github.com/wt159/SoundBridge/issues/242) / [#243](https://github.com/wt159/SoundBridge/issues/243) / [#244](https://github.com/wt159/SoundBridge/issues/244) 与研究 [#236](https://github.com/wt159/SoundBridge/issues/236) / [#237](https://github.com/wt159/SoundBridge/issues/237)。

## 1. 边界、真相源与锚点

### 1.1 三层真相源

| 层 | 内容 | 谁写 | 谁读 | 本规格是否锁定 |
|---|---|---|---|---|
| 播放器事实 | 状态、当前曲目、时长、位置、错误码、播放列表 | `soundbridge::Player` 经 `PlayerCallbacks` 发出 | `PlayerController` | 否；以 SDK 规格为准 |
| UI 缓存 | `PlayerViewState` 的全部字段 | 仅 `PlayerController` 的回调实现 | `MainWindow` | 只锁**可见结果**，不锁字段结构 |
| 控件外观 | 按钮勾选、文本、列表行、滑块位置 | 仅 `MainWindow::renderPlayerViewState()` | 人 | 是；见 §2–§8 |
| 本地偏好 | 音量、静音、收藏集合 | `MainWindow`（经 `QSettings` 读写） | `MainWindow` | 是；见 §6.1 与 §8.4–§8.6 |

**注（2026-11-21，地图 [#235](https://github.com/wt159/SoundBridge/issues/235)）**：标题的「三层」指播放器侧的真相源链；本地偏好是并列的第四条，**不在这条链上**——它不进 `PlayerViewState`，也不由回调驱动（裁定见 ADR [0014](../docs/adr/0014-track-identity-and-favorites-ownership.md)）。因此不变量 2 对它不适用：界面写本地偏好是允许的，写播放状态仍然禁止。

三条不变量：

1. `MainWindow` 不直接持有 `soundbridge::Player`，只经 `PlayerController` 下发命令。
2. 界面不得反向写播放状态；控件点击只表达意图，结果以回调为准。
3. 回调在 SDK 单线程发出，跨到 UI 线程靠 Qt 队列连接按值拷贝；`renderPlayerViewState()` 只在 UI 线程执行。

### 1.2 本文锁定与不锁定

- **锁定**：可见结果（文案、勾选、列表标记、滑块位置）、判据锚点、持久化键名、错误文案分档。
- **不锁定**：`PlayerViewState` 的字段增删、`PlayerController` 的内部结构、具体像素与色值、控件实现方式（允许将来换实现，只要可见结果与锚点不变）。
- 字段语义若变化（例如新增字段并改变既有字段含义），必须回改本文；仅新增未被 UI 消费的字段不算破坏契约。

### 1.3 判据锚点

锚点即 `objectName`。**改名 = 破坏契约**；qss 选择器必须使用锚点，不允许指向不存在的名字。

| 锚点 | 类型 | 现状 | 角色 |
|---|---|---|---|
| `mListWidget` | `QListWidget` | 已有 | 曲目列表 |
| `mListMask` | `QWidget` | 已有（高度 0、隐藏） | 列表底部遮罩；不要求可见，但不得拦截鼠标 |
| `mDurationSlider` | `QSlider` | 已有 | 播放进度 |
| `mAutoSkipCheck` | `QCheckBox` | 已有 | 自动跳过开关 |
| `btn_previous` | `QPushButton` | 已有 | 上一首 |
| `btn_play` | `QPushButton` | 已有（checkable） | 播放/暂停 |
| `btn_next` | `QPushButton` | 已有 | 下一首 |
| `btn_favorite` | `QPushButton` | 已有 | 占位（§8） |
| `btn_mode` | `QPushButton` | 已有 | 播放模式 |
| `btn_menu` | `QPushButton` | 已有 | 占位（§8） |
| `btn_volume` | `QPushButton` | 已有 | 占位（§8） |
| `mNowPlayingTitle` | `QLabel` | **待补** | 当前曲目标题 |
| `mNowPlayingSubtitle` | `QLabel` | **待补** | 状态/错误副标题 |
| `mPositionLabel` | `QLabel` | **待补** | 已播放时间 |
| `mDurationLabel` | `QLabel` | **待补** | 总时长 |
| `mVolumePopup` | `QWidget`（`Qt::Popup`） | 新增 | 音量浮层容器 |
| `mVolumeSlider` | `QSlider` | 新增 | 音量 0–100 |
| `btn_mute` | `QPushButton`（checkable） | 新增 | 静音开关 |
| `mPlaylistPopup` | `QWidget`（`Qt::Popup`） | 新增 | 播放列表面板容器 |
| `mPanelList` | `QListWidget` | 新增 | 面板内的只读列表视图 |

**注（2026-11-21，地图 [#235](https://github.com/wt159/SoundBridge/issues/235)）**：`btn_favorite` / `btn_menu` / `btn_volume` 三行的「角色」列保留时点原文（占位），现行角色以 §8.4–§8.6 为准；`mListMask` 行随死代码删除作废，见 §7 更正四。

「待补」由图外执行票补齐；补齐前，依赖这四个锚点的判据标「未满足」。

### 1.4 文案语言

- 界面文案锁定为**英文**：`IDLE`、`NOW PLAYING`、错误文案（§5.2）、`Auto skip on error`、模式名（§4.4）、`Not implemented`。
- 不引入 `tr()` 与语言资源；中文只出现在日志与代码注释。
- 文案大小写与连字符按本文逐字取；改字视为破坏契约。
- **解锁控件文案（2026-11-21，地图 [#235](https://github.com/wt159/SoundBridge/issues/235)）**：`Volume: <n>%`（`<n>` 为 0–100 的十进制、无前导零）、`Volume: muted`、`MUTE`（`btn_mute` 文本）、`Favorite` / `Favorited`（`btn_favorite` 提示）、`Playlist`（`btn_menu` 提示）。`Not implemented` 不再是任何控件的提示文案。

### 1.5 新增控件流程

1. 先在本文登记：锚点名、类型、角色、所属章节与判据。
2. 再改代码，并保证锚点与 qss 选择器一致。
3. 未登记的控件不得出现在界面上（判据 A1 的集合比对）。

## 2. 状态与时间

### 2.1 三态映射

| `PlayerState` | `btn_play` checked | subtitle | 其它 |
|---|---|---|---|
| `Playing` | `true` | `NOW PLAYING` | — |
| `Paused` | `false` | `NOW PLAYING` | — |
| `Stopped` | `false` | `IDLE` | 标题与时长**保留**，不清空 |

- 勾选状态只由最近一次状态回调决定；点击造成的瞬时勾选若与状态不符，在下一次回调时纠正。
- **停止态保留上下文**（裁定）：`Stopped` 不清标题、不清时长；「未在播放」已由 subtitle 的 `IDLE` 表达。代价是停止态会显示「上次那首」，这是有意为之。

### 2.2 错误文案的保留

- 错误 subtitle 一经写入，`onStateChanged` 不得覆盖，直到 `onTrackChanged` 清除。
- 同曲重播是否清除，取决于 SDK 是否发 `onTrackChanged`；本文不作额外承诺，判据只覆盖「换曲后清除」。

### 2.3 标题

- `title = playlist[currentTrackIndex]`（原样字符串）。
- `currentTrackIndex < 0`、`playlist` 为空或索引越界 → `—`（U+2014）。
- 标题不得由 UI 从路径重新拼装或截断；视觉省略由控件负责。

### 2.4 时间与进度条

- 格式：`mm:ss`，`mm` 为**总分钟数**（两位补零，≥100 原样），`ss` 两位补零；**不进位到小时**（3600 s 显示 `60:00`）。
- 时长为 0：两个标签均 `00:00`，滑块范围 `0..0`，位置回调不改变标签。
- 滑块范围：`0..floor(durationMs / 1000)`；滑块值：`floor(positionMs / 1000)`，仅在用户未按住滑块时回写。
- 拖动中：滑块值 = 用户当前位置；时间标签仍显示**播放位置**，不显示拖动目标（裁定：标签是「已播到哪」的读数，拖动预览属界外）。
- `sliderReleased` 时按滑块值 seek（秒粒度，`value × 1000 ms`）；seek 后标签由下一次位置回调更新。
- 位置大于时长（异常数据）：滑块值被控件钳制到最大值，标签仍显示回调原值。

### 2.5 空列表

| 元素 | 期望 |
|---|---|
| 列表 | 空，无占位提示 |
| 标题 | `—` |
| subtitle | `IDLE`（若错误文案保留期则显示错误） |
| 时间 | `00:00` / `00:00`，滑块 `0..0` |
| 播放按钮 | `unchecked` |
| 上一首/下一首/播放 | 点击无可见变化（允许一次状态回调纠正勾选） |

不新增空状态引导文案（界外：§10）。

### 2.6 判据

| 编号 | 输入/前置 | 期望 | 类 |
|---|---|---|---|
| `UI-S1` | 播放 → 暂停 → 停止 | checked `true→false→false`；subtitle `NOW PLAYING→NOW PLAYING→IDLE` | B |
| `UI-S2` | 停止态 | 标题与时长保持上一首的值，不清空 | B |
| `UI-S3` | 制造一次错误后切歌 | 错误 subtitle 在切歌后消失，恢复状态文案 | B |
| `UI-S4` | 时长 65 s 的素材 | 标签 `01:05`；滑块最大值 65 | B |
| `UI-S5` | 时长 0 的素材 | `00:00` / `00:00`，滑块不可拖动出值 | B |
| `UI-S6` | 播放中按住滑块拖到中点不松手 | 滑块停在中点；位置标签仍显示播放位置 | B |
| `UI-S7` | 松手 | seek 到滑块值；随后位置回调更新标签 | B |
| `UI-S8` | 空列表 | 标题 `—`、`IDLE`、`00:00`、播放按钮 unchecked | B |

## 3. 列表与曲目选择

### 3.1 条目文本

- 条目文本 = `TrackInfo.name` 原样（UTF-8 → `QString`）。
- 不解析路径、不取扩展名、不读取元数据；命名归提取器/SDK，UI 不改写。
- 现状形如 `music\48000_fltp_1`，属预期，不是缺陷。

### 3.2 选中态与播放中

- 两个标记**必须同时可区分**：选中（当前选择行）与播放中（`kRolePlaying = true` 的行）。
- 播放中标记至少包含两项可见证据：行背景变化 + 左侧竖条；选中标记由选中态提供。
- 同一行可以同时是选中与播放中，此时两种标记都要可见。
- 切换曲目或重建列表后，旧行的播放中标记必须清除，新行必须设置。

### 3.3 点击语义

- 点击任意行 = `selectTrack(row)`：`stop()` → `setCurrentTrack(row)` → `play()`。
- 点击当前正在播放的行 = 从头重播（裁定：与 `selectTrack` 语义一致，用户可用来重听）。
- 越界索引（`< 0` 或 `>= trackCount()`）由控制器忽略，界面无可见变化。

### 3.4 刷新与滚动

- `onPlaylistChanged`：全量重建（`clear()` + 逐项 `addItem`）；重建期间禁用重绘，结束后恢复并重排。
- `onTrackChanged`：清除全部播放中标记与选择，设置当前行、选中它，并滚动到**居中**。
- 列表重建与曲目变化同时发生时，以一次重绘完成，不出现中间空列表闪烁。

### 3.5 曲目来源

- 启动时扫描 `QCoreApplication::applicationDirPath() + "/../../music"`。
- 目录不存在：列表为空，写一条 Warning 日志；不弹窗、不阻塞启动。
- 扫描顺序即列表顺序；本文不承诺排序规则（归 SDK）。

### 3.6 判据

| 编号 | 输入/前置 | 期望 | 类 |
|---|---|---|---|
| `UI-L1` | 启动，`music` 目录存在 | 列表条目文本与文件名一致（含相对路径形态） | B |
| `UI-L2` | 播放第 2 行 | 第 2 行同时具备选中与播放中标记；第 1 行标记消失 | B |
| `UI-L3` | 点击当前播放行 | 从头重播：位置回到 0 附近，状态保持 `Playing` | B |
| `UI-L4` | 点击列表滚动区外/越界 | 无可见变化 | B |
| `UI-L5` | 播放中切到列表外的曲目（`next`） | 新行居中可见，旧行标记清除 | B |
| `UI-L6` | 删除/改名 `music` 目录后启动 | 空列表 + Warning 日志，无弹窗 | B |
| `UI-L7` | 锚点集合 | `mListWidget` 存在且唯一 | A |

## 4. 传输控制与播放模式

### 4.1 播放/暂停

- 点击语义：`Stopped` / `Paused` → `play()`；`Playing` → `pause()`。
- 按钮 `checked` 只表达「正在播放」，见 §2.1；不表达「用户刚刚点过」。
- 连续快速点击允许中间态闪烁，最终态以最后一次回调为准；不要求去抖。

### 4.2 上一首/下一首

- `trackCount() == 0` 时直接忽略。
- 否则顺序固定：`stop()` → `previous()` / `next()` → `play()`。
- 允许可见序列中短暂出现 `Stopped` / `IDLE`（`stop()` 的必然结果）；本文不承诺「无闪切歌」。
- 判据只看最终态：曲目变化 + `Playing` + 位置从 0 附近开始。

### 4.3 进度条拖动

见 §2.4。补充：任何导致 `sliderReleased` 的交互（拖动后松手、点击滑轨翻页后松手）都按当前值 seek。

### 4.4 播放模式

- 点击 `btn_mode` 按固定顺序循环：`Sequential → Loop → Random → SingleOnce → SingleLoop → Sequential`。
- 五个模式对界面的唯一表达是按钮提示文案：
  - tooltip / statusTip / accessibleDescription 均为 `Playback mode: <Mode>`；
  - `<Mode>` ∈ {`Sequential`, `Loop`, `Random`, `Single Once`, `Single Loop`}。
- 模式变化不改变列表、不改变标题、不改变 subtitle。
- 持久化见 §6.1；读回的非法值回退 `Sequential`。

### 4.5 Auto skip on error

- 默认 `true`；复选框标签 `Auto skip on error`。
- 勾选变化立即下发 SDK（`setAutoSkipOnError`），并发布一次界面状态。
- SDK 回调回写勾选状态时必须抑制信号，避免二次下发。
- 持久化见 §6.1。

### 4.6 判据

| 编号 | 输入/前置 | 期望 | 类 |
|---|---|---|---|
| `UI-T1` | 停止态点播放 | 状态 `Playing`，按钮 checked | B |
| `UI-T2` | 播放中点暂停 | 状态 `Paused`，按钮 unchecked，subtitle 仍 `NOW PLAYING` | B |
| `UI-T3` | 空列表点上一首/下一首/播放 | 无曲目变化、无弹窗 | B |
| `UI-T4` | 播放中连点下一首 3 次 | 最终曲目为 +3；状态 `Playing`；不崩溃 | B |
| `UI-T5` | 点模式按钮 5 次 | 提示文案按 §4.4 顺序回到起点 | B |
| `UI-T6` | 关闭重开 | 模式与 autoSkip 勾选保持 | B |
| `UI-T7` | 勾选 autoSkip 后由 SDK 回写 | 不产生第二次 `setAutoSkipOnError` 调用 | A（代码核对） |

## 5. 错误呈现

### 5.1 两条通道

| `autoSkipOnError` | subtitle | 弹窗 | 日志 |
|---|---|---|---|
| `true` | 更新 | 不弹 | 一条 Warning，含 `trace=` |
| `false` | 更新 | `QMessageBox::warning` | app 不额外写日志 |

两通道互斥；subtitle 无论哪条通道都先更新。

### 5.2 subtitle 文案分档

基础文案（逐字）：

| 错误码 | 文案 |
|---|---|
| `ExtractorUnsupported` | `FORMAT NOT SUPPORTED` |
| `ExtractorInitFailed` | `FILE IS DAMAGED - CANNOT PLAY` |
| `FileOpenFailed` | `FILE CANNOT BE OPENED` |
| `DecodeInitFailed` | `FILE CANNOT BE DECODED` |
| `DataCorrupted` | `FILE IS CORRUPTED` |
| `DataTruncated` | `FILE MAY BE INCOMPLETE` |
| 其它 | `GetErrorInfo(code).message` 的全大写 |

后缀规则（按顺序拼接）：

1. `recoverable == true` 且 `positionMs > 0` → 追加 ` AT mm:ss`（格式同 §2.4）。
2. `action == SkipTrack` → 追加 ` - SKIPPED`（autoSkip 开）或 ` - STOPPED`（autoSkip 关）。
3. 批量扫描汇总（`trackIndex < 0` 且 `detail` 非空）→ **只显示** `detail` 的全大写（例：`3 FILES CANNOT BE PLAYED`），不拼基础文案与后缀。

每个错误码一句独立文案，禁止共用。

### 5.3 保留

见 §2.2：保留到 `onTrackChanged`，状态变化不覆盖。

### 5.4 弹窗内容

标题 `Playback Error`；正文按顺序、逐行：

| 行 | 出现条件 |
|---|---|
| `Message: <FormatError(code, detail)>` | 总是 |
| `Module: <ToString(module)>` | 总是 |
| `Severity: <ToString(severity)>` | 总是 |
| `Action: <ToString(action)>` | 总是 |
| `Index: <trackIndex>` | `trackIndex >= 0` |
| `File: <path>` | `path` 非空 |
| `Trace: <traceId>` | `traceId` 非空 |

正文整体 `trimmed()` 后显示。

### 5.5 判据

| 编号 | 输入/前置 | 期望 | 类 |
|---|---|---|---|
| `UI-E1` | 坏文件 + autoSkip 开 | subtitle 为对应文案 + ` - SKIPPED`；无弹窗；日志有 `trace=` | B |
| `UI-E2` | 坏文件 + autoSkip 关 | 弹窗字段齐全；subtitle 为对应文案 + ` - STOPPED` | B |
| `UI-E3` | 播到 3 s 后触发可恢复错误 | 文案含 ` AT 00:03` | B |
| `UI-E4` | 四个不可恢复错误码 | 文案不含 ` AT ...` | A（代码核对） |
| `UI-E5` | 批量扫描汇总 | subtitle 只有 `N FILES CANNOT BE PLAYED` 形态 | B |
| `UI-E6` | 六个固定文案 | 两两不同，且与 §5.2 逐字一致 | A |
| `UI-E7` | 错误后切歌 | subtitle 恢复状态文案 | B |

## 6. 持久化与启动

### 6.1 QSettings 键表

组织名/应用名：`SoundBridge` / `SoundBridge`（`app/main.cpp:106-107`）。

| 键 | 类型 | 默认 | 合法域 | 非法回退 | 读 | 写 |
|---|---|---|---|---|---|---|
| `window/geometry` | `QByteArray` | 空 | `restoreGeometry` 可接受 | 空 → 按屏幕居中 | 构造期 | 关闭时 |
| `window/maximized` | `bool` | `false` | true/false | `false` | 构造期 | 关闭时 |
| `playback/auto_skip_on_error` | `bool` | `true` | true/false | `true` | 构造期 | 关闭时 |
| `playback/mode` | `int` | `Sequential` | 五个枚举值 | `Sequential`（并回写修正值） | 构造期 | 关闭时 |
| `playback/volume` | `int` | `100` | `0..100` | 钳制到端点（并回写修正值） | 构造期 | 变更后去抖 400 ms，另在关闭时兜底 |
| `playback/muted` | `bool` | `false` | true/false | `false` | 构造期 | 变更即写，另在关闭时兜底 |
| `library/favorites` | `QStringList` | 空 | 任意字符串，空串丢弃 | 丢弃非法项 | 构造期 | 变更即写 |

- 新增键必须登记在本表，且留在 `playback/*` 或 `window/*` 命名区。
- 写入时机唯一：`closeEvent`；不在运行期零散写入（模式回写修正值除外）。

**注（2026-11-21，地图 [#235](https://github.com/wt159/SoundBridge/issues/235)）**：上两条是被本注记部分取代的时点文字，保留原文。现行口径：

1. **命名区扩为三个**：`window/*`（窗口几何）、`playback/*`（播放参数，含音量与静音）、`library/*`（曲库级本地状态，当前只有收藏集合）。收藏不是播放参数——它不随播放变化，且将来可能脱离播放器单独浏览，所以不进 `playback/*`。`app/AGENTS.md` 的「持久化项集中在 `playback/*`」同步改成这三个区。
2. **写入时机分三档**：窗口几何 / 最大化 / 模式 / autoSkip 仍只在 `closeEvent` 写；音量与静音在变更后**去抖 400 ms** 写一次（滑杆拖动每像素一次 `valueChanged`，逐次写会把 `QSettings` 变成同步 IO）；收藏在**每次 toggle 后立即写**（低频、且用户预期「点了就存住」）。四类值都在 `closeEvent` 兜底写一次。
3. **去抖窗口内的崩溃会丢最后一次音量调整**，这是刻意接受的代价；不引入额外的落盘线程或原子写。
4. **不引入 schema 版本键**：收藏仍是 `QStringList`，形态变化时再加 `schema/version`。`QSettings::status()` 只检查并写 Warning 日志，不向用户弹窗（写入失败属环境问题，不是播放错误）。

### 6.2 窗口

- 类型：`Qt::Window`；标题 `SoundBridge`；无菜单栏、无托盘。
- 最小尺寸 `960×540`；默认 `1200×720`。
- **实测（2026-11-21）**：声明的最小尺寸与布局实际所需不一致；小于 `1200×720` 时传输行或时长标签会被裁切，见 §7 更正与票 [#228](https://github.com/wt159/SoundBridge/issues/228)。

**更正（2026-11-21，收口后审计）**：上一行「小于 `1200×720` 时…会被裁切」是从四个采样点外推的过强表述。审计补测客户端 `1100×720`（`MoveWindow` 后 `GetClientRect` 复核为 `1100×720`，DPI 96）**完整无裁切**：传输行、滑块、两个时间标签与三个次级控件均在窗内。已证裁切的三档仍为 `960×540` / `960×600` / `1000×700`；完整与裁切的分界在 `1000×700` 与 `1100×720` 之间，精确定位归票 [#228](https://github.com/wt159/SoundBridge/issues/228)。

**落地（2026-11-21，[#228](https://github.com/wt159/SoundBridge/issues/228)）**：重排布局后，最小尺寸 `960×540` 重新成为可兑现的承诺。四处约束调整：① 两栏容器只锁宽度（`310` / `320`），高度交给布局计算；② 列表最小高度 `265 → 150`（列表本身可滚动，是唯一可压缩项）；③ 左栏底部与右栏底部的 `30 px` 间隔由固定改为可压缩；④ 传输行容器高度 `80 → 90`，与其内容 `H2(20) + 间隔 10 + H3(60)` 一致，并把该容器的布局间距归零（间隔由既有 spacer 提供）。实测 `960×540` / `960×600` / `1000×700` / `1100×720` / `1200×720` 五档均无裁切、无重叠，`960×540` 下列表出现滚动条。
- 首次启动（`geometry` 为空）在可用屏幕区域居中；有保存值时 `restoreGeometry` 优先。
- 保存了最大化状态则恢复最大化。

### 6.3 启动顺序

与应用可观察行为相关的顺序固定为：

1. 初始化日志（`<exe>/log`）；
2. 初始化 CJK 字体（系统字体 → 捆绑字体 → 警告）；
3. 加载 `:/style.qss`；
4. 构造 `MainWindow`：布局 → 窗口策略 → 创建 `Player`/控制器 → 读设置 → 扫描 `music` → 连接信号 → 首次渲染；
5. `show()`。

设置必须在首次渲染前生效；扫描必须在首次渲染前发起（结果可异步到达）。

### 6.4 字体

- 依次尝试：系统 CJK 字体关键字（`CJK`、`WenQuanYi`、`Microsoft YaHei`、`SimHei`、`PingFang`、`Noto Sans SC`、`Source Han Sans`）→ 捆绑 `:/fonts/NotoSansCJK-Regular.ttc` → 警告日志。
- 任何一步失败都不得崩溃；允许文字降级显示。

### 6.5 判据

| 编号 | 输入/前置 | 期望 | 类 |
|---|---|---|---|
| `UI-P1` | 首次启动（清空设置） | 窗口 1200×720，居中 | B |
| `UI-P2` | 改窗口大小、模式、autoSkip 后关闭重开 | 三项均恢复 | B |
| `UI-P3` | 手改 `playback/mode=99` 后启动 | 模式回退 `Sequential`，设置被回写为合法值 | B |
| `UI-P4` | 无 CJK 字体的环境（可模拟） | 加载捆绑字体，不崩溃 | B |
| `UI-P5` | 键名集合 | 与 §6.1 表逐字一致 | A |
| `UI-P6` | 最小尺寸 960×540 | 无控件重叠/裁切，列表可滚动 | B |

## 7. 视觉不可回归约束

本文不写像素与色值，只锁「不可回归」的观感与结构。

| 编号 | 约束 | 判定方法 |
|---|---|---|
| `UI-V1` | 双栏分区：左栏（标题/副标题/列表/开关/传输）；右栏（封面/进度/时间/次级控制） | 截图：两栏同时可见，顺序正确 |
| `UI-V2` | 最小尺寸下无重叠、无裁切；列表出现滚动条 | 缩到 960×540 截图 |
| `UI-V3` | 深色主题：窗口背景为深色，正文与次要文本可读 | 截图目测；不以特定色值为判据 |
| `UI-V4` | 选中、播放中、hover 三种视觉状态可区分 | 同屏存在选中行与播放行时对比截图 |
| `UI-V5` | 按钮图标完整（transport 大图标、次级小图标） | 截图：无空白按钮 |
| `UI-V6` | 样式表选择器只能指向 §1.3 的锚点；子控件必须用 `::handle` / `::sub-page`（双冒号）；不得存在语法拼写错误的选择器 | A（代码核对） |
| `UI-V7` | qrc 中每个 `<file>` 在磁盘存在 | A（文件核对） |
| `UI-V8` | `mListMask` 高度 0、隐藏、对鼠标透明；不要求可见 | A（代码核对） |
| `UI-V9` | 两个浮层打开时不遮挡触发按钮所在行、不越出屏幕可用区域；空间不足时翻转到按钮下方 | 截图 + 960×540 复跑 |
| `UI-V10` | 浮层关闭后键盘焦点回到触发按钮；浮层为 `Qt::Popup`，点外部与 Esc 都可关闭 | A（代码核对）+ B |

现状偏差（2026-11-21）：`style.qss` 的 `#listWidget` / `#listMask` / `#durationSlider` 与锚点不匹配；`QListWidgetlistWidget:item:active` 缺分隔符；`:handle` / `:sub-page` 用了单冒号。均属 `UI-V6` 不满足，由执行票修正（方向：以锚点为准改选择器/删死规则，不改锚点）。

**更正（2026-11-21，落地核对）**：`UI-V2` 原文要求「最小尺寸下无重叠、无裁切」。实测 960×540 / 960×600 / 1000×700 三档均出现裁切（传输行或右侧时长标签不可见），1200×720 才完整。`UI-V2` 的通过条件因此收窄为「默认 1200×720 无裁切」；960×540 的布局重排登记为已知缺口（票 [#228](https://github.com/wt159/SoundBridge/issues/228)）。上表单元格不改。

**更正二（2026-11-21，收口后审计）**：`UI-V8` 与 §1.3 的「`mListMask` 高度 0」不成立。`mListMask` 构造时设了 `setMinimumSize(310, 50)` 与 `setMinimumHeight(50)`；`resizeEvent` 与构造末尾的 `setGeometry(..., 0)` 受最小高度约束，实际高度被钳制为 50。其真实状态是「`hide()` + `WA_TransparentForMouseEvents` + 有效高度 50」；判据实质（不覆盖列表、不拦截鼠标、不参与可见布局）成立，字面「高度 0」作废。`UI-V8` 的核对口径以本条为准。

**更正三（2026-11-21，[#228](https://github.com/wt159/SoundBridge/issues/228) 落地）**：`UI-V2` 的通过条件恢复为原文口径——最小尺寸 `960×540` 下无重叠、无裁切，且列表出现滚动条。此前「收窄为默认 `1200×720` 无裁切」只覆盖缺口期，随本票落地作废；`UI-V2` 的判定列（「缩到 960×540 截图」）也因此重新可用。上表单元格不改。

**更正四（2026-11-21，地图 [#235](https://github.com/wt159/SoundBridge/issues/235) 落地）**：`UI-V8` 作废——`mListMask` 是死对象（构造时几何高度被 `setMinimumHeight(50)` 钳制、`hide()` 后从未 `show()`、无任何消费方），随三控件转正一并删除，`mListMask` 也不再是锚点（§1.3 该行同步作废）。删除理由与替代物见 ADR [0015](../docs/adr/0015-popup-base-and-list-mask-removal.md)：列表底部不再需要遮罩，浮层自行管理边界。上表 `UI-V8` 单元格保留时点原文。

## 8. 占位控件

### 8.1 定义与规则

占位控件 = 已出现在界面上、但功能未实现的控件。规则：

1. 必须可见；
2. 必须 `enabled == false`；
3. 必须 `toolTip == "Not implemented"`；
4. 不得 `checkable`（避免图标悬挂成「已开启」）；
5. 点击零副作用（禁用后不产生信号）。

### 8.2 登记表

| 锚点 | 控件 | 处置 |
|---|---|---|
| `btn_favorite` | 收藏 | 禁用 + 去掉 checkable + tooltip |
| `btn_menu` | 播放列表菜单 | 禁用 + tooltip |
| `btn_volume` | 音量 | 禁用 + tooltip |

现状（2026-11-21）：三者均 `enabled`；`btn_favorite` 为 `checkable` 且点击只切换图标，属 `UI-H1` 不满足；`btn_menu` / `btn_volume` 为空槽。由执行票修正。

功能实现（收藏存储、播放列表面板、音量接口）属界外（§10）；将来实现时先改本节与锚点表。

**转正（2026-11-21，地图 [#235](https://github.com/wt159/SoundBridge/issues/235)）**：上段「属界外」与 §8.2 的「现状」段均为时点文字，保留原文。三个控件已按本节准入流程转正：先扩 §1.3 锚点表（新增 5 个锚点），再改写 §10 的三条界外（见 §10 注记），最后由 §8.4–§8.8 给出可见语义与判据。§8.1 的五条规则对**将来**新增的占位控件继续有效；当前占位清单为空。

### 8.3 判据

| 编号 | 输入/前置 | 期望 | 类 |
|---|---|---|---|
| `UI-H1` | 三个占位控件 | `enabled == false`、`toolTip == "Not implemented"`、`!isCheckable()`（收藏） | A |
| `UI-H2` | 点击三个占位控件 | 无信号、无状态变化、无弹窗 | B |
| `UI-H3` | 锚点集合 | 未登记控件不得出现（与 §1.3 比对） | A |

**注（2026-11-21，地图 [#235](https://github.com/wt159/SoundBridge/issues/235)）**：`UI-H1` 的「三者均禁用」在三控件转正后不成立，`UI-H2` 的「点击无反应」同理。三条判据**退役**，替代判据见 §8.8；上表保留时点原文，不再执行。

### 8.4 收藏

- **键**：`TrackInfo::id`（`trackIds[currentTrackIndex]`），派生自 `fullPath` 的归一化形式（ADR [0014](../docs/adr/0014-track-identity-and-favorites-ownership.md)）。收藏集合是**本地偏好**，不进 `PlayerViewState`、不回调下发。
- **归属**：app 持有。SDK 只提供稳定键，不提供收藏状态。
- **toggle**：点击在「当前曲目 id ∈ 集合」上取反；图标与 `checked` 立即变化，不等任何回调。
- **可见**：`checkable == true`；`checked` → `btn_favorite_yes.png`，`!checked` → `btn_favorite_no.png`（qss `#btn_favorite:checked` 分支已就绪）；提示文案 `Favorite` / `Favorited`。
- **切曲**：`checked` 由新曲目的 id 决定；允许一帧内残留旧值，下一次渲染纠正（同 §2.1 口径）。
- **不可收藏**：当前曲目 id 为空（无当前曲目、或来源无路径）→ `enabled == false`、`checked == false`、提示 `Favorite`。
- **悬挂条目**：文件被删/改名后 id 失效，条目**保留不清理**；同路径文件重新出现即恢复收藏态。不做启动期 GC。
- **不做**：收藏夹列表、按收藏筛选、收藏排序、列表行上的收藏标记。
- **持久化**：`library/favorites`，toggle 后立即写（§6.1）。

### 8.5 音量

- **取值范围**：整数 `0..100`。`0` = 输出全零 PCM；`100` = 增益 1.0，字节原样输出。**不支持 >100**（不做软件放大，因而不需要削波策略）。越界按端点钳制，不报错。
- **静音**：独立布尔，与取值正交。静音**不改** `volume` 值；取消静音恢复到原值。拖动 `mVolumeSlider` 视为「要听」，自动解除静音。
- **生效时机**：SDK 在下一次消费拍起按新值输出；稳态设备队列深度约 256 KiB，因此**改动在已排队 PCM 播完后才可听（≈1.5 s 量级）**。本文**不承诺**毫秒级上限，判据只写「最终一致」（见 §8.8）。规格依据 ADR [0013](../docs/adr/0013-software-volume-gain.md)。
- **生命周期**：跨曲目、跨 `stop`/`play`、跨设备重开（LIVE 重连）保持。增益是会话级设置，不是曲目级状态。
- **交互**：点 `btn_volume` 打开 `mVolumePopup`（不切换静音）；滑杆 `0..100`、单步 1、横向；拖动即改（不等松手）；`btn_mute` 切换静音。
- **可见**：`btn_volume` 提示 `Volume: <n>%`，静音时 `Volume: muted`；`mVolumeSlider` 值等于 `volume`；`btn_mute` 的 `checked` 等于静音态，文本 `MUTE`。
- **不做**：多输出设备选择、系统音量、设备级音量（SDL2 无此能力，见 ADR 0013）、>100 放大。

### 8.6 播放列表面板

- **打开**：点 `btn_menu`。播放列表为空时 `btn_menu` 为 `enabled == false`（提示仍为 `Playlist`），因此**不存在空面板**。
- **内容**：与主列表同源（`viewState.playlist`），条目文本 = `TrackInfo.name` 原样（同 §3.1）；播放中的行用青色前景（`#5edcf3`）+ 加粗标识。不显示序号、时长、收藏标记、失败标记。
- **点击语义**：点任意行 = `selectTrack(row)`（与 §3.3 一致，含"点当前行 = 从头重播"），随后面板关闭。
- **跟随**：面板可见期间 `onTrackChanged` 更新高亮并滚动到居中；主列表的选中态仍由既有渲染路径负责，面板**不反向写**主列表。
- **规模**：打开时全量填充（千曲量级可接受，约数十毫秒）；`setUniformItemSizes(true)`。
- **不做**：增删、重排、清空、搜索、拖放、多选。列表增删需要 SDK 新增公开接口并联动播放状态机、失败集合与随机模式，属界外（§10）。

### 8.7 浮层底座

两个浮层共用 `PopupPanel`（ADR [0015](../docs/adr/0015-popup-base-and-list-mask-removal.md)）：

- **机制**：自绘 `QWidget` + `Qt::Popup`；无窗口装饰、不进任务栏。点外部关闭由 popup grab 提供，Esc 显式处理。
- **定位**：水平居中于触发按钮；垂直在按钮上方 8 px。上方可用空间小于浮层高度时翻转到按钮下方；左右钳制到 `QScreen::availableGeometry()`。
- **尺寸**：音量浮层 `200×56`；面板 `300×260`（可用高度不足时可压到 `120`，列表出滚动条）。
- **关闭**：Esc / 点外部 / 再次点同一按钮 / 面板内点条目。
- **焦点**：打开时浮层取焦点（Esc 与键盘操作需要）；关闭后 `setFocus()` 回到触发按钮。
- **不做**：动画、阴影、圆角之外的装饰、多显示器边界的特殊处理。

### 8.8 判据

| 编号 | 输入/前置 | 期望 | 类 |
|---|---|---|---|
| `UI-F1` | `btn_favorite` | `isCheckable() == true`；qss 存在 `:checked` 与默认两条图标分支 | A |
| `UI-F2` | 收藏当前曲目后关闭重开 | 图标为已收藏态；集合键 `library/favorites` 含该曲目 id | B |
| `UI-F3` | 切到未收藏曲目再切回 | `checked` 依次为 `false` → `true` | B |
| `UI-F4` | 无当前曲目 / id 为空 | `btn_favorite` 禁用且 `unchecked`，点击不写入集合 | A（代码核对） |
| `UI-VOL1` | `setVolume(-5)` / `setVolume(150)` / 构造后默认值 | `0` / `100` / `100` | A（单测） |
| `UI-VOL2` | 增益数学 | S16 半幅样本减半、增益 0 输出全零、块内线性渐变端点正确、非 S16 格式不被误改 | A（单测） |
| `UI-VOL3` | 拖滑杆到 60，关闭重开 | 提示 `Volume: 60%`；重启后滑杆仍 60 且输出音量一致 | B |
| `UI-VOL4` | 点 `btn_mute` 再取消 | 依次无声 / 有声；`volume` 值全程不变 | B |
| `UI-M1` | 空列表 / 非空列表 | `btn_menu` 依次 `enabled == false` / `true` | A（代码核对） |
| `UI-M2` | 打开面板 | 条目数与主列表一致；当前播放行为青色加粗 | B |
| `UI-M3` | 点面板第 3 行 | 播放第 3 首、面板关闭、主列表选中同步 | B |
| `UI-M4` | 面板打开时按下一首 | 高亮跟随到新行并居中 | B |
| `UI-M5` | 两个浮层 | `windowFlags() & Qt::Popup`；有 Esc 分支；关闭后焦点回触发按钮 | A（代码核对） |

音量「最终一致」的取证口径：改值后等待 **2 s** 再判断有无声音变化（覆盖 §8.5 的 ≈1.5 s 上界），不写「≤N 毫秒」这类依赖采样撞窗口的断言。

## 9. 判据与验收

### 9.1 编号与两类判据

- 编号：`UI-<区>-<序号>`；区 = `S`（状态）/ `L`（列表）/ `T`（传输）/ `E`（错误）/ `P`（持久化）/ `V`（视觉）/ `H`（占位）。
- **新增区（2026-11-21，地图 [#235](https://github.com/wt159/SoundBridge/issues/235)）**：`F`（收藏）、`M`（播放列表面板）、`VOL`（音量）。`VOL` 是三字母区，编号写作 `UI-VOL1` 不带连字符，以免与视觉区的 `UI-V1` 混淆。`H` 区自本日起无在册判据（占位清单为空），编号保留不复用。
- **A 类**：可脚本核对（锚点、选择器、资源、常量、键名）；当前不新增自动化测试，核对方式是 `grep` + 人工读代码，证据贴票。
- **B 类**：手工冒烟；按步骤操作，截图/日志取证。

### 9.2 A 类核对清单

| 编号 | 核对对象 | 方法 |
|---|---|---|
| `UI-A1` | 锚点集合 | `grep -n 'setObjectName' app/mainwindow.cpp`，与 §1.3 表逐项比对 |
| `UI-A2` | qss 选择器 | `grep -n '#' app/style.qss`，每个 id 必须在 §1.3 表中；子控件双冒号 |
| `UI-A3` | qrc 资源 | `app/res.qrc` 每个 `<file>` 对应文件存在 |
| `UI-A4` | 模式循环顺序 | 读 `btn_playMode_clicked()` 的 switch，与 §4.4 一致 |
| `UI-A5` | 键名 | 读 `restoreWindowState()` / `persistWindowState()`，与 §6.1 一致 |
| `UI-A6` | 错误文案 | 读 `errorReason()`，六个字符串两两不同且与 §5.2 一致 |
| `UI-A7` | 收藏键 | 读 `mainwindow.cpp` 的收藏分支：键名 `library/favorites`，空 id 不写 |
| `UI-A8` | 音量键与钳制 | 读 `restoreWindowState()` / `persistWindowState()`：`playback/volume` 钳制 `0..100`，`playback/muted` 为布尔 |
| `UI-A9` | 面板可点性 | 读渲染路径：`btn_menu->setEnabled(!playlist.isEmpty())` |
| `UI-A10` | 浮层机制 | 读 `app/popuppanel.cpp`：`Qt::Popup`、Esc 分支、关闭后 `anchor->setFocus()` |
| `UI-A11` | 身份键派生 | `UnitTests -ts=MusicId`：混合分隔符 / `..` / 相对路径 / URL 四类用例全绿 |
| `UI-A12` | 增益数学与音量钳制 | `UnitTests -ts=PcmGain`（半幅 / 零增益 / 渐变端点 / 格式直通）与 `-ts=MusicPlayerVolume`（默认 100、越界钳制、静音不改取值）全绿 |

### 9.3 B 类冒烟清单

前置：`music/` 下有可播放素材；准备一个坏文件（复制任意非音频文件并改成 `.mp3`）用于错误场景。

| 步骤 | 操作 | 期望 | 判据 |
|---|---|---|---|
| 1 | 清空 QSettings 后启动 | 1200×720 居中；列表加载；`IDLE` | `UI-P1`、`UI-L1` |
| 2 | 点播放 → 暂停 → 播放 | 三态与按钮勾选按 §2.1 | `UI-T1`、`UI-T2` |
| 3 | 点列表第 2 行 | 切歌并播放；两种标记正确 | `UI-L2` |
| 4 | 再点同一行 | 从头重播 | `UI-L3` |
| 5 | 拖动进度条到中点并松手 | 拖动中标签不动；松手后 seek | `UI-S6`、`UI-S7` |
| 6 | 连点下一首 3 次 | 曲目 +3，最终 `Playing` | `UI-T4` |
| 7 | 点模式按钮 5 次 | 提示文案按序循环 | `UI-T5` |
| 8 | 取消勾选 autoSkip（关闭），播坏文件 | 弹窗字段齐全，subtitle ` - STOPPED` | `UI-E2` |
| 9 | 勾选 autoSkip（开启），播坏文件 | 无弹窗，` - SKIPPED`，日志含 `trace=` | `UI-E1` |
| 10 | 关闭重开 | 窗口/模式/autoSkip 保持 | `UI-P2`、`UI-T6` |
| 11 | 缩到 960×540，再回到 1200×720 | 960×540 记录裁切现状（已知缺口 #228，不计入通过）；1200×720 无裁切 | `UI-V2`、`UI-P6` |
| 12 | 点三个占位控件 | 无反应、无弹窗 | `UI-H2` |
| 13 | 收藏当前曲目，切到别的曲目再切回 | 收藏态按曲目切换，图标随之变化 | `UI-F2`、`UI-F3` |
| 14 | 点音量按钮，拖滑杆到 0，再回到 60 | 先无声、后有声（每次改值后等 2 s 再判） | `UI-VOL3` |
| 15 | 静音后再取消静音 | 无声 → 有声；提示文案在 `Volume: muted` 与 `Volume: 60%` 之间切换 | `UI-VOL4` |
| 16 | 点播放列表按钮，点面板第 3 行 | 播放第 3 首，面板关闭，主列表选中同步 | `UI-M2`、`UI-M3` |
| 17 | 面板打开时按下一首 | 高亮跟随到新行并居中 | `UI-M4` |

**注记（2026-11-21，地图 [#235](https://github.com/wt159/SoundBridge/issues/235)）**：步骤 12 随 `UI-H1` / `UI-H2` 退役作废，三控件改由步骤 13–17 覆盖；本表行保留时点原文。

**注记（2026-11-21，[#228](https://github.com/wt159/SoundBridge/issues/228) 落地）**：步骤 11 的期望「`960×540` 记录裁切现状（已知缺口 #228，不计入通过）」在缺口关闭后改为「`960×540` 与 `1200×720` 两档均无裁切」。行内文字保留时点原样，实际执行按本注记。

### 9.4 取证

- 截图：`python scripts/ui.py shot --window SoundBridge --out <name>.png`；字符图定位用 `map`；点击用 `click --client X,Y`（先 `--dry-run`）。
- 日志：`<exe>/log/soundbridge_*.log`，错误场景必须贴出含 `trace=` 的行。
- 截图不入库；附在票或 PR 评论里，命名 `ui-<判据编号>-<日期>.png`。

**取证注记（2026-11-21，[#228](https://github.com/wt159/SoundBridge/issues/228)）**：本节的截图与尺寸复核有两类已实证的陷阱，按下面顺序执行。

1. **陈旧位图**：`shot` 走 `PrintWindow`，窗口不在交互桌面或刚 `resize` 时会返回 `resize` 之前的画面（本轮曾据此把已修好的版本判为「仍裁切」）。截图前先 `RedrawWindow(hwnd, NULL, NULL, RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW | RDW_FRAME)` 并等约 0.8 s。
2. **串窗**：窗口定位用 `--pid <pid>`，不要用 `--window SoundBridge`；残留进程会让标题匹配命中旧窗口。
3. **判定依据**：判「是否裁切」优先信进程内几何（`centralWidget()->size()`、各容器 `minimumSizeHint()`、控件 `mapTo`），截图只作辅证。
4. **尺寸口径**：`setMinimumSize(w, h)` 在 Qt/Windows 上按**客户区**解释，外框比声明值大一圈（`960×540` → 外框 `976×579`，DPI 96）；用 `MoveWindow` 设外框时会先被撑到该最小值。
5. **附件通道**：`gh` 无二进制附件上传能力。截图按 `ui-<判据编号>-<日期>.png` 命名后随交付给 owner，票内附可复现命令与几何数字；确需上图时走网页端手工上传。

### 9.5 自动化边界

- 首版**无自动化 UI 测试**，不新增 ctest 目标、不改 CI。
- 研究结论（[#224](https://github.com/wt159/SoundBridge/issues/224)，快照见 [`docs/research/app-ui-test-feasibility.md`](../docs/research/app-ui-test-feasibility.md)）：QtTest + offscreen 本机可用（4 例 / 72 ms），但 `MainWindow` 没有状态注入缝，拆库/加渲染入口是一次 app 层 API 改动，且新增目标违反「14 个 ctest 目标」纪律。
- 毕业条件（三条同时满足才考虑引入）：① 给 `MainWindow` 一个不暴露 `Player` 的只读渲染入口；② CI 实测 Windows `qtbase` 含 `Qt5Test.dll`、Linux 装齐 `libqt5test5`；③ 明确新目标对目标数 / `TIMEOUT` 纪律的例外与预算。

### 9.6 落地状态（2026-11-21）

| 判据 | 现状 | 处置 |
|---|---|---|
| `UI-A1` / `UI-A2` / `UI-H1` / `UI-H3` | **不满足** | 图外执行票（锚点补齐、qss 对齐、占位控件） |
| `UI-A3` / `UI-A4` / `UI-A5` / `UI-A6` / `UI-V7` / `UI-V8` | 满足 | 随 A 类核对复核 |
| `UI-V2` / `UI-P6`（最小尺寸） | **不满足**：960×540 下裁切 | 已知缺口，票 [#228](https://github.com/wt159/SoundBridge/issues/228)（界外） |
| B 类 | 可人工执行 | 按 §9.3 冒烟 |

**落地更新（2026-11-21，PR [#229](https://github.com/wt159/SoundBridge/pull/229) 合并后）**：上表的 `UI-A1` / `UI-A2` / `UI-H1` / `UI-H3` 已由 `8d936c8c5` 补齐并通过机器核对；`UI-V2` / `UI-P6` 仍不满足，缺口见 [#228](https://github.com/wt159/SoundBridge/issues/228)。上表保留时点原样，不改。

**时点文字的范围（2026-11-21，收口后审计）**：上一条只按判据编号记状态，不逐处回改正文。§1.3 锚点表的「待补」列、§7「现状偏差」段、§8.2「现状」段都是执行前的时点文字；三处的现行判定以本节「落地更新」与 §7 更正二为准，不要把它们读成仍未落地。

**落地更新（2026-11-21，[#228](https://github.com/wt159/SoundBridge/issues/228)）**：上表的 `UI-V2` / `UI-P6`（最小尺寸）由「不满足」转为**满足**——`960×540` 与 `1200×720` 两档实测无裁切、无重叠，`960×540` 下列表出现滚动条（`UI-V2` 的滚动条要求）。上表保留时点原样，不改。

**落地更新（2026-11-21，地图 [#235](https://github.com/wt159/SoundBridge/issues/235)）**：`UI-H1` / `UI-H2` / `UI-H3` 退役（§8.3 注记），`UI-V8` 作废（§7 更正四）。新增的 `UI-F*` / `UI-VOL*` / `UI-M*` 共 13 条判据中，A 类 6 条（`UI-F1` / `UI-F4` / `UI-VOL1` / `UI-VOL2` / `UI-M1` / `UI-M5`）由 §9.2 的机器核对与 `MusicId` / `PcmGain` 两个单测套件承担，B 类 7 条进 §9.3 冒烟步骤 13–17。三控件的代码实现走图外执行票，与本规格同批交付。

## 10. 界外

- SDK 公开契约与 `PlayerViewState` 结构调整（含删除 `artist` / `album` 死字段）。
- 收藏功能、播放列表面板、音量控制（含设备音量接口）。
- 打开文件/目录、拖放、曲库扫描策略与排序。
- 列表搜索、元数据编辑、封面加载（当前封面是固定资源）。
- 视觉重设计：像素级规范、配色体系、亮色主题、图标重绘。
- 键盘快捷键、Tab 顺序、屏幕阅读器支持。
- 多语言与 `tr()`。
- 自动化 UI 测试目标与 CI 改动（条件见 §9.5）。
- 空状态引导文案、首次使用引导、关于对话框。
- 打包、安装器、便携版路径策略。

**注（2026-11-21，地图 [#235](https://github.com/wt159/SoundBridge/issues/235)）**：上列条目保留时点原文，其中三条已被本图显式覆盖——

1. **「收藏功能」「播放列表面板」「音量控制」**已转正，范围见 §8.4–§8.6。仍未纳入的部分：收藏夹列表/筛选/排序、面板的增删重排、**设备音量接口**（SDL2 无设备级音量，ADR [0013](../docs/adr/0013-software-volume-gain.md) 裁决不做）。
2. **「SDK 公开契约」被覆盖一次**：`TrackInfo` 新增派生键 `id`（ADR [0014](../docs/adr/0014-track-identity-and-favorites-ownership.md)），`Player` 新增 `setVolume` / `volume` / `setMuted` / `muted` 四个方法（ADR [0013](../docs/adr/0013-software-volume-gain.md)），`SOUNDBRIDGE_ABI_VERSION` 1 → 2。此覆盖是**定向的**：除这两处外，「不在本图内改动 SDK 公开契约」仍然有效。
3. 其余条目（打开文件/目录、拖放、曲库扫描、搜索、元数据、封面、视觉重设计、快捷键、多语言、自动化 UI 测试、空状态、打包）一律不动。

## 11. 变更记录

- 2026-11-21：定稿。地图 [#216](https://github.com/wt159/SoundBridge/issues/216) 收口；规格与 ADR 0012 同时落地。既有 SDK 规格零改动，`player-state-contract.md` 只加一行纯新增指针。
- 2026-11-21（落地核对）：收窄 `UI-V2` 的通过条件为「默认 1200×720 无裁切」，把 960×540 的布局重排登记为已知缺口（[#228](https://github.com/wt159/SoundBridge/issues/228)）；冒烟步骤 11 同步改为记录现状。
- 2026-11-21（执行落地）：锚点、样式表、占位控件由 PR [#229](https://github.com/wt159/SoundBridge/pull/229) 落地；§9.6 追加落地更新，`UI-V2` / `UI-P6` 仍为 #228。
- 2026-11-21（收口后审计）：补测客户端 `1100×720` 推翻 §6.2「小于 `1200×720` 必裁切」的外推，追加更正；`UI-V8` 的「高度 0」与 `setMinimumHeight(50)` 矛盾，追加更正二；§9.6 注明三处时点文字与落地更新的关系。
- 2026-11-21（[#228](https://github.com/wt159/SoundBridge/issues/228) 落地）：重排最小尺寸布局，`960×540` 恢复为可兑现承诺；§6.2 / §7 / §9.3 / §9.6 各追加时点注记，既有表格单元格与正文未改。
- 2026-11-21（地图 [#235](https://github.com/wt159/SoundBridge/issues/235) 转正）：收藏 / 音量 / 播放列表面板三控件转正。§1.1 增「本地偏好」第四条真相源；§1.3 增 5 个锚点并注明 `mListMask` 作废；§1.4 增控件文案；§6.1 增 3 个键并把命名区扩为 `window` / `playback` / `library`；§7 增 `UI-V9` / `UI-V10` 与更正四；§8 增 §8.4–§8.8；§9 增 `F` / `M` / `VOL` 三个编号区、6 条 A 类核对与 5 步冒烟；§10 注明三条界外被覆盖。ADR [0013](../docs/adr/0013-software-volume-gain.md) / [0014](../docs/adr/0014-track-identity-and-favorites-ownership.md) / [0015](../docs/adr/0015-popup-base-and-list-mask-removal.md) 同批落地。
