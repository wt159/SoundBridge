# app 层界面契约

- 状态：定稿（2026-11-21）
- 来源：[地图：app 层 UI 契约](https://github.com/wt159/SoundBridge/issues/216)；决策票 [#217](https://github.com/wt159/SoundBridge/issues/217) / [#218](https://github.com/wt159/SoundBridge/issues/218) / [#219](https://github.com/wt159/SoundBridge/issues/219) / [#220](https://github.com/wt159/SoundBridge/issues/220) / [#221](https://github.com/wt159/SoundBridge/issues/221) / [#222](https://github.com/wt159/SoundBridge/issues/222)；判据票 [#223](https://github.com/wt159/SoundBridge/issues/223)；研究 [#224](https://github.com/wt159/SoundBridge/issues/224)
- 事实基线：`master @ b0543e3ce` + 2026-11-21 实跑截图（1200×720，SDL dummy 设备）
- 关系：SDK 公开契约仍以 [`player-state-contract.md`](player-state-contract.md) 为准；本文只覆盖 `app/`。既有 SDK 规格的既有行一字不动，只在该文件 §1 加一行纯新增指针。
- 术语：**界面状态**（`PlayerViewState` 的值）、**可见状态**（截图/控件属性可观察到的结果）、**判据锚点**（被本文锁定的 `objectName`）、**占位控件**（已存在但功能未实现的控件）。

## 1. 边界、真相源与锚点

### 1.1 三层真相源

| 层 | 内容 | 谁写 | 谁读 | 本规格是否锁定 |
|---|---|---|---|---|
| 播放器事实 | 状态、当前曲目、时长、位置、错误码、播放列表 | `soundbridge::Player` 经 `PlayerCallbacks` 发出 | `PlayerController` | 否；以 SDK 规格为准 |
| UI 缓存 | `PlayerViewState` 的全部字段 | 仅 `PlayerController` 的回调实现 | `MainWindow` | 只锁**可见结果**，不锁字段结构 |
| 控件外观 | 按钮勾选、文本、列表行、滑块位置 | 仅 `MainWindow::renderPlayerViewState()` | 人 | 是；见 §2–§8 |

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

「待补」由图外执行票补齐；补齐前，依赖这四个锚点的判据标「未满足」。

### 1.4 文案语言

- 界面文案锁定为**英文**：`IDLE`、`NOW PLAYING`、错误文案（§5.2）、`Auto skip on error`、模式名（§4.4）、`Not implemented`。
- 不引入 `tr()` 与语言资源；中文只出现在日志与代码注释。
- 文案大小写与连字符按本文逐字取；改字视为破坏契约。

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

- 新增键必须登记在本表，且留在 `playback/*` 或 `window/*` 命名区。
- 写入时机唯一：`closeEvent`；不在运行期零散写入（模式回写修正值除外）。

### 6.2 窗口

- 类型：`Qt::Window`；标题 `SoundBridge`；无菜单栏、无托盘。
- 最小尺寸 `960×540`；默认 `1200×720`。
- **实测（2026-11-21）**：声明的最小尺寸与布局实际所需不一致；小于 `1200×720` 时传输行或时长标签会被裁切，见 §7 更正与票 [#228](https://github.com/wt159/SoundBridge/issues/228)。
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

现状偏差（2026-11-21）：`style.qss` 的 `#listWidget` / `#listMask` / `#durationSlider` 与锚点不匹配；`QListWidgetlistWidget:item:active` 缺分隔符；`:handle` / `:sub-page` 用了单冒号。均属 `UI-V6` 不满足，由执行票修正（方向：以锚点为准改选择器/删死规则，不改锚点）。

**更正（2026-11-21，落地核对）**：`UI-V2` 原文要求「最小尺寸下无重叠、无裁切」。实测 960×540 / 960×600 / 1000×700 三档均出现裁切（传输行或右侧时长标签不可见），1200×720 才完整。`UI-V2` 的通过条件因此收窄为「默认 1200×720 无裁切」；960×540 的布局重排登记为已知缺口（票 [#228](https://github.com/wt159/SoundBridge/issues/228)）。上表单元格不改。

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

### 8.3 判据

| 编号 | 输入/前置 | 期望 | 类 |
|---|---|---|---|
| `UI-H1` | 三个占位控件 | `enabled == false`、`toolTip == "Not implemented"`、`!isCheckable()`（收藏） | A |
| `UI-H2` | 点击三个占位控件 | 无信号、无状态变化、无弹窗 | B |
| `UI-H3` | 锚点集合 | 未登记控件不得出现（与 §1.3 比对） | A |

## 9. 判据与验收

### 9.1 编号与两类判据

- 编号：`UI-<区>-<序号>`；区 = `S`（状态）/ `L`（列表）/ `T`（传输）/ `E`（错误）/ `P`（持久化）/ `V`（视觉）/ `H`（占位）。
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

### 9.4 取证

- 截图：`python scripts/ui.py shot --window SoundBridge --out <name>.png`；字符图定位用 `map`；点击用 `click --client X,Y`（先 `--dry-run`）。
- 日志：`<exe>/log/soundbridge_*.log`，错误场景必须贴出含 `trace=` 的行。
- 截图不入库；附在票或 PR 评论里，命名 `ui-<判据编号>-<日期>.png`。

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

## 11. 变更记录

- 2026-11-21：定稿。地图 [#216](https://github.com/wt159/SoundBridge/issues/216) 收口；规格与 ADR 0012 同时落地。既有 SDK 规格零改动，`player-state-contract.md` 只加一行纯新增指针。
- 2026-11-21（落地核对）：收窄 `UI-V2` 的通过条件为「默认 1200×720 无裁切」，把 960×540 的布局重排登记为已知缺口（[#228](https://github.com/wt159/SoundBridge/issues/228)）；冒烟步骤 11 同步改为记录现状。
- 2026-11-21（执行落地）：锚点、样式表、占位控件由 PR [#229](https://github.com/wt159/SoundBridge/pull/229) 落地；§9.6 追加落地更新，`UI-V2` / `UI-P6` 仍为 #228。
