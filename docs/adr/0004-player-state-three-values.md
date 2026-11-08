# 0004 播放状态保持三态，不开第二套状态机

- 状态：已接受
- 日期：2026-11-07
- 来源：[#77「播放状态集合与术语边界」](https://github.com/wt159/SoundBridge/issues/77)、[#75「地图：播放器状态与公开 API 契约」](https://github.com/wt159/SoundBridge/issues/75)；事实基线见 [#76](https://github.com/wt159/SoundBridge/issues/76) 与 [`docs/research/public-api-contract-facts.md`](../research/public-api-contract-facts.md)

## 背景

`soundbridge::PlayerState` 对外只有 `Playing / Paused / Stopped` 三态（`sdk/include/soundbridge/player.h:11`），内部权威 `MusicPlayerState` 也是三态、且是唯一写入口 `updatePlayState()` 的投影来源。但代码里还并行存在 `StreamDecoderState` 五态（含 SEEKING 静默窗口）、`m_switching`、`PlaybackTask` 存在性、`m_failedIndices` 等隐式判据；`shouldFillSilence()` 甚至完全不读 `m_state`。

`doc/开发方向建议.md:126` 建议把状态机扩成六态 `Idle / Preparing / Playing / Paused / Stopped / Error`，`doc/streaming-playback-spec.md` §4.1 又把 `ERROR` / `SWITCHING` 当状态名用，而公开 API 没有 Error 值。于是「对外到底有几种状态」成了接入方必须自己猜的事；地图 #75 的目的地就是把它钉死。

约束来自上一张图：#42 的规格 §4.1 规定「状态基准是 `MusicPlayerState`，`PlayerState` 与 `StreamDecoderState` 只作投影 / 诊断，**不新增状态机**」，这条在 #75 被列为不可推翻前提。

## 决策

**对外播放状态保持三态 `Playing / Paused / Stopped`。错误不进状态面，任务内部进度不进状态面；两者分别由 `onError` 与「播放任务状态」表达。**

- `Playing / Paused / Stopped` 是 `MusicPlayerState` 的直接投影，唯一权威仍是内部状态；不新增第二套状态机，也不把 `MusicPlayerState` 升格为对外基准。
- 无曲目 / 扫描中 / 准备中 / 停机一律投影为 `Stopped`；seek 的 SEEKING 窗口、EOS 两曲之间、切歌、自动跳过坏文件都不改变对外状态（切歌前后都是 Playing，不闪 Stopped）。
- 错误由 `onError` 表达原因，播放状态只表达处置结果；autoSkip 跳轨成功时可以完全没有状态事件。
- 术语分家并写进 `GLOSSARY.md`：**播放状态**（对外唯一状态面）与**播放任务状态**（内部空闲 / 解码中 / 定位中 / 已到末尾 / 出错）。
- 契约细节（三连回调顺序、error→state、幂等、真相源、判据）落在 `doc/player-state-contract.md`，不在本 ADR 重复。

## 考虑过的替代方案

**扩到六态 Idle/Preparing/Playing/Paused/Stopped/Error。** 语义更细，UI 能直接显示「准备中」。否决：Idle 与 Stopped 同义；Preparing 对应 init 期只读扫描，而 ADR 0003 已允许该扫描且不改变「是否在消费音频」；Error 与 `onError` 重复，且 `doc/bad-input-spec.md` §8.2 规定错误时状态是 Paused/Stopped 这类处置结果。更硬的问题是：六态要么新增第二套状态机，要么让内部状态直接对外，两者都违反「不新增状态机」，并会让 #42 的 Seek / 切歌状态迁移表整体失效。

**把 `StreamDecoderState` 直接公开。** 能让调用方看到 SEEKING / EOS。否决：它是单次播放任务的诊断状态，生命周期比曲目短，公开后调用方要处理「状态存在但任务已销毁」的窗口；且它含 `ERROR` 值，会与 `onError` 形成两个错误来源。

**给 `PlayerState` 加 `Error` 一个值，其余保持。** 改动最小。否决：错误有 12 段码与 `traceId`，一个 `Error` 值表达不了原因；且与 `onError` 双轨后，「autoSkip 开启且跳轨成功」到底算不算 Error 无法自洽。

## 后果

- `doc/开发方向建议.md` 方向六的第 1 步（定义六态）就此关闭，改注为「已裁定保持三态」；第 2–4 步（回调语义、API 文档、接入示例）中，回调语义由本图规格接手，文档与示例仍属界外。
- `doc/streaming-playback-spec.md` §4.1 的 `ERROR` / `SWITCHING` 措辞要改成「播放任务状态」，§1 / §10.4 的界外声明要改指向 `doc/player-state-contract.md`；迁移表本身不动。
- 代价是三态无法区分「准备中」与「停止」，调用方只能靠 `trackCount()` 与回调组合判断；这是有意识接受的表达力损失，写进规格 §10.3。
- 头文件注释、错误路径顺序调整、判据用例都属图外执行项（规格 §10.1 的 C1–C6），本 ADR 不承诺代码已改。
- 本图只开这一条 ADR：其余决策（回调顺序、命令返回值、真相源、上报范围）都是改文档即可反悔的契约，不满足「难反悔 / 无背景会惊讶 / 真实权衡」三条同时成立。
