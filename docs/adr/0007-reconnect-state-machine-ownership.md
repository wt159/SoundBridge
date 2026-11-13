# 0007 重连状态机归解码线程、FFmpeg 原生 reconnect 不托管

- 状态：已接受
- 日期：2026-11-13
- 来源：[地图：直播源（HLS LIVE / ICY）生命周期与无限流终止语义](https://github.com/wt159/SoundBridge/issues/147)、[重连状态机的归属层与触发点](https://github.com/wt159/SoundBridge/issues/149)、[重连策略、可取消性与停止上界](https://github.com/wt159/SoundBridge/issues/150)；规格落点 [`doc/live-source-spec.md`](../../doc/live-source-spec.md) §5。事实基线见 [#148](https://github.com/wt159/SoundBridge/issues/148) 产出的 [`docs/research/live-source-facts.md`](https://github.com/wt159/SoundBridge/blob/research/live-source-facts/docs/research/live-source-facts.md)。

## 背景

无限流必然会遇到供给中断：HLS LIVE 的窗口耗尽 / segment 404 / `m3u8_hold_counters` 耗尽一律以 `AVERROR_EOF` 收场，ICY 的 TCP 断流表现为 EIO / EOF。上位规格 `doc/network-audio-source-spec.md` §11 把「独立连接 / 重连状态机 effort」列为 P3 的重开条件，但没有裁定它归哪一层。

现状有四个候选落点，且各自看似合理：数据源层有 `DataSource::reconnectAtOffset(off64_t)` 这个「现成」虚接缝；提取器独占 `AVFormatContext *m_fmt` 与 private `open()`，看起来最适合自愈；解码线程持 `m_abortDecode` 并消费包；播放器层能重走 `start()`。同时 FFmpeg 4.4 的 http 协议自带 `reconnect` / `reconnect_streamed` / `reconnect_delay_max`，看起来可以直接托管。

逐行复核推翻了多数直觉：`reconnectAtOffset` 挂在 `DataSource` 而所有持有者持的是 `DataSourceBase *`，生产调用点为零，三个生产子类均未 override，`UrlSource::readAt()` 恒返 `-1`；`start()` 缓存了 `m_srcSpec` / `m_codecID` / `m_durationMs`，而 `open()` 会重算 `m_consumed` / `m_audioIndex` / `extradata`；原生 `reconnect*` 既不下传 hls 的 segment 请求，也不受自有取消信号控制。

## 决策

**状态机归解码线程 `AudioStreamDecoder::runFfmpegPacketLoop`；`HlsExtractor` 只提供一个无状态的「重开原语」（`closeFmt() + open()`）；P3 显式不启用 FFmpeg 原生 `reconnect*` 家族，重连由自有状态机驱动。**

- **决策与执行分离**：状态机（触发判定、预算、退避、等待、终局）在解码侧；重开动作在提取器侧，不持预算、不 sleep、不重试、不判可解性。理由：触发点已被 #151 钉在解码侧（`nextPacket()==false` 后查诊断状态），`m_abortDecode` 与预算计量都在该层；而 `m_fmt` / private `open()` 被提取器独占，解码侧没有重开能力。
- **单一入口**：读失败 / 供给停滞经同一入口进重连；**读超时不设独立触发源**（#155 证明它被 hls 洗成 EOF，本来就经同一入口）；**预算耗尽是终局出口，不是触发点**（防自激重连）。
- **可取消性**：重连等待以 100 ms 分片轮询同一个 `m_abortDecode` 实现；重开仍走既有 `interrupt_callback`（转发同一信号）；`stop()` 的「置位 → `join()` → 解绑」序列一字不动，**不引入第二取消信号**。
- **形态无关**：HLS LIVE 与 ICY 走同一状态机，不按形态分叉、不新增形态位；恢复语义差异（HLS 可续窗口 / ICY 必丢数据从 offset 0 开始）只写规格表格。若 ICY 走另一条提取器，该提取器**必须提供同形状的只读诊断状态**——[#152](https://github.com/wt159/SoundBridge/issues/152) 后来裁定复用同一直通提取器，此要求自动满足。
- **复位契约**：重开后复用 seek 分支的 `cursor.reset` + 重建 packet decoder，不重走 `start()`、不重建整个 decoder、不重发回调。**不复位 `PacketCursor` 会让 `packetAt(index)` 的 `index != m_consumed` 直接返回 `nullptr` → 立刻 EOS**。**重连成功的准入不变量** = 重开后 `AudioSpec` / `codecID` / `durationMs` / `extradata` 与首次 open 一致；不一致判本次失败、计入预算，不做隐式规格切换。
- **对外零信号**：重连期间三态不变、不发 error、不回退 position、不进 EOS，成功也静默；只有预算耗尽走既有 `error → state`（`FileOpenFailed`）。
- **策略**（#150）：有限重连 3 次、固定间隔 1 s（100 ms 分片 ×10）、不指数退避；单次窗口 ≤ 4 s、总预算 ≤ 12 s（含重开窗口）；**`stop()` 上界是 100 ms 取消粒度，与总预算解耦**（这两个数是策略参数，不是 CI 门禁阈值）。
- **历史接缝**：`reconnectAtOffset` 登记为「历史虚接缝，P3 不使用」，**废弃但不删、不改签名**（`off64_t offset` 对无限流无意义；改公开虚接口属越界；删除会动 `test_extractor.cpp:77` / `:348` 两处测试 override）。

**范围声明（逐字）**：本 ADR **只管「消费侧重连状态机的归属与原生选项取舍」**；**不触碰 ADR 0002**（可解性仍只在播放期判定，重开是供给恢复动作）、**不触碰 ADR 0003**（包仍是唯一层间单元，重开原语不承载数据）、**不触碰 ADR 0006**（提取器出口的读失败与流结束分离由后者单独裁定）。ADR 0006 管出口，本 ADR 管消费侧，两者正交。

## 考虑过的替代方案

**(a) 数据源层，复用 `reconnectAtOffset`。** 表面改动最小。否决：接缝挂在 `DataSource` 而 `HlsExtractor::m_source` 与 `AudioStreamDecoder` 拿到的都是 `DataSourceBase *`；生产零调用、三个生产子类均未 override；`UrlSource::readAt()` 恒返 `-1`，直通路径不经 readAt 供数，原生 URL 层的失败对数据源不可见。这条接缝连「够得着」都不成立；要救它得先改公开虚接口签名，属越界。

**(b) 提取器层全权自愈（`packetAt` 内部完成重开 + 退避，对解码侧透明）。** 拥有 `m_fmt` 与 `open()`，看似最内聚。否决：`packetAt` 是同步单包接口，内嵌 sleep / 预算会卡住解码线程；「对解码侧透明」与 #151 已裁的「解码侧查一次诊断状态」直接冲突；把重试状态机塞进包工厂会擦 ADR 0003 的边界。故只保留其**执行**能力，不保留状态机。

**(d) 播放器层重开曲目（`MusicPlayer` 重走 `start()`）。** 实现最省。否决：语义上等价切歌，会重置 duration / position 并污染 `doc/player-state-contract.md` §2.2 三态与 §3.1 回调顺序；还会把 `stop()` / 切歌与重连混成同一路径。

**托管给 FFmpeg 原生 `reconnect*`。** 只需开 `reconnect=1` + `reconnect_streamed=1`。否决三条：① **预算不可计量** —— 退避 1→3→7… 与 `reconnect_delay_max=120 s` 在 FFmpeg 内部，可判定的停止上界落不了地；② **取消不可打断** —— 原生退避不受 `m_abortDecode` 控制；③ **覆盖不全** —— hls 的 `save_avio_options` 只下传 7 项（`hls.c:1716`），`reconnect*` 不在其中，segment 是全新 `URLContext` 回到默认 0，只可能覆盖 playlist 层。另：流式源重连回 **offset 0** 会重播已播内容；`http_persistent=1` 使重连判定只能建立在「读失败事件」上，不能建立在连接对象上。

## 后果

- `doc/live-source-spec.md` §5 是本决策的契约落点；本 ADR 只记「为什么归解码线程 / 为什么不用原生 reconnect / 为什么不复用历史接缝」，不重复数值条款。
- #150 的预算计量位置因此固定在 `runFfmpegPacketLoop`；#155 的 `rw_timeout` 补齐与「3 s 表现为 EOF」形态限定成为 #150 停止上界的输入；#153 的「重连成功 / 失败都不改变 `duration()`」由本 ADR 的规格一致准入不变量保证。
- 公开面零变化：三态、回调顺序、错误码集合、能力位、`stop()` 序列全部不变；不新增第二取消信号。
- **反悔通道**：若日后要把状态机移到提取器层或改托管原生 reconnect，须另开新票，同时处理本 ADR、#150 的预算计量位置、#153 的对外语义与 #155 的触发集回填；不得只改一处留下「触发在解码侧、预算在提取器」的分裂状态。
- 本图未改任何生产代码 / 用例 / CI / 既有判据数值；实现按规格 §11 的 P3 票链另开图外 effort。
