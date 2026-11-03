# 0001 错误码只表达原因，处置交给 ErrorInfo

- 状态：已接受
- 日期：2026-11-03
- 来源：[#25](https://github.com/wt159/SoundBridge/issues/25)

## 背景

`ErrorCode` 是公开头（`sdk/include/soundbridge/error.h`）里的一部分，调用方按它分支。加坏输入相关的新错误时出现一个问题：**「为什么失败」和「该怎么处理」要不要都写在码里**。

现状已经有两条线索：

- `ErrorInfo` 里已经有 `action`（`SkipTrack` / `StopPlayback` / `CheckFile`）和 `recoverable`；
- `app` 层的文案只看 autoSkip 开关，不看码，所以码实际上没承担处置语义。

## 决策

**错误码只表达原因。处置一律由 `ErrorInfo.action` / `recoverable` 决定。**

具体到坏输入，新增两个码：

- `DataTruncated`（`0x030003`）：文件可能不完整；
- `DataCorrupted`（`0x030004`）：流内坏帧。

两者 `recoverable = true`、`action = SkipTrack`，但**不合并成一个「坏文件」码**——截断是「播完剩下的再收尾」，坏帧是「跳过继续」，处置不同，`action` 就没法填。同时删除 `DecodeFailed`（`0x030002`）：全仓零运行时调用者，只有 `ErrorInfo` 表和测试引用它。

## 考虑过的替代方案

**让码带处置语义**（例如 `DataTruncatedButPlayable`、`DataCorruptedSkipped`）。否决理由：处置会随 autoSkip 开关、是否批量扫描、当前是否是选中曲目而变化，这些信息在产生错误的地方拿不到。码带处置等于把「策略」焊死在「事实」上，第二处真源必然和 `ErrorInfo` 打架。

## 后果

- 新增错误码时必须同时填 `recoverable` 与 `action`，否则编译期发现不了、运行期行为退化。
- app 层文案要按 `action` + 码分档，不能再只看 autoSkip 开关（现状 `app/playercontroller.cpp:171` 只有两个串）。
- 「坏在哪」不由码承担：位置复用已有的 `curPositionMs`（`MusicPlayer.cpp:458-469`），`detail` 只放人类可读的原因，不放机器可解析的结构化数据。这样不必改任何回调签名。
