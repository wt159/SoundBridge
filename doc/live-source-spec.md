# 直播源（HLS LIVE / ICY）生命周期与无限流终止语义规格

> 来源：wayfinder 地图 [地图：直播源（HLS LIVE / ICY）生命周期与无限流终止语义](https://github.com/wt159/SoundBridge/issues/147)，由 9 张子票（[#148](https://github.com/wt159/SoundBridge/issues/148)、[#149](https://github.com/wt159/SoundBridge/issues/149)、[#150](https://github.com/wt159/SoundBridge/issues/150)、[#151](https://github.com/wt159/SoundBridge/issues/151)、[#152](https://github.com/wt159/SoundBridge/issues/152)、[#153](https://github.com/wt159/SoundBridge/issues/153)、[#154](https://github.com/wt159/SoundBridge/issues/154)、[#155](https://github.com/wt159/SoundBridge/issues/155)、[#156](https://github.com/wt159/SoundBridge/issues/156)）的决策折叠而成。2026-11-13 收口。
>
> **本文件是规格，不是实现。** 代码、用例与 CI 改动走图外 P3 执行票链（P3-0 契约 → P3-1 实现 → P3-2 门禁）；本图未改 `sdk/` / `app/` / CI / 用例 / 任何既有判据数值与 `TIMEOUT`。
> 上位规格：[网络音频源接入规格](network-audio-source-spec.md)、[播放器状态契约](player-state-contract.md)。术语以 [`GLOSSARY.md`](../GLOSSARY.md) 为准。难逆转决策见 [ADR 0006](../docs/adr/0006-live-source-read-failure-separation.md) 与 [ADR 0007](../docs/adr/0007-reconnect-state-machine-ownership.md)。
> 事实基线：[`docs/research/live-source-facts.md`](https://github.com/wt159/SoundBridge/blob/research/live-source-facts/docs/research/live-source-facts.md)（`research/live-source-facts` 分支 `e2d24d3c8`，**时点快照**），其 §5.2 已由 [#155](https://github.com/wt159/SoundBridge/issues/155) 实证推翻，更正见 [#148 comment](https://github.com/wt159/SoundBridge/issues/148#issuecomment-6843635133)。**行号会漂移，冲突一律以源码与现行规格为准。**

## 1. 目的地与范围

把 **HLS LIVE 与 ICY 直播**两类无限流形态的**连接 / 重连生命周期**与**无自然终点的终止语义**裁成可判定的契约：状态机归属层、触发入口、预算与停止上界、读失败与流结束的分离、`rw_timeout` 的真实生效面、分派层放行条件、公开 UI 语义、判据形态与合规边界。走到这里，`doc/network-audio-source-spec.md` §11 的 P3 两项**技术**重开条件（独立连接 / 重连状态机意见、无限流 EOS / UI 语义定稿）即命中；P3 实现 effort 不再需要任何语义裁决。

- **范围只含规格与决策**：只改 `doc/` / `docs/`，不改生产代码 / 用例 / CI，不动既有判据数值、容差、基线与 `TIMEOUT`。
- **授权复核属界外**：合规结论是时点快照（上位规格 §10）。本规格只产「三档归类 + 复核清单 + 责任归属」，**不写死任何公网 URL 与授权结论**；P3 第三项重开条件「授权明确」由该清单在图外执行时闭环（§11）。
- **界外**：app 侧 UI 实现与直播视觉指示；公网第三方流进 CI 门禁或产品默认列表；授权结论本身；DASH（P4，`CONFIG_DASH_DEMUXER 0`）；AES-128 / DRM；LL-HLS 与多码率 ABR 切换。
- **不可推翻前提**逐条核对见 §13；本规格**没有**触碰任何一条（这是与上位规格 §12.2 的差别，后者有一条已登记的冲突化解）。

## 2. 术语

`GLOSSARY.md` **一字不动**（[#151](https://github.com/wt159/SoundBridge/issues/151) 裁「无限流源」词条不改、[#155](https://github.com/wt159/SoundBridge/issues/155) 裁不新增超时类词条、[#153](https://github.com/wt159/SoundBridge/issues/153) 与 [#152](https://github.com/wt159/SoundBridge/issues/152) 均不新增术语）。本规格内定义以下工作用语，**只在规格内有效，不进词表**（沿用上位规格 §5 的纪律）：

| 用语 | 含义 |
|---|---|
| **不主动产生自然终点** | 无限流的规格措辞。播放器不主动产生 EOS；但外部供给可能停滞（见「供给停滞」），这不构成流的自然终点。 |
| **供给停滞** | LIVE（`duration()==0`）下的一次 `AVERROR_EOF`。不是流结束、不是错误终局，是重连通道的触发事件（[#151](https://github.com/wt159/SoundBridge/issues/151)）。 |
| **读失败** | `av_read_frame` 的负返回（EIO / HTTP 错 / playlist 重载失败等），经提取器诊断旁路表达（§6）。 |
| **重连原语** | `HlsExtractor` 新增的**无状态**内部方法，只做 `closeFmt() + open()`；不持预算、不 sleep、不重试、不判可解性（[#149](https://github.com/wt159/SoundBridge/issues/149)）。实现命名交图外 effort。 |
| **直通提取器** | `HlsExtractor` 在本规格中的职能名：`avformat_open_input(..., nullptr, &opts)` 自动探测 demuxer 的原生 URL 层提取器，同时承载 HLS LIVE 与 ICY（[#152](https://github.com/wt159/SoundBridge/issues/152)）。 |
| **诊断旁路** | 提取器记录最近一次负返回的只读状态查询；**不是第二个层间单元**，包仍是唯一层间单元（ADR 0003 / [#151](https://github.com/wt159/SoundBridge/issues/151)）。 |
| **探测期超时 / 播放期读超时 / 取消打断** | 三类不同机制的墙钟 / 取消概念，按 [#155](https://github.com/wt159/SoundBridge/issues/155) 分层（§3.3）。 |
| **重连预算 / 单次窗口 / 总预算** | 自有状态机的策略参数（§5.3），**不是 CI 门禁阈值**。 |

停止使用「网络流 / 流式播放 / 在线流」指代网络来源（词表 `_Avoid_`），统一用「网络音频源」；HLS playlist 一律叫「清单」。

## 3. 现状事实基线

以下均为源码 / 本地实证，不是方案；用来给每条裁决定位「改哪里」。事实基线全文见 `docs/research/live-source-facts.md`（分支 `research/live-source-facts`，提交 `e2d24d3c8`）。

### 3.1 连接与重开接缝（[#148](https://github.com/wt159/SoundBridge/issues/148) §1、[#149](https://github.com/wt159/SoundBridge/issues/149)）

| 层 | 事实 |
|---|---|
| 历史接缝 | `DataSource::reconnectAtOffset(off64_t)`（`DataSource.hpp:173`）**生产调用点为零**，挂在 `DataSource` 而非持有者实际持有的 `DataSourceBase *`；`FileSource` / `HttpSource` / `UrlSource` 均未 override；`UrlSource::readAt()` 恒返 `-1`（直通路径不经 readAt 供数）。 |
| 直通提取器 | `HlsExtractor` 独占 `AVFormatContext *m_fmt` 与 private `open()`；`open()` 用 `avformat_open_input(&m_fmt, url, nullptr, &opts)`（`HlsExtractor.cpp:101`，**自动探测 demuxer**），opts 只有 `protocol_whitelist="http,tcp"`；`m_consumed` / `m_audioIndex` / `extradata` 在 open 时重算。 |
| 解码侧缓存 | `AudioStreamDecoder::start()` 缓存 `m_srcSpec` / `m_codecID` / `m_durationMs`（`AudioStreamDecoder.cpp:143-145`）；`runFfmpegPacketLoop` 的 seek 分支已有 `cursor.reset` + 重建 packet decoder 的现成写法（`:996-998`）。 |
| 包游标 | `PacketCursor::nextPacket()` 在 `kUnknownPacketCount` 下把 `nullptr` 当正常结束（`ExtractorHelper.hpp:233-235`）；`packetAt` 把 `av_read_frame` 的**任何**负返回译 `nullptr`（`HlsExtractor.cpp:188-191`）。 |
| 分派 | `MusicPlayList::_addMusic()`（`MusicPlayList.cpp:248-292`）用 `UrlSource::headHasVodFeatures()` 判 VOD，false 即报错 `playlist content has no VOD features (LIVE rejected)` 明确拒绝；`HttpSource::probe()` 遇 `Icy-*` 头即拒（`:685-687`）。 |
| LIVE 行为 | 无 `#EXT-X-ENDLIST` 的 playlist 自行重载（间隔 `TARGETDURATION/2`）；窗口耗尽 / segment 404 / `m3u8_hold_counters=1000` 耗尽**一律以 `AVERROR_EOF` 收场且不报错**；LIVE 的 `AVFormatContext::duration == AV_NOPTS_VALUE` → 归一成 0；`av_seek_frame` 在 LIVE 上失败且错误被洗成裸 `-1`（hls 声明 `AVFMT_NOGENSEARCH`）；`live_start_index=-3`。 |
| ICY 行为 | FFmpeg `http` 的 `icy` **默认 1**（自动发 `Icy-MetaData: 1` 并剥离元数据块）；`HttpSource::probe()` 自己**不发**该头，故存在「服务端不回任何 `Icy-*` → 被当静态点播」的漏网面。`hasIcyHeader()` 已 `lowerCopy` 匹配任意 `icy-` 前缀（`:345-352`），问题不在匹配。 |
| 原生重连选项 | `reconnect` / `reconnect_streamed` 默认 0 且须同时开；流式源重连回 **offset 0**；`reconnect_delay_max` 默认 120 s、退避 1→3→7…；hls 的 `save_avio_options` 固定 7 项（`headers, http_proxy, user_agent, cookies, referer, rw_timeout, icy`，`hls.c:1714-1734`）**不含** `reconnect*` / `timeout`；`http_persistent` 默认 1。 |
| 构建 | Windows `CONFIG_HLS_DEMUXER 1` / `CONFIG_HTTP_PROTOCOL 1` / `CONFIG_DASH_DEMUXER 0`；`CONFIG_OPENSSL 0` + `CONFIG_GNUTLS 0` ⇒ `https://` 实际不可用；Linux 侧无产物，为推断。 |

### 3.2 必须记住的三条"坑"（本图核源码新发现）

1. **不复位 `PacketCursor` 会立刻 EOS**：`start()` 缓存规格，`open()` 重算 `m_consumed`；重开后 `packetAt(index)` 的 `index != m_consumed` 判断（`HlsExtractor.cpp:170-172`）直接返回 `nullptr` → 解码侧 `finishAtEndOfStream()`（[#149](https://github.com/wt159/SoundBridge/issues/149) 裁决 6）。
2. **`pos==0` 归零例外会把 LIVE 变成 EOS**：`seekToMs(0)` → `packetAt(0)` → `av_seek_frame`（LIVE 必失败、错误洗成裸 `-1`）→ `nullptr` → EOS 汇总（[#153](https://github.com/wt159/SoundBridge/issues/153) 裁决 4）。
3. **`interrupt_callback` / `protocol_whitelist` / 取消贯通三条上位规格 §3 描述已过期**：`HlsExtractor.cpp:92-94` 已挂 `interrupt_callback`、两处已设 `http,tcp`、P1 已贯通阻塞读点。上位规格正文按历史留痕不动，更正见本规格 §12.2。

### 3.3 读超时的三层真相（[#155](https://github.com/wt159/SoundBridge/issues/155) 本地实证，推翻事实基线 §5.2）

| 路径 | 定性 | 实证 |
|---|---|---|
| `HttpSource`（P0 自写 BSD socket） | `SO_RCVTIMEO/SO_SNDTIMEO=3s` **已生效**，但注释自认「只防测试挂死，**非播放任务级超时**」 | `HttpSource.cpp:83-96`；connect 100 ms 分片 × 3 s（`:170-240`） |
| `UrlSource::fetchHead()`（构造期嗅探） | `timeout=3000000` **已生效** | `avio_read` **3.146 s** 返回 `AVERROR(ETIMEDOUT)` = -138 |
| `HlsExtractor::open()` + segment（**播放主路径**） | **完全无效**：opts 只有 `protocol_whitelist`，压根没设超时 | **15 s 仍阻塞**；`interrupt_callback` 置位后 **91 ms** 返回 `AVERROR_EXIT` |

- **漏掉的通道**：`rw_timeout` 虽被 `av_opt_set_dict` 在最外层 `URLContext` 消费，但经 **`avio.c:319-323` 的 `av_opt_copy(parent)`** 复制到 tcp `URLContext`（http/tcp 同为 `ffurl_context_class`），被 `tcp_read`（`tcp.c:236-245`）的 `ff_network_wait_fd_timeout`（`network.c:78-95`，`POLLING_TIME 100` ms）消费。**与 `avio.c:383-387` 的 EAGAIN 重试路径无关**——事实基线「阻塞 socket 不会走到那里」不成立。
- **补 `rw_timeout=3000000`**：`avio_read` **3.149 s** 返回 -138；HLS 真实路径 `avformat_open_input` **3.182 s** 返回 `AVERROR_EOF`。**补 `timeout` 无效**（HLS 仍 15 s 挂死）。
- **两条硬约束**：① `timeout` 经 `tcp.c:139-141` **静默覆盖** `rw_timeout`（实测 8 s + 3 s → 3.142 s，胜出的是 `timeout`）；② hls 的 7 项抄写含 `rw_timeout` 不含 `timeout` ⇒ 补超时**只能补 `rw_timeout`**。
- **原始问题的答案**：「P1 的 3 s 读超时在 http 上是否成立」= **分路径成立**：探测期与构造期嗅探成立，播放主路径不成立；不是「名义生效」，是**压根没设置**。
- **形态限定**：3 s 到点的表现形态是 `AVERROR_EOF` / 结束序列，**不是可上报的超时错误**；判据只能证明「有限时间内返回」，不得写成「捕获到 ETIMEDOUT」。

## 4. 裁决表

### 4.1 生命周期与状态机（[#149](https://github.com/wt159/SoundBridge/issues/149)、[#150](https://github.com/wt159/SoundBridge/issues/150)）

| # | 决策 | 裁决 | 来源 |
|---|---|---|---|
| D1 | 状态机归属 | **解码线程 `AudioStreamDecoder::runFfmpegPacketLoop`**（择 (c)）；`HlsExtractor` 只做无状态重开原语 | [#149](https://github.com/wt159/SoundBridge/issues/149) |
| D2 | 触发入口 | **单一入口** = `nextPacket()==false` 后查一次诊断状态；读超时不独立；预算耗尽是**终局出口不是触发点** | [#149](https://github.com/wt159/SoundBridge/issues/149) / [#155](https://github.com/wt159/SoundBridge/issues/155) |
| D3 | 形态分叉 | HLS LIVE / ICY **同一形态无关状态机**，不新增形态位；恢复差异只写表格 | [#149](https://github.com/wt159/SoundBridge/issues/149) / [#152](https://github.com/wt159/SoundBridge/issues/152) |
| D4 | 原生选项 | **P3 不启用 FFmpeg 原生 `reconnect*` 家族，显式不启用** | [#149](https://github.com/wt159/SoundBridge/issues/149) |
| D5 | 复位契约 | 复用 seek 分支 `cursor.reset` + 重建 packet decoder；规格一致为成功准入 | [#149](https://github.com/wt159/SoundBridge/issues/149) |
| D6 | 对外状态 | 重连期间**什么都不发**（成功也静默）；只有预算耗尽走 `error → state` | [#149](https://github.com/wt159/SoundBridge/issues/149) |
| D7 | 历史接缝 | `reconnectAtOffset` **废弃但不删、不改签名** | [#149](https://github.com/wt159/SoundBridge/issues/149) |
| D8 | 重连策略 | 有限重连 **3 次**、固定间隔 **1 s**（100 ms 分片轮询 ×10）、**不指数退避** | [#150](https://github.com/wt159/SoundBridge/issues/150) |
| D9 | 预算口径 | 单次窗口 ≤ 4 s、**总预算 ≤ 12 s**（含重开窗口）；策略参数非门禁阈值 | [#150](https://github.com/wt159/SoundBridge/issues/150) |
| D10 | 停止上界 | **100 ms 取消粒度**，与总预算解耦；`stop()` 序列一字不动 | [#150](https://github.com/wt159/SoundBridge/issues/150) / [#155](https://github.com/wt159/SoundBridge/issues/155) |

### 4.2 区分契约、超时、分派、UI、门禁（[#151](https://github.com/wt159/SoundBridge/issues/151)–[#155](https://github.com/wt159/SoundBridge/issues/155)）

| # | 决策 | 裁决 | 来源 |
|---|---|---|---|
| D11 | 读失败 vs 流结束 | **提取器诊断旁路**；`packetAt` 仍 `nullptr`、`nullptr` 语义不扩展 | [#151](https://github.com/wt159/SoundBridge/issues/151) |
| D12 | LIVE 的 EOF | **供给停滞、非自然终点**；复用 `duration()==0` 判真结束，不新增能力位 / 形态位 | [#151](https://github.com/wt159/SoundBridge/issues/151) |
| D13 | 诊断状态生命周期 | `open()` 置无失败 / **任一成功读包即清零** / 消费一次失效 | [#151](https://github.com/wt159/SoundBridge/issues/151) |
| D14 | VOD 回归 | **都记，只由消费侧分流**；#145 门禁数值 / 目标 / `TIMEOUT` 一字不动 | [#151](https://github.com/wt159/SoundBridge/issues/151) |
| D15 | 播放主路径超时 | 图外 effort 在 `HlsExtractor::open()` **补 `rw_timeout=3000000`**；只补它、不设 `timeout`、VOD/LIVE 都补、不做形态分支 | [#155](https://github.com/wt159/SoundBridge/issues/155) |
| D16 | 超时归一 | 读超时在提取器出口**不可辨识为独立事件** → 归一为 EOF / 供给停滞 → 同一入口；不新增入口 / 状态位 / 回调 / 错误码 | [#155](https://github.com/wt159/SoundBridge/issues/155) |
| D17 | LIVE 准入 | `#EXTM3U` 起始 + 无 `ENDLIST` + 无 `PLAYLIST-TYPE:VOD` + **至少一条分片 / variant 行** | [#152](https://github.com/wt159/SoundBridge/issues/152) |
| D18 | `PLAYLIST-TYPE:EVENT` | **归 LIVE 放行**（改写 #143 一条结论，已加更正 comment） | [#152](https://github.com/wt159/SoundBridge/issues/152) |
| D19 | ICY 入口 | 判据不变 + 嗅探**补发 `Icy-MetaData: 1`**，结论从拒绝改走原生 URL 层；**复用同一直通提取器**（诊断状态天然覆盖） | [#152](https://github.com/wt159/SoundBridge/issues/152) |
| D20 | 终止路径 | **恰好三条**：stop / 切歌 / 错误（含预算耗尽）；重连与供给停滞都不是终止路径 | [#153](https://github.com/wt159/SoundBridge/issues/153) |
| D21 | `pos==0` 归零例外 | 无限流上 **静默失败 / 实现层短路**，不得触发 `packetAt(0)` 的 `av_seek_frame` 重放；公开契约一字不动 | [#153](https://github.com/wt159/SoundBridge/issues/153) |
| D22 | `onPositionChanged` | **不设任何数值上界**；溢出行为属实现细节 | [#153](https://github.com/wt159/SoundBridge/issues/153) |
| D23 | 判据载体 | 本地 mock 扩展 + **不注册 ctest 的独立可执行** + 事件计数判据；不进 CI、不新增耗时 gate | [#154](https://github.com/wt159/SoundBridge/issues/154) |
| D24 | 三档归类 | HLS LIVE / ICY 均为「可进 CI 否 / 可进产品默认列表 否 / 仅手工冒烟 是」 | [#154](https://github.com/wt159/SoundBridge/issues/154) |
| D25 | 授权复核 | 七项清单 + 责任归图外 effort；规格**不含任何具体电台结论** | [#154](https://github.com/wt159/SoundBridge/issues/154) |

### 4.3 恢复语义差异表（写规格，不进状态机分支，[#149](https://github.com/wt159/SoundBridge/issues/149)）

| 形态 | 供给停滞的典型来源 | 重开后能否续上 | 数据损失 | 诊断状态载体 |
|---|---|---|---|---|
| HLS LIVE | 窗口耗尽 / segment 404 / `m3u8_hold_counters` 耗尽 → 一律 `AVERROR_EOF` | **可**：重拉 playlist 从当前滑动窗口续上 | 窗口滚动期间的分片可能丢失，不回退已消费 PCM | `HlsExtractor` 诊断旁路 |
| ICY | TCP 断流 → EIO / EOF | **不可**：裸无限字节流，重连从 offset 0 开始 | **必丢数据**（断流到重连之间的音频） | **同一直通提取器**（`avformat_open_input` 自动探测 demuxer，无需第二条） |

## 5. 连接 / 重连状态机契约

### 5.1 归属层（D1，[#149](https://github.com/wt159/SoundBridge/issues/149) 裁决 1）

- **状态机**（触发判定、预算、退避、等待、终局）归 `AudioStreamDecoder::runFfmpegPacketLoop`。
- **重开动作**归 `HlsExtractor` 的无状态内部原语，内部只做 `closeFmt() + open()`；不持预算、不 sleep、不判可解性、不重试。
- **决策与执行分离的理由**：`m_fmt` / private `open()` 被提取器独占，解码侧无重开能力；而触发点已被 #151 钉在解码侧、`m_abortDecode` 就在该层、预算与停止上界同层。
- **被排除的三个方案**：(a) 数据源层 —— `reconnectAtOffset` 挂在 `DataSource` 而持有者持 `DataSourceBase *`、生产零调用、`UrlSource::readAt()` 恒 `-1`，连「够得着」都不成立；(b) 提取器层全权自愈 —— `packetAt` 是同步单包接口，内嵌 sleep / 预算会卡住解码线程，且与 #151「解码侧查一次诊断状态」冲突、擦 ADR 0003 边界；(d) 播放器层重开曲目 —— 等价切歌，污染三态与回调顺序。
- 重连是**连接生命周期动作**，不是可解性判定（ADR 0002 不动），不承载音频数据（ADR 0003 不动）。

### 5.2 单一入口与触发集（D2，[#149](https://github.com/wt159/SoundBridge/issues/149) / [#155](https://github.com/wt159/SoundBridge/issues/155)）

- **唯一入口**：`nextPacket()==false` 后查一次诊断状态，判为「供给停滞」（LIVE 的 `AVERROR_EOF`）或「数据源层失败」（EIO / HTTP 错 / playlist 重载失败）才进重连。
- **取消优先**：`AVERROR_EXIT` 沿用现有 break，既不 EOS 也不报错、不进重连（[#151](https://github.com/wt159/SoundBridge/issues/151) 裁决 4）。
- **读超时不设独立触发源**：hls 把 segment 超时洗成 `AVERROR_EOF`，而 #151 已把 LIVE 下 EOF 裁为供给停滞 ⇒ 超时**本来就经同一入口**进重连；#149 曾留的「若 #155 证明可观测则回填」分支**收口为不回填**（[#155](https://github.com/wt159/SoundBridge/issues/155) 裁决 3）。
- **预算耗尽是终局出口，不是触发点**；做成触发会自激重连。
- 判定只能建立在**读失败事件**上，不能建立在连接对象 / 连接数上（`http_persistent=1` 同域复用 keep-alive）。

### 5.3 预算与停止上界（D8–D10，[#150](https://github.com/wt159/SoundBridge/issues/150)）

| 项 | 取值 | 说明 |
|---|---|---|
| 次数上限 | **3 次** | 首次供给失败后最多 3 次重开；第 3 次失败即耗尽，**没有额外最后一次** |
| 间隔 | **1 s 固定** | 由 100 ms 分片轮询 `m_abortDecode` × 10 实现，**不是裸 sleep**；不指数退避、不设退避封顶 |
| 单次窗口 | **≤ 4 s** | 1 s 等待 + 3 s 单次 open 墙钟 + O(1) 规格一致性判定 |
| 总预算 | **≤ 12 s** | 3 × 4 s，**重开自身窗口计入** |
| `stop()` / 切歌 / 析构上界 | **100 ms 取消粒度** | 唯一硬保证；等待期由分片轮询打断，`avformat_open_input` / `avformat_find_stream_info` 阻塞由同一 `interrupt_callback` 打断 |

- 12 s 与 4 s 是**策略参数，不是 CI 门禁阈值**：不写进任何 `TIMEOUT`、不新增观测字段 / 记录面 / 数值 gate（#94 / #104 永久裁决）。
- **总预算不是 `stop()` 的上界**；事实基线 §3.3 的「`find_stream_info` 40 s 未返回」是**未取消**时的观察。
- 判据只能证明「取消后有限时间内返回」，**不得写成「在 12 s 内返回」或「捕获到 ETIMEDOUT」**（#155 形态限定）。
- 重连预算归**数据源层（连接生命周期）**，不进播放任务状态；**不是超时的一种**（超时是单次读 / open 的墙钟边界，预算是恢复策略计量）。上位规格 §5.3「超时归数据源层、不进播放任务状态」同层、正文不动，只加指针（§12.2）。
- **两个失败面共用同一预算**（[#155](https://github.com/wt159/SoundBridge/issues/155)）：首次 open 超时 → 工厂 `FileOpenFailed`；重连原语 `open()` 失败 → 本次重连失败、计入预算、不进 EOS 汇总、不重发回调；播放中途读超时 → EOF → 供给停滞 → 同一入口。

### 5.4 复位与成功准入（D5，[#149](https://github.com/wt159/SoundBridge/issues/149) 裁决 6）

- 重开后只做两件：`cursor.reset(new PacketCursor(...))` + 重建 ffmpeg packet decoder，复用 `runFfmpegPacketLoop` 现有 seek 分支写法。**不重走 `start()`、不重建整个 `AudioStreamDecoder`、不重发任何回调**。
- **不变量**：重开后 `AudioSpec` / `codecID` / `durationMs` / `extradata` **与首次 open 一致**；不一致（码率 / 编解码器切换、LIVE 转 VOD）判**本次重连失败、计入预算**，不做隐式规格切换。
- 重开仍走既有 `interrupt_callback`（opaque = `m_source`，转发同一 `m_abortDecode`），**不新建取消通道**；`stop()` 的「置位 → `join()` → 解绑」序列一字不动，无第二取消信号。
- 与 §8.4 的 `seek()` 空操作 / 归零例外是**两条独立路径**，不得混为一谈。

### 5.5 原生 `reconnect*` 不托管（D4，[#149](https://github.com/wt159/SoundBridge/issues/149) 裁决 5）

规格逐字登记「**P3 不启用 reconnect 家族，重连由自有状态机驱动**」。三条否决理由：① **预算不可计量**（原生退避 1→3→7… 与 `reconnect_delay_max=120 s` 在 FFmpeg 内部）；② **取消不可打断**（不受 `m_abortDecode` 控制）；③ **覆盖不全**（hls 只下传 7 个 avio 选项，`reconnect*` 不在其中，segment 是全新 `URLContext` 回到默认 0）。另：默认 `reconnect=0` / `reconnect_streamed=0` 须同时开启才生效，且流式源重连回 offset 0 会重播已播内容。

### 5.6 历史接缝（D7，[#149](https://github.com/wt159/SoundBridge/issues/149) 裁决 8）

`DataSource::reconnectAtOffset`（`DataSource.hpp:173`）登记为「**历史虚接缝，P3 不使用**」，图外实现 effort 不得复用：① `off64_t offset` 对无限流无字节偏移语义；② 改公开虚接口签名属越界（#118 / #143 纪律）；③ 删除会动 `test_extractor.cpp:77` / `:348` 两处测试 override，是无谓回归面。

## 6. 读失败与流结束的区分契约（[#151](https://github.com/wt159/SoundBridge/issues/151)）

### 6.1 诊断旁路（D11、D13）

- `HlsExtractor` 记下最近一次 `av_read_frame` 的负返回，新增**一条只读状态查询**；`packetAt` 仍返 `nullptr`，`PacketCursor::nextPacket` 仍返 `bool`；解码侧在 `nextPacket()==false` 之后**查一次**再决定 EOS / 错误 / 重连。
- 不改 `PacketCursor`、不动 4 个 `PacketReader`、不新增回调类型 / 错误码、不改 `onError` 签名。诊断状态**不承载音频数据、不参与包序列、不进公开面**，包仍是唯一层间单元（ADR 0003）。
- **三条生命周期不变量**：`open()` 时置「无失败」；**任一成功读包即清零**；解码侧**读取一次即失效**。失败状态不得跨越一次成功读包、不得被消费两次。提取器与 `PacketCursor` 同在解码线程，无需原子 / release-acquire。（呼应 #113 的清槽教训。）
- 排除方案：`nextPacket` 改三态（改通用契约与所有调用点、HLS VOD 门禁重验）；数据源层持失败原因（`UrlSource` 持 URL 不持 socket，原生 URL 层失败不可见）；解码侧反推（错误性质在 `HlsExtractor.cpp:188-191` 已丢失）。

### 6.2 LIVE 的 EOF 定性与单一信号（D12、D14）

| `duration()` | `AVERROR_EOF` 的含义 | 出口 |
|---|---|---|
| `> 0`（VOD） | **真结束** | `finishAtEndOfStream()`，与今天逐字一致 |
| `== 0`（LIVE） | **供给停滞、非自然终点** | 不进 EOS → 重连通道 → 预算耗尽才转终局 |

- 判定复用 `duration()==0` 这一个既有信号的底层事实（实证 LIVE `duration == AV_NOPTS_VALUE` → 归一成 0），**不新增能力位 / LIVE-VOD 形态位**；`player-state-contract.md` §7 与上位规格 §8.2 不动。零长度本地文件不走 `HlsExtractor`，不会误命中。
- **VOD 都记、只由消费侧分流**：提取器对所有负返回都记状态，不在提取器里做 LIVE/VOD 分支；#145 门禁数值 / 目标 / `TIMEOUT` **一字不动**。
- **术语冲突消解**：`GLOSSARY.md`「无限流源」词条不改；规格统一写「**不主动**产生自然终点」，把 `m3u8_hold_counters=1000` / `max_reload=1000` 登记为**外部供给停滞上界**，不写成流的自然终点。

### 6.3 负返回按层分流（[#151](https://github.com/wt159/SoundBridge/issues/151) 裁决 4）

| 负返回 | 归属层 | 出口 |
|---|---|---|
| `AVERROR_EXIT` | 取消 | 沿用现有 break，**既不 EOS 也不报错** |
| `AVERROR(EIO)` / HTTP 错误 / playlist 重载失败 | 数据源层 | 既有 `FileOpenFailed` |
| `AVERROR_INVALIDDATA` 等坏数据 | 内容层 | 仍交播放期「包耗尽 + EOS 汇总」，**ADR 0002 不动** |
| `AVERROR_EOF` @ LIVE（`duration==0`） | 供给 | 不直接 EOS → 重连 → 预算耗尽转 `FileOpenFailed` |

**取消检查优先级高于任何分类**（`AudioStreamDecoder.cpp:1006-1014` / `:1026-1028` / `:704-709` 一字不动），不产生第二条取消通道。本契约分的是**数据源层供给失败**，坏帧仍由播放期判定，不触碰 ADR 0002。

### 6.4 `rw_timeout` 补齐的三条实现约束（D15，[#155](https://github.com/wt159/SoundBridge/issues/155)）

由图外实现 effort 在 `HlsExtractor::open()` 的 opts 增设 `rw_timeout=3000000`，**本图不执行**：

1. **只设 `rw_timeout`、不设 `timeout`**：后者经 `tcp.c:139-141` 静默覆盖前者，且不在 hls 7 项抄写内（实测 HLS + `timeout` 仍 15 s 挂死）。
2. **一处设置覆盖两层**：经 `avio.c:319-323` 的 `av_opt_copy(parent)` 下传 tcp，也在 `hls.c:1716` 的 7 项里抄给 segment ⇒ 同时覆盖 playlist 与 segment。
3. **沿用既有 `3000000 µs`**：不新增配置项 / 观测字段 / 公开能力位；**VOD / LIVE 都补、不做形态分支**，#145 门禁目标 / 数值 / `TIMEOUT` 一字不动；补选项不得改变 VOD 的正常结束判据（正常 VOD 不触发 3 s，只有原本挂死的坏流从「无限等」变成「3 s 后结束」，属收敛而非放宽）。

## 7. 错误语义

- **分层不互替**：数据源层（URL 打开失败 / DNS / 重定向链 / 断流 / 分片过期 / 供给停滞耗尽）→ `FileOpenFailed`；内容层（坏帧 / 容器撒谎）→ `createExtractor()==nullptr` 仍是拒绝唯一出口（`doc/bad-input-spec.md` §2.1）。两层不互相顶替。
- **终局**：预算耗尽 → 既有 `FileOpenFailed` → `onError → onStateChanged`（仅当状态真改变）；autoSkip 开启且跳轨成功时可能没有状态事件（`player-state-contract.md` §6.1）。不新增错误码、不改 `onError` 签名、不新造顺序、不逐次上报重试失败。
- **重连期间零信号**：不发 `onError`、不发 `onStateChanged`、不进 EOS；重连成功同样静默（D6）。
- 细粒度网络原因**只进日志与本规格归类表**，不改变对外错误码集合；`doc/observability-contract.md`「不作为失败门禁」「不设任何数值阈值」永久裁决继续适用，不新增记录面 / 观测仪表。

## 8. 公开契约与 UI 行为约束（[#153](https://github.com/wt159/SoundBridge/issues/153)）

### 8.1 终止路径穷举（D20）

| 路径 | 对外状态 | 回调序列 |
|---|---|---|
| `stop()` | `Stopped` | `onStateChanged` 一次；无 `onError` |
| 切歌 | 三态不变（前后都是 Playing） | 曲目三连 `onTrackChanged → onDurationChanged → onPositionChanged`，每曲一次 |
| 错误（含重连预算耗尽） | 由既有 `error → state` 处置 | `onError(FileOpenFailed) → onStateChanged`（仅当状态真改变） |

- **没有第四条**：重连是供给恢复动作、不是终止路径；供给停滞的 EOF 不是终止路径；`m3u8_hold_counters=1000` 的 EOF 是外部供给约束、不是产品终点。
- 上位规格 §8.3「不主动产生 EOS、不发曲目结束回调、不触发自动跳轨、曲目三连只发生前两连」**逐字沿用**；本规格只补穷举与「重连非终止」。

### 8.2 `duration()==0` 单一信号（D12）

重连**成功与失败都不改变 `duration()`**，恒为 0（依据 §5.4 的规格一致准入）。`player-state-contract.md` §7 与上位规格 §8.2 一字不动；UI 仍由 `setRange(0,0)` 隐式禁用拖动，**不引入 `isSeekable()` / 能力位 / 形态位**；`onDurationChanged` 每曲恰好一次且与 `duration()` 一致。

### 8.3 position 推进与停滞

- 按已消费 PCM 推进（ADR 0003）；重连中断期间 **position 停滞**：不跳变、不回退、不归零；成功后从停滞值继续推进；期间零 `onPositionChanged`。
- **明确写进规格**：停滞**不违反**「同曲目会话内单调不回退」（§3.1 承诺单调不回退，不承诺持续推进或固定频率）。position 复位只发生在**新曲**，重连不重走 `start()`。

### 8.4 `seek()` 空操作与 `pos==0` 归零例外（D21）

- `durationMs==0 && pos>0` 的空操作判据**一字不动**（`player-state-contract.md` §7 / `sdk_interaction_tests` 既有判据）。
- `pos==0` 归零例外在**查询层仍保留**（§7 一字不动），但新增一条**实现层约束**：无限流上**不得触发 `packetAt(0)` 的 `av_seek_frame` 重放路径** —— LIVE 上该调用必然失败且错误被 hls 洗成裸 `-1`（`AVFMT_NOGENSEARCH`），`packetAt` 返回 `nullptr` 后会把「拖回 0」误变成曲目结束。
- 处置 = **静默失败 / 空操作短路**：不回退 position、不发 `onPositionChanged`、不改状态、不发 `onError`、不进 EOS。不采用「拒绝」（需新增错误码 / 回调，且裸 `-1` 不可辨识）。
- **逐字声明**：这是对归零例外的实现层短路，**不是改公开契约**；既有 seek 判据数值一字不动。与 §5.4 的重连 `cursor.reset` 是两条独立路径。

### 8.5 播放模式、autoSkip 与混合列表

- `SingleLoop` / `SingleOnce` 对无限流**不构成终点**：循环语义是「曲目结束时重播」，无限流不主动结束 ⇒ 永不触发，等同「永远是同一曲」；不新增模式专属规则。
- autoSkip 开启时，**断流的终局沿用既有 `shouldAutoSkipOnError()` / `ErrorAction::SkipTrack` 语义**（§6.3 / `streaming-playback-spec.md` §4.1），是否跳下一曲由既有开关决定，**不新造 URL 专属跳轨规则**。与 §8.1「不主动跳轨」的分界：不主动结束 ⇒ 不主动跳轨；错误终局仍按既有错误处置。
- 直播曲目与本地文件混排时，切歌 / 跳轨 / autoSkip **完全沿用上位规格 §8.4**。

### 8.6 `onPositionChanged` 上界（D22）

**不设任何数值上界 / 阈值 / 数值门禁 / 观测字段**（#94 / #104 永久裁决）；`uint64` 溢出后的行为属实现细节，规格不定义，交图外 effort（若需饱和处理，登记为实现约束而非公开契约）。

### 8.7 ICY 内联元数据（fog 清空）

`Icy-Name` / `StreamTitle` 等 `Icy-*` 头与 vorbis comment 的更新，在「不新增回调类型」约束下**没有出口**：FFmpeg `icy=1` 默认剥离元数据块后只交付纯音频，SDK 不上报元数据更新、不新增回调 / 状态值 / 能力位 / 记录面。写入界外，**不是待办**。这与 §6.1 的「诊断状态要求」是两件事（控制旁路 vs 元数据展示），不得混为一谈。

## 9. 分派层放行条件（[#152](https://github.com/wt159/SoundBridge/issues/152)）

### 9.1 HLS LIVE 准入正面特征（D17）

放行判据 = `#EXTM3U` 起始（跳过 BOM / 空白）+ **无** `#EXT-X-ENDLIST` + **无** `#EXT-X-PLAYLIST-TYPE:VOD` + **至少一条分片 / variant 行**（`#EXTINF` 或 `#EXT-X-STREAM-INF`）。只凭「有 `#EXTM3U` 且无 `ENDLIST`」不够：会把空清单 / 伪清单 / 坏清单放进来再无限重试。判据全部落在 `UrlSource` 构造期头部嗅探（**数据源层内部**），不新增能力位 / 公开入口；`<?xml` 起始仍按 `.mpd` / P4 拒绝。

### 9.2 `PLAYLIST-TYPE:EVENT`（D18）

**归 LIVE 放行**（「不主动产生自然终点」）。这**显式改写 #143 一条结论**，已在 [#143 加更正 comment](https://github.com/wt159/SoundBridge/issues/143#issuecomment-6844057273)，票正文按历史留痕不改。

> **门禁边界（必须逐字理解）**：[#145](https://github.com/wt159/SoundBridge/issues/145) 门禁矩阵的「LIVE（无 ENDLIST）→ 明确拒绝」与「`PLAYLIST-TYPE:EVENT` → 明确拒绝」两行，由**图外 P3-2 门禁票**改为放行判据；VOD 行、白名单、`https` 行、`TIMEOUT` 与所有既有数值**一字不动**。「门禁数值 / 目标 / `TIMEOUT` 一字不动」约束的是**本图零改动**，不是永久冻结用例；本规格不自行改判据。

### 9.3 master playlist 的 variant 复查

保留「跟随首个 variant URI 复查一次」（`UrlSource.cpp:171-179`），语义从「找 VOD 特征」改为「**判形态**」：variant 有 `ENDLIST` / `PLAYLIST-TYPE:VOD` → VOD；仍无 `ENDLIST` 或 `PLAYLIST-TYPE:EVENT` → LIVE 放行；**取回失败 / 无法解析 → 明确拒绝**（不静默回退、不按「保守归 LIVE」放行，依 §6.3）。

**已知误判面显式登记**：`UrlSource.cpp:20` 已承认「超长清单尾部 `ENDLIST` 嗅探不到 → 按无 VOD 特征拒绝」，P3 放行后会变成「VOD 误判为 LIVE」。本图**不放宽嗅探长度**（会动 P2 判据数值），登记为已知代价；图外 effort 若要解决须另开票。

### 9.4 ICY 入口（D19）

- 保留 `HttpSource::probe()` 现有 ICY 判据（响应出现任意 `Icy-*` 头即命中）与 P0 的 206 / `Accept-Ranges` / 总长 / 三次探测判据**一字不动**；唯一补强 = 嗅探请求**主动带 `Icy-MetaData: 1`**（与 FFmpeg `icy=1` 默认一致），消除「服务端不回 `Icy-*` 被当静态点播」的漏网面。不采用「`Content-Type` + `icy-metaint`」更强正面判定（会改 P0 判据形态）。
- 命中后**明确拒绝自定义 AVIO 路径**（要求已知长度 + `Range/206`），改走 `UrlSource` + 直通提取器，不静默回退。
- **诊断状态载体（#149 契约要求）**：**复用 `HlsExtractor`**（`avformat_open_input(..., nullptr, &opts)` 自动探测 demuxer，实证 `HlsExtractor.cpp:101`；注册表 `REGISTER_EXTRACTOR(".m3u8", HlsExtractor)` 只是键，`ExtractorFactory.cpp:348`）⇒ #151 诊断旁路天然覆盖 ICY，**不新增第二条提取器 / 第二套诊断状态**。分派层沿用既有「按内容映射注册键」的做法。
- 恢复语义差异（§4.3）**不进分派层判据、不进状态机分支**。

### 9.5 重定向与拒绝语义

- 302 / 301 / 307 / 308 按**最终响应**判定（上位规格 §4.3 既有），P3 不改；重定向链本身不改形态归类，每轮复查清单类。
- 分派失败一律「**明确拒绝、不静默回退到另一条路径**」（§6.3）；TLS / `.mpd` / `https://` 拒绝面一字不动（事实基线 §6 实证 `https://` 放开白名单也不可用）；`protocol_whitelist="http,tcp"` 不变。

## 10. 门禁形态（[#154](https://github.com/wt159/SoundBridge/issues/154)）

### 10.1 本地 mock 形态

在 #145 的 `MockHlsServer`（扩展 `MockHttpServer`）上增三类端点：① **无 `ENDLIST` 的 media playlist + 分片轮转**（固定本地生成 wav 素材循环产出、窗口可前进）；② **可注入断流**（停发分片 / 分片 404 / 发完响应头不发 body 触发 3 s 墙钟），覆盖 §5.3 的两个失败面；③ **`PLAYLIST-TYPE:EVENT` 与 ICY 端点**（ICY = 不回 EOF 的 socket + `Icy-*` 头含 `icy-metaint` + 元数据块 + 循环音频字节）。

- 素材本地生成、可复现、公网零依赖；**进程私有端口 + 日志 / 临时目录带 pid**（`ctest -j4` 纪律，上位规格 §9.3）。
- **ICY mock 必要**，且与 HLS 共用同一 mock 进程（#152 已裁同一直通提取器 / 同一诊断状态）；不做会让 ICY 侧「无限流不结束」「重连」「预算耗尽」三类契约全无判据。

### 10.2 判据载体（D23）

**不注册进 ctest 的独立可执行**（复用 `sdk/test` 的 mock 库）或 `scripts/` 下手工冒烟脚本，由人在本机跑；判据定义在**图外 P3-2 门禁票**内。输出为**事件计数 + 回调序列 + 「取消后有限时间内返回」**。

- **不进 CI 门禁**：公网 LIVE / ICY 零门禁（上位规格 §4.2 / §9.3），本地 mock 的 LIVE 正路径同样不注册 ctest；不新增 ctest 目标、不放宽 `TIMEOUT`、不新增耗时 gate。
- **事件计数为主**（[#150](https://github.com/wt159/SoundBridge/issues/150)）：重开次数 == 3、第 4 次不发生、终局恰好一次 `FileOpenFailed`、期间零 `onStateChanged` / 零 `onError`、position 停滞、duration 恒 0、无限流不主动结束；取消判据用 `std::promise/std::future` 或轮询辅助函数（**不裸 `sleep`**）证明「取消后有限时间内返回」。
- **墙钟只作防挂死守卫**（沿用现有 `TIMEOUT`），不基于墙钟数值断言、不写成「捕获到 ETIMEDOUT」。
- 可红可绿 = 注入对应缺陷（去掉分片轮询的取消检查 / 去掉预算计数 / 去掉 `nullptr` 后查诊断状态 / 去掉 ICY 请求头）必红，还原绿；可复现由「固定素材 + 固定注入序列 + 事件计数输出」保证。

### 10.3 三档授权归类（D24，上位规格 §4.2 必填字段）

| 形态 | 可进 CI | 可进产品默认列表 | 仅手工冒烟 | 理由 |
|---|---|---|---|---|
| HLS LIVE | **否** | **否** | **是**（用户手动输入 URL；ToS 红线源连冒烟都不进） | 公网第三方服务不得作门禁依赖；产品不内置公开电台列表；条款页未复核闭环 |
| ICY 直播 | **否** | **否** | **是**（同上） | 同上；FFmpeg `icy=1` 剥离元数据块，元数据侧亦无可进 CI 的素材 |

「否」针对公网形态；本地 mock 判据按 §10.2 同样不注册 ctest，故不产生「本地副本可进 CI」的例外解读（与 S1 静态点播不同：LIVE 的无限时长本身不适合 CI 门禁）。规格不写死任何具体电台 / 公网 URL / 授权结论；「**无条款不得反推允许**」逐字沿用上位规格 §10。

### 10.4 授权复核清单与责任（D25）

图外实现 effort 在 P3 放行前须逐项闭环并留归档记录（条款页 URL + 抓取日期 + 版本）：

1. 条款页 / ToS 是否**可得且可归档**（无条款页视为不可得，不得反推允许）。
2. 是否允许**第三方客户端 / 自动化**访问（SomaFM 类明确拒绝者直接排除）。
3. 是否允许**产品内嵌默认列表**、商业分发、二次分发。
4. 是否要求**署名 / 禁止录制 / 限制并发 / 限制地域**。
5. 是否允许**本地 mock 复现其协议形态**（避免把公网素材带进仓库）。
6. **复核周期**：每次产品发布前至少一次 + 条款页变更时即时复核；这是流程要求，**不是数值门禁 / 观测阈值**。
7. 复核结论与**归档责任归图外实现 effort**（P3-0 契约票指定责任人）。

本规格**不写任何具体电台的授权结论**；P3 第三项重开条件「授权明确」**本图不判「已明确」**：由图外 effort 按本清单闭环并归档后才算命中（§11）。

## 11. 分阶段落地与 P3 准入判定

### 11.1 图外执行票链

| 票 | 内容 | 承接本规格 |
|---|---|---|
| **P3-0 契约** | 实现契约对齐、责任人指定（含授权复核清单归档责任） | §5 / §6 / §8 / §9 |
| **P3-1 实现** | 诊断旁路、重连原语与状态机、复位契约、`rw_timeout` 补齐、分派层放行、ICY 请求头补发 | §5.4 / §6.1 / §6.4 / §9 |
| **P3-2 门禁** | 本地 mock 三端点、独立可执行、事件计数判据；#145 两行拒绝判据改为放行 | §10 / §9.2 |

本图只产规格与决策、不带执行；GitHub 票正文是历史留痕不改，只在需要更正的旧票上加 comment。

### 11.2 P3 准入判定（[#156](https://github.com/wt159/SoundBridge/issues/156) 第 4 条）

| 重开条件 | 判定 | 依据 |
|---|---|---|
| **① 独立连接 / 重连状态机意见** | **命中** | 归属层、触发点、预算与停止上界、复位契约、原生选项取舍全部定稿且可判定（§4.1 / §5）；ADR 0007 已落 |
| **② 无限流 EOS / UI 语义定稿** | **命中** | 终止路径穷举、区分契约、position / duration / seek / autoSkip / 混合列表全部定稿（§4.2 / §8）；ADR 0006 已落 |
| **③ 授权明确** | **未闭环，属图外** | 本图按 §10.4 只出清单 + 责任 + 三档归类，结论与归档由图外 P3-0 闭环；**不得带着「授权已明确」的前提开工** |
| **附加项：播放主路径挂死** | **未落地，属图外** | 补 `rw_timeout=3000000` 的动作在 P3-1；在此之前 `HlsExtractor::open()` 仍可能 15 s 挂死（§3.3）。**登记为未命中项，不得带着模糊前提宣布准入** |

**结论**：两项**技术**重开条件已命中，P3 实现 effort 可开工且不再需要语义裁决；③ 与附加项是**实现 effort 的准入检查项**，不是语义空白。若图外 effort 未落地这两项，P3 不得宣布整体准入。

> **P3 执行落地时点注记（2026-11-13，收口后审计补录）**：§11.2 表格、本节结论、§6.4 开头「本图不执行」、§12.1 接缝清单与 §14 验收矩阵中「授权复核与 `rw_timeout` 补齐登记为图外未闭环项」均为**定稿时点的准入判定快照**（提交 `4eab701e8`），不是实现状态真源。P3 图外执行票链 [#157](https://github.com/wt159/SoundBridge/issues/157) / [#158](https://github.com/wt159/SoundBridge/issues/158) / [#159](https://github.com/wt159/SoundBridge/issues/159) 已全部 CLOSED，实现 `efaddd3ff` + 审查修复 `c39ff451e` 经 [PR #160](https://github.com/wt159/SoundBridge/pull/160) 合入 master，master push 与 PR 共 6 个 CI run 全 `success` attempt 1。**附加项「播放主路径挂死」已闭环**：`HlsExtractor::open()` 已按 D15 三条约束增设 `rw_timeout=3000000`，15 s 挂死解除；**③「授权明确」仍为界外未闭环项**，按 §10.4 七项清单由人复核归档，整体准入仍不得宣布。实现真源以 `sdk/` 与 #158 / #159 收口 comment 为准；本规格既有行、表格单元格、判据数值与 `TIMEOUT` 一字不动，[#147](https://github.com/wt159/SoundBridge/issues/147) / [#156](https://github.com/wt159/SoundBridge/issues/156) 已挂更正 comment。

## 12. 文档接缝与更正（只加指针与注记，原判据与表格一字不动）

### 12.1 接缝回填清单（在 `doc/network-audio-source-spec.md` 落地）

| 位置 | 处置 |
|---|---|
| §4.2 裁决表 S3 / S4 | 表格单元格不动；表下新增「P3 接缝」注记，指向本规格 §4 / §9 |
| §11 P3 行 | 表格单元格不动；表下新增「P3 准入」注记，登记「两项技术重开条件已命中」「P3 不启用 FFmpeg 原生 `reconnect*` 家族」（三条理由见 §5.5） |
| §11 P1 行 / §5.3 前置能力 | 新增更正注记：「连接期与非播放主路径已落地，播放主路径待补」 |
| §3 三条已过期描述 | 新增「时点更正」注记：`interrupt_callback` 零命中 / `protocol_whitelist` 未设置 / 取消未贯通阻塞读点，正文表格按历史留痕不动 |
| §5.1 `AVSEEK_SIZE` / `seekable` | 新增措辞注记：两句**标为工程约定、无逐字出处**；`read_packet` 那句保留逐字引用 |
| §5.3 「超时归数据源层」 | 新增一句「播放主路径（`HlsExtractor::open()`）未设读超时，由图外实现 effort 补 `rw_timeout`」+ 指向本规格 §3.3 的指针 |
| `DataSource.hpp:173` `reconnectAtOffset` | 规格登记「历史虚接缝，P3 不使用」（本规格 §5.6） |
| `docs/research/live-source-facts.md` | **时点快照正文不回改**；更正落在 [#148 comment](https://github.com/wt159/SoundBridge/issues/148#issuecomment-6843635133)（已挂），本规格 §3.3 写明更正后结论与 `av_opt_copy` 通道 |
| [#143](https://github.com/wt159/SoundBridge/issues/143) | 已挂更正 comment（`PLAYLIST-TYPE:EVENT` 归 LIVE 放行） |
| `GLOSSARY.md` | **一字不动**（本图无新增词条，§2） |

### 12.2 更正后的事实口径

- 「P1 的 3 s 读超时在 http 上成立」→ **分路径成立**（§3.3）。
- 「`rw_timeout` 只在 EAGAIN 重试路径生效」→ **作废**；真实通道是 `avio.c:319-323` 的 `av_opt_copy(parent)` → `tcp_read` 的 `ff_network_wait_fd_timeout`。
- 「无限流没有自然终点」→ **不主动产生自然终点**（§2 / §6.2）。
- 「#143：EVENT 归 LIVE 侧拒绝」→ **归 LIVE 侧纳入**（§9.2）。
- 「`interrupt_callback` 零命中 / `protocol_whitelist` 未设置 / 取消未贯通阻塞读点」→ 三条**已过期**（§3.2）。

## 13. 不可推翻前提核对

| 前提（来源） | 本图处置 |
|---|---|
| 包为唯一层间单元、`positionMs` 归消费端（#42 / ADR 0003） | **一字不动**；诊断旁路是旁路，不承载数据 |
| 可解性不在提取器判定（#42 / ADR 0002） | **一字不动**；只分数据源层供给失败，坏帧仍由播放期汇总 |
| `m_abortDecode` 唯一取消信号、seek 只留 `seekToMs`（#42 / ADR 0003） | **一字不动**；分片轮询与重开 `interrupt_callback` 复用同一信号，`stop()` 序列不改 |
| 三态、错误不进状态面、回调单线程串行、七个控制入口 `void`（#75 / ADR 0004） | **一字不动**；重连期间零信号 |
| `durationMs==0` 的 seek 空操作、不引入能力位 / `isSeekable()`（#75 §7） | **一字不动**；`pos==0` 只加实现层短路（§8.4） |
| 不新增错误码 / 不改 `onError` / `error → state`（#75 §6） | **一字不动**；终局沿用 `FileOpenFailed` |
| 坏输入内容层拒绝唯一出口、55 例 / 396 断言（#22） | **一字不动**；网络失败归数据源层，两层不混 |
| 观测「不作为失败门禁」「不设任何数值阈值」（#94 / #104） | **一字不动**；12 s / 4 s 是策略参数不是门禁阈值，不新增记录面 |
| 14 个 `sdk_*` / `TIMEOUT` / 快全量集 / 覆盖度 / `ctest -j` 每进程只写自己的文件 | **一字不动**；判据不注册 ctest、不放宽 `TIMEOUT` |
| 两条路径并存互斥、按负载形态分派、`protocol_whitelist="http,tcp"`（#118 / ADR 0005 / #143） | **一字不动**；只把「拒绝」改「走原生 URL 层」 |
| 时点快照纪律：不写死公网 URL 与授权结论（上位规格 §10） | **一字不动**；§10.4 只出清单与责任 |
| 既有规格界外章节 | **补指针 + 措辞更正**，原判据与表格不动（§12） |

## 14. 验收判据

| 契约 | 判据 | 落点 |
|---|---|---|
| 生命周期状态机可判定 | §4.1 十行每行有归属 / 触发 / 预算 / 上界，且「预算耗尽是出口不是触发点」 | 本规格 §4 / §5（文档判据） |
| 区分契约可判定 | 「诊断旁路 + `nullptr` 语义不扩展 + 三条生命周期不变量」齐全，VOD 零行为变化 | 本规格 §6（文档判据） |
| 读超时三层定罪 | 三层定性表 + `av_opt_copy` 通道 + 两条硬约束 + 「3 s 表现为 EOF 不是超时错误」形态限定 | 本规格 §3.3（文档判据） |
| ADR 0006 已落、范围声明齐全 | 逐字声明不触碰 ADR 0002 / 0003，记录两个被排除方案 | [`docs/adr/0006`](../docs/adr/0006-live-source-read-failure-separation.md)（文档判据） |
| ADR 0007 已落、范围声明齐全 | 逐字声明不触碰 ADR 0002 / 0003 / 0006，记录三个被排除归属方案 | [`docs/adr/0007`](../docs/adr/0007-reconnect-state-machine-ownership.md)（文档判据） |
| 分派层放行条件 | 正面特征最小集、EVENT 翻转与 #143 更正、`#145` 两行的图外处置、variant 语义、ICY 复用诊断状态 | 本规格 §9（文档判据） |
| 无限流 UI 语义 | 三条终止路径穷举、`pos==0` 静默失败短路、position 停滞不违反单调、无数值上界 | 本规格 §8（文档判据） |
| 重连预算可判定 | 3 次 / 1 s / ≤12 s + 「总预算不是 stop 上界」+ 事件计数判据形态 | 本规格 §5.3 / §10.2 |
| LIVE / ICY 判据形态齐备且不进 CI | mock 三端点、独立可执行不注册 ctest、ICY mock 必要性、事件计数判据 | 本规格 §10（文档判据） |
| 三档归类逐形态填写 | HLS LIVE / ICY 两行三档齐全，规格不含写死的公网 URL 与授权结论 | 本规格 §10.3（文档判据） |
| 授权复核清单不含具体结论 | 七项清单 + 责任归图外 effort + 「本图不判已明确」 | 本规格 §10.4 / §11.2 |
| 三处更正 comment 已落、research 正文未回改 | `network-audio-source-spec.md` §5.3 / §11 P1 行、#148 comment；原判据数值 / 表格 / `TIMEOUT` 逐字未动 | 本规格 §12 |
| P3 准入判定 | 两项技术条件命中；授权复核与 `rw_timeout` 补齐登记为**图外未闭环项** | 本规格 §11.2 |
| 不可推翻前提 | §13 逐条「一字不动」 | 本规格 §13（文档判据） |
| 零改动 | 未改 `sdk/` / `app/` / CI / 用例 / 任何既有判据数值、`TIMEOUT` | 本规格 §15 |

## 15. 零改动声明与 fog 清空记录

**零改动声明**：本图只写 `doc/live-source-spec.md`、`docs/adr/0006-*`、`docs/adr/0007-*` 与 `doc/network-audio-source-spec.md` 的追加注记；`GLOSSARY.md` 一字不动。未改 `sdk/` / `app/` / CI 配置 / 用例 / 任何既有判据数值、容差、基线、`TIMEOUT`。#155 的实证探针在仓库外 `D:/tmp/sbprobe155/`，未入库。

**fog 清空记录**（地图 `Not yet specified` 已清空）：①「直播曲目的最长播放保护」→ [#150](https://github.com/wt159/SoundBridge/issues/150) 裁「不需要独立上限，终局唯一来自 stop / 切歌 / 预算耗尽」；②「重连失败的用户可见性」→ [#149](https://github.com/wt159/SoundBridge/issues/149) 裁「什么都不发」，[#150](https://github.com/wt159/SoundBridge/issues/150) 收口预算耗尽时序；③「ICY 内联元数据出口」→ [#152](https://github.com/wt159/SoundBridge/issues/152) 裁「不提供」并写入界外（§8.7）。
