# 0005 静态点播走自定义 AVIO，清单类与无限流走 FFmpeg 原生 URL 层

- 状态：已接受
- 日期：2026-11-11
- 来源：[地图：网络音频源（URL）接入规格与全谱系架构决策](https://github.com/wt159/SoundBridge/issues/118)、[接入路径裁决：自定义 AVIO 扩展与 FFmpeg 原生 URL 层如何分工](https://github.com/wt159/SoundBridge/issues/123)；事实基线见 [事实核对：DataSource、自定义 AVIO 与提取器链路的网络接入现状](https://github.com/wt159/SoundBridge/issues/119) 与 [事实复核：全谱系协议形态的一手规范、授权与合规边界](https://github.com/wt159/SoundBridge/issues/120) 产出的 [`docs/research/network-datasource-facts.md`](../research/network-datasource-facts.md) / [`docs/research/network-protocol-facts.md`](../research/network-protocol-facts.md)

## 背景

项目当前的音频解码链是「本地文件 + 自定义 AVIO」：`MusicPlayList::_addMusic()` 把路径交给唯一的 `DataSource` 生产子类 `FileSource`，4 个 `PacketReader`（ape / mkv / asf / m4a）用 `AVFMT_FLAG_CUSTOM_IO` + `avformat_open_input(&fmt, nullptr, nullptr, nullptr)` 从 `readAt(off, …)` 按绝对字节偏移读，并在 `open()` 里要求 `size > 0`、`AVSEEK_SIZE` 返回构造时已知长度。URL 字符串在第一层就被 `FileSource` 当本地路径打开失败、以 `FileOpenFailed` 拒绝，根本到不了 `ExtractorFactory`。

FFmpeg 4.4.4 构建本身带网络协议（HTTP/HTTPS/TLS/HLS/RTMP 均为 1），但那是 configure 默认产物；项目没显式设 `protocol_whitelist`、没设 `interrupt_callback`，`dash` demuxer 因 `libxml2` 未启用而为 0。也就是说，障碍在自有代码侧，不在依赖。

用户要求「全谱系都进规格」：静态点播、HLS VOD、HLS LIVE、ICY 直播、DASH 以及两条接入路径都要有裁决。于是「这两种装配方式如何分工」成了必须先钉死的架构分叉。

## 决策

**两条路径并存但互斥，按负载形态分派，不按 URL scheme 分派；本版只实现 P0 静态 HTTP 点播，其余形态写进规格但本版不做。**

- **静态点播源 → 自定义 AVIO 扩展**：新增数据源实现把 `Range: bytes=<off>-…` 映射成 `DataSource::readAt(off, buf, size)`，继续走既有 `PacketReader` 链路，承接 ADR 0003「包为唯一层间单元」。
- **清单类源 / 无限流源 → FFmpeg 原生 URL 层**：`avformat_open_input(&fmt, url, ...)` + 显式 `protocol_whitelist` + `interrupt_callback`；规格逐条写明它不适用自定义 AVIO 的哪些条款。
- **分派点在数据源构造层**，按「是否清单类」判定；分派失败一律明确拒绝，**不静默回退**到另一条路径。
- 每种「本版不做」的形态都必须在规格里有完整裁决行（理由 + 重开触发条件），不得以「界外」一笔带过。

支撑事实：FFmpeg 4.4 有自定义 `pb` 时 `init_input()` 直接返回、`s->url` 为空串，HLS 用 `parse_playlist(c, s->url, ...)` 拿空 base；之后每个 segment / variant 经 `hls.c → s->io_open → io_open_default → ffio_open_whitelist` 用 playlist 内相对 URL 重开，无 scheme 无法解析协议 ⇒ 打开失败。这里**更正** 11-09 调研快照的简化表述：`ff_make_absolute_url2` 对空 base + 相对 URI 原样返回（非空），`if (!pls->url[0])` 不因此触发；工程结论不变，机制措辞以本 ADR 为准。

## 考虑过的替代方案

**全走自定义 AVIO 扩展（不为清单类开第二条路径）。** 改动面最小、层间契约最统一。否决：自定义 `pb` 只承载初始 playlist 字节，HLS 的 segment / variant 需要真实 URL 与 base 才能重开；把清单文本喂给现有 11 个二进制 magic sniffer 只会落到 `createExtractor()==nullptr`。这是协议层的硬限制，不是调参问题。

**全走 FFmpeg 原生 URL 层（连本地文件与静态点播一起换）。** 一条路径到底，取消与白名单配置统一。否决：会推翻 ADR 0003 已验证的「包为唯一层间单元 + demux 播放期存活 + 索引 O(1)」链路，改动面覆盖已稳定的本地文件播放与 E/C/O 三链的判据基线；而静态点播与现有「已知长度 + 可随机 seek」契约天然兼容，没必要为它付这个代价。

**只把静态点播写进规格，其余形态留白。** 最省事。否决：用户明确选了「全谱系都进规格」，且「没做的形态不写理由与重开条件」会让下一次接入 effort 重新争论同一批问题；本 ADR 记录的正是这些「明确的 no」。

## 后果

- `doc/network-audio-source-spec.md` 是本决策的契约落点；本 ADR 只记「两条路径如何分工 + 本版只做 P0」，不重复契约条款。
- P1（能力模型、`m_abortDecode` 贯通 `avioRead` + `interrupt_callback` + 超时）是 P2/P3 的**前置能力**，属图外实现 effort；在此之前无限流形态没有可判定的取消语义。
- 公开面**不新增能力位 / `isSeekable()`**：能力表达只放数据源层内部，UI 继续用 `duration()==0` 单一信号（ADR 0004 与 `doc/player-state-contract.md` §7 的不可推翻前提）。
- `CONFIG_DASH_DEMUXER 0` 是 2026-11-11 Windows configure 产物的时点事实；Linux 侧无产物可实证，启用 dash 属 P4 重开条件的一部分。
- 本图未改任何生产代码 / 用例 / CI / 既有判据数值；实现按规格 §11 的 P0–P4 另开图外执行票。
