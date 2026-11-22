# 0016 app 层 UI 自动化采用双注入缝：命令口 + 只读渲染入口

- 状态：已接受
- 日期：2026-11-22
- 来源：[地图：app 层 UI 判据全量自动化](https://github.com/wt159/SoundBridge/issues/248)、[注入缝形态与粒度](https://github.com/wt159/SoundBridge/issues/250)、[最小可跑骨架](https://github.com/wt159/SoundBridge/issues/253)。研究快照 [`docs/research/app-ui-test-feasibility.md`](../research/app-ui-test-feasibility.md)（时点，行号已漂移）与 [`docs/research/app-ui-test-ci-capability.md`](../research/app-ui-test-ci-capability.md)（#249）。规格落点 [`doc/app-ui-spec.md`](../../doc/app-ui-spec.md) §9.7.1。

## 背景

`MainWindow` 私有持有 `std::unique_ptr<PlayerController>`（`app/mainwindow.h:52`），`PlayerController` 构造即建 `soundbridge::Player`；状态经 `viewStateChanged` → `renderPlayerViewState`（`private slots`，`app/mainwindow.h:163`）单向渲染。地图 #228 曾裁定首版零自动化 UI 测试，卡点是「状态注入缝，不是构建」；#249 已取证 CI 双端 QtTest 与 offscreen 可用。目的地要求「尽可能全量自动化」，即两类判据都要能自动：

1. **渲染类**：给定一帧 `PlayerViewState`，界面长什么样（标签文案、按钮态、列表行、几何）。
2. **命令类**：交互（点击按钮、拖滑块、点面板行）会向播放控制层下发哪条命令、参数与次数。

且测试进程不得拉起 SDL 音频设备或真实解码链路（`ctest -j4` 稳定性与速度的前提）。

## 决策

**双缝：命令口 + 只读渲染入口，构造参数注入。**

1. **命令口 `PlayerCommandPort`**（新文件 `app/playerport.h`）：纯虚接口，17 个公开方法与 `PlayerController` 同名同签名——`playPause` / `nextTrack` / `previousTrack` / `selectTrack` / `seekSeconds` / `setPlaybackMode` / `setAutoSkipOnError` / `setVolume` / `setMuted` / `addMusicDirectory` 十个命令方法，`playbackMode` / `autoSkipOnError` / `volume` / `muted` / `trackCount` / `currentTrackIndex` 六个只读访问器，加 `viewState()`。`PlayerController` 实现该接口；`MainWindow` 只依赖接口。测试注入记录型 fake（同时是 `QObject`，声明与 `PlayerController` 同签名的 `viewStateChanged` / `errorOccurred` 信号，复用既有 `SLOT()` 连接），断言「点哪个控件 → 收到哪条命令、参数、次数、顺序」。
2. **渲染入口**：`MainWindow::renderPlayerViewState(PlayerViewState)` 由 `private slots` 提升为 `public slots`——**不改名、不改签名**，既有 `SLOT(renderPlayerViewState(PlayerViewState))` 连接不变。测试直接调用它注入一帧状态，再用 `objectName` 查找控件（如 `mNowPlayingSubtitle`）断言 `text` / `checked` / `enabled` / `geometry`。
3. **构造注入**：`MainWindow(const AppUiPorts &ports = AppUiPorts(), QWidget *parent = nullptr)`，`AppUiPorts` 两字段——`PlayerCommandPort *player`（为空 ⇒ 自建 `PlayerController`，即生产路径不变）与 `QString musicDirectory`（为空 ⇒ `applicationDirPath() + "/../../music"` 现状）。

## 不变量

- `soundbridge::Player` 公开面零改动，`SOUNDBRIDGE_ABI_VERSION` 保持 2。
- 生产默认路径逐行为等价：`AppUiPorts()` 缺省构造时与现状同一构造序、同一信号接线。
- 注入测试进程零 SDL：不构造 `PlayerController`，不触碰 `AudioDevice`。
- 依赖方向不变：`playerport.h` 只依赖 `soundbridge` 值类型（如 `PlaybackMode`）与 Qt，不反向进 `sdk/`。

## 考虑过的替代方案

**只做渲染缝。** 能断言「状态 → 界面」，但命令类判据只能靠真实驱动，B 类交互判据继续人工——与「全量自动化」目的地冲突，否决。

**真实 `Player` + `SDL_AUDIODRIVER=dummy` 驱动。** 判据退化成端到端：慢、受音频链路时序影响，「采样撞短窗口必假红」纪律直接命中；dummy 只作 offscreen 环境变量兜底，不承载判据，否决。

**直接暴露 `Player` 指针给测试。** 违反毕业条件①「不暴露 `Player` 的只读渲染入口」，把 SDK ABI 风险带进 UI 测试，否决。

**反射式 `findChild` + QTimer 驱动真实状态。** 依赖真实文件与时序，断言脆弱；与 #115/#116 的「异步会话起点按事件定位」教训同源，否决。

## 后果

- `MainWindow` 公开面：构造函数多一个带默认值的参数（源兼容），`renderPlayerViewState` 提升为 public（不改名）；新增 `app/playerport.h`。
- fake 口放在测试源里，不进生产目标；`PlayerController` 仍是生产唯一构造点。
- §9.3 冒烟步骤保留为人工抽查用途；回归口径以 §9.7.2 分级为准。
- 错误路径注意：`handlePlayerError` 的非 autoSkip 分支弹 `QMessageBox`，在 Windows + offscreen 下必崩（§9.7.3），相关呈现判据永久人工（§9.7.5）。

## 追加注记（2026-11-22，执行票 [#257](https://github.com/wt159/SoundBridge/issues/257) 落地）

「决策」第 3 条写 `AppUiPorts` 两字段，实现为**三字段**：多一个 `QObject *playerSignals`。理由：`PlayerCommandPort` 是纯虚接口、本身不是 `QObject`，无法从 `PlayerCommandPort *` 交叉转型到 QObject 面，而 `SLOT()` 信号连接需要 QObject 面；接口形状因此保持不变，信号面由注入方显式给出（生产路径取 `mController.get()`，注入路径取替身的 QObject 面）。上文保留原文。

同批新增三处静态方法作为可断言测试缝，均不改变生产行为：`PlayerController::errorReasonText` / `PlayerController::composeErrorSubtitle`（由 `playercontroller.cpp` 匿名命名空间函数上移）与 `MainWindow::buildErrorMessageBoxText`（由 `handlePlayerError` 的文案构造提取）。它们服务于规格 §9.7.2 的 `UI-E2`–`UI-E6` 文案断言；错误码到文案的映射与 `QMessageBox` 的呈现分支逐字未动。

## 追加注记（2026-11-22，地图 [#260](https://github.com/wt159/SoundBridge/issues/260)）

「决策」末条的不变量「`soundbridge::Player` 公开面零改动，`SOUNDBRIDGE_ABI_VERSION` 保持 2」被**定向覆盖一次**：

- `Player` 新增 `addTrack` / `removeTrack` / `clearPlaylist` 三个非虚成员函数（ADR [0018](0018-playlist-edit-contract.md)）；`PlayerCallbacks` 与 `TrackInfo` 的字段一字未动，`SOUNDBRIDGE_ABI_VERSION` **仍为 2**。
- 因此「决策」第 1 条的「17 个公开方法」涨到 **19**：新增 `removeTrack` / `clearPlaylist`。`addTrack` **不进端口**——它在 UI 层没有入口（`QFileDialog` 是 `QDialog` 子类，被 ADR [0015](0015-popup-base-and-list-mask-removal.md) 否决），进端口会让「端口方法与 UI 实际能发出的命令一一对应」这条核对失去意义。
- 其余结论不变：`AppUiPorts` 三字段、`renderPlayerViewState` 提升 public、fake 口不进生产目标、`PlayerController` 仍是生产唯一构造点。
