# 0006 读失败与流结束的分离在提取器出口以诊断旁路表达

- 状态：已接受
- 日期：2026-11-13
- 来源：[地图：直播源（HLS LIVE / ICY）生命周期与无限流终止语义](https://github.com/wt159/SoundBridge/issues/147)、[读失败与流结束的区分契约（packetAt 出口）](https://github.com/wt159/SoundBridge/issues/151)；规格落点 [`doc/live-source-spec.md`](../../doc/live-source-spec.md) §6。事实基线见 [#148](https://github.com/wt159/SoundBridge/issues/148) 产出的 [`docs/research/live-source-facts.md`](https://github.com/wt159/SoundBridge/blob/research/live-source-facts/docs/research/live-source-facts.md)（§5.2 更正见 [#148 comment](https://github.com/wt159/SoundBridge/issues/148#issuecomment-6843635133)）。

## 背景

原生 URL 层直通提取器 `HlsExtractor::packetAt()` 把 `av_read_frame` 的**任何**负返回一律译作 `nullptr`（`HlsExtractor.cpp:188-191`），而 `PacketCursor::nextPacket()` 在 `kUnknownPacketCount` 下把 `nullptr` 定义为「包序列正常结束」（`ExtractorHelper.hpp:233-235`）；解码侧随后无条件 `flushDecode() → finishAtEndOfStream()`。也就是说今天一次网络抖动会被当成曲目结束。

对无限流来说这尤其致命：事实基线实证 HLS LIVE 的窗口耗尽、segment 404、`m3u8_hold_counters=1000` 耗尽**一律收敛成 `AVERROR_EOF` 且不报错**。若不做区分，重连状态机既没有触发事件，也无法把「供给停滞」与「真结束」分开。

难点在于：错误性质在提取器边界已经丢失，而解码侧只看到一个 `bool`；同时项目有若干不可推翻前提——包是唯一层间单元（ADR 0003）、可解性不在提取器判定（ADR 0002）、不新增错误码 / 回调类型 / 能力位、`nullptr` 语义是既有通用契约。

## 决策

**在提取器侧新增一条只读「诊断旁路」：记录最近一次 `av_read_frame` 的负返回；`packetAt` 仍返回 `nullptr`、`nullptr` 语义不扩展；解码侧在 `nextPacket()==false` 之后查询一次该状态，再决定 EOS / 错误 / 重连。**

- 诊断状态**不是第二个层间单元**：不承载音频数据、不参与包序列、不进公开面。包仍是唯一层间单元（**ADR 0003 一字不动**）。
- `ExtractorHelper.hpp:233-235` 的「未知总数下 `nullptr` 就是结束」逐字不动；不改 `PacketCursor`、不动 4 个 `PacketReader`、不新增回调类型、不新增错误码、不改 `onError` 签名。
- **LIVE 下 `AVERROR_EOF` 判「供给停滞、非自然终点」**，不进 EOS、交重连通道；真结束的判据复用 `duration()==0`（VOD `>0` 的 EOF 仍是真 EOS）。**不新增能力位 / LIVE-VOD 形态位**；`GLOSSARY.md` 词条不改，规格措辞统一为「不主动产生自然终点」，把 `m3u8_hold_counters=1000` 登记为外部供给约束。
- **负返回按层分流**：`AVERROR_EXIT` → 取消（既不 EOS 也不报错，沿用现有 break）；EIO / HTTP 错 / playlist 重载失败 → 数据源层既有 `FileOpenFailed`；`AVERROR_INVALIDDATA` 等坏数据 → 内容层，仍交播放期「包耗尽 + EOS 汇总」（**ADR 0002 一字不动**）；LIVE 的 EOF → 重连预算耗尽才转终局。**取消检查优先级高于任何分类**。
- **三条生命周期不变量**：`open()` 置「无失败」；**任一成功读包即清零**；解码侧**读取一次即失效**。失败状态不得跨越一次成功读包、不得被消费两次（呼应 #113 的清槽教训）。提取器与 `PacketCursor` 同在解码线程，无需原子 / release-acquire。
- **VOD 都记，只由消费侧分流**：提取器里不做 LIVE/VOD 判断，`duration>0` 的行为与今天逐字一致，[#145](https://github.com/wt159/SoundBridge/issues/145) 门禁数值 / 目标 / `TIMEOUT` 一字不动。

**范围声明（逐字）**：本 ADR **只管「读失败与流结束的分离」在提取器出口的表达**；**不触碰 ADR 0002**（可解性仍只在播放期判定）、**不触碰 ADR 0003**（包仍是唯一层间单元，旁路不承载数据）、**不触碰 ADR 0007**（消费侧状态机归属由后者单独裁定）。`rw_timeout` 的补齐与该选项的归一规则见 [#155](https://github.com/wt159/SoundBridge/issues/155)，属实现约束，不属本 ADR 的契约变更。

## 考虑过的替代方案

**把 `PacketCursor::nextPacket` 从 `bool` 改成 ok / end / error 三态。** 语义最干净，一口气把区分表达在通用契约上。否决：要改通用契约与所有调用点，`kUnknownPacketCount` 语义要重定义，HLS VOD 的门禁判据必须重验；与「原判据数值一字不动」的不可推翻前提相冲，回归面明显更大。诊断旁路把变更面收在提取器 + 解码侧一个查询点上。

**让数据源层持失败原因。** `UrlSource` 记录失败。否决：它持 URL 不持 socket（`UrlSource.cpp:151-159`），FFmpeg 原生 URL 层的失败对它不可见（事实基线 §1）；直通路径根本不经 `readAt`（恒返 `-1`）。这条连「够得着」都不成立。

**解码侧反推**（靠 `flushDecode()` 结果或耗时猜测）。否决：错误性质在 `HlsExtractor.cpp:188-191` 已丢失，反推不是判定而是猜测，且会让「取消」与「真结束」混在一起。

## 后果

- `doc/live-source-spec.md` §6 是本决策的契约落点；本 ADR 只记「为何用旁路表达」与范围边界，不重复条款。
- 后续 [#149](https://github.com/wt159/SoundBridge/issues/149) 的重连状态机把「`nextPacket()==false` 后查诊断状态」定为唯一触发入口；[#155](https://github.com/wt159/SoundBridge/issues/155) 证明读超时被 hls 洗成 `AVERROR_EOF`，因此不需要第二个触发入口，本 ADR 的 `nullptr` 语义得以保持。
- 公开面零变化：三态、错误码集合、回调类型、能力位、`duration()==0` 单一信号全部不变；P2 门禁判据无需重验。
- **反悔通道**：若日后要撤回诊断旁路（改回三态或另立通道），须另开新票，同时处理本 ADR、VOD 回归判据、#149 的重连触发点与 #150 的预算计量；不得只删旁路留下「读失败仍当 EOS」的旧语义。
- 本图未改任何生产代码 / 用例 / CI / 既有判据数值；实现按规格 §11 的 P3 票链另开图外 effort。
