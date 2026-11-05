# 本地文件边解边播规格

> 来源：wayfinder 地图 [#42「本地文件真·边解边播的规格与架构决策」](https://github.com/wt159/SoundBridge/issues/42)，
> 由 #43–#54 十二张票的决策折叠而成。2026-11-05 收口。
>
> **本文件是规格，不是实现。** 用例、断言与代码改动走图外执行票；实现清单见第 9 节。
> 术语以 [`GLOSSARY.md`](../GLOSSARY.md) 为准；难逆转的架构决策见
> [`docs/adr/0003`](../docs/adr/0003-incremental-supply-contract.md)。

## 1. 目的地与范围

让本地音频文件的播放满足**边解边播**这一数据供给契约：解码与消费并行推进，播放期间为当前曲目持有的音频数据量不随素材时长增长。

- **只含本地文件**。网络流 / URL / HTTP 数据源 / 边下边播 / 断线重连不在范围内。
- 播放器状态机与 app UI（`PlayerState` 三态、`durationMs == 0` 的显示与 seek 禁用、事件回调语义）不在范围内，留给独立地图。
- 设备抽象 `IAudioSink`、听感与重采样质量、格式扩展与提取器插件化不在范围内。
- 用例与断言的实现不在本图内；本文件只给判据与决策。
- **#8「support stream programme」保持 OPEN** 作目的地锚点。其正文 *Streaming decoding and playback scheme for local music players* 讲的是本地文件边解边播，旧文档里的「网络流」是措辞偏差。

### 1.1 现状基线（2026-11-04 实测）

生产链 `MusicPlayList::startStreaming()` 直接 `AudioStreamDecoder::start(extractor)`（`MusicPlayList.cpp:346-378`），逐帧 PCM 写 256 KB ring，**不经过 `AudioDecodeProcess`**；全量 PCM 只存在于测试 / 探针用的 `AudioDecodeProcess`。真正随音频长度增长的是**整首压缩 payload 全量常驻**：extractor init 逐包存入 `m_packetizedMetaBuf`，`getMetaData()` 再拼一份等长缓存。所以本次改造要取消的是「整首压缩 payload 常驻」，不是「全量 PCM 解码」。

## 2. 术语

canonical 术语是 **「边解边播」**：一种数据供给契约，解码与消费并行推进，播放期间为当前曲目持有的音频数据量不随素材时长增长。它不是可切换的播放模式，也不表达音频来源是否来自网络。

`_Avoid_`：流式、流式播放、流式解码、网络流。代码符号（`startStreaming` / `AudioStreamDecoder`）保留历史命名，属于已知代价，不回改。

其余词（包、提取器、标称时长、解码时长、坏数据降级）沿用 `GLOSSARY.md`，本文件不另造术语。判据口径（峰值内存、首播延迟、稳态窗口、切换开销）含实现细节，**不进词表**。

**验收口径**（#46）：播放期间 SDK 为当前曲目持有的音频数据（压缩 payload 与 PCM）峰值字节数与素材时长无关，只由固定容量 ring、单个包和解码器内部缓冲决定；且解码在消费端取走 PCM 的同时继续推进。两个反例：① 素材时长翻倍而峰值内存随之增长 → 不满足；② 解码必须整体结束才有 PCM 可消费 → 不满足。

关键区分：**读过 ≠ 常驻**。允许启动前扫全文取标称时长，不允许整首压缩数据留在内存；#47 Q9 进一步收紧为「任何随包数增长的索引 / 时间轴结构都不允许常驻」。

## 3. 层间契约（#47）

| 层 | 持有状态 | 谁推进 / 传递单元 | 失败如何传播 |
|---|---|---|---|
| 提取器 / 包工厂 | 格式能力、声明 spec、标称时长、`DataSource` 句柄；init 期只读扫描**只留 O(1) 定长摘要** | 被解码线程 pull `nextPacket()`，不主动推；单元 = **单个压缩包**（边界仍是音频负载唯一表示） | 上抛为解码错误，不新增错误码 |
| 解码器 | demux 句柄（`fmt` / `avioCtx` / `AvioData`）、首帧实测权威 spec、惰性重采样器 | 解码线程独占、播放期存活；`availableWrite < minWriteSpace` 时暂停 demux；逐帧 PCM 写 ring | 单一汇总 `RunDecodeResult` → `setError` → `handleDecodeError`；EOS = 包耗尽 + ring 排空 |
| ring + 消费端 | 单调 `m_readPos`（本段内计数）、`segmentBaseBytes`、设备 spec | 10 ms 定时 push 读 4096 B（驱动模型不变）；首播阈值 = ring 可读量 ≥ 4096 B | 只消费不判错 |

- **包为唯一层间单元**，demux 句柄从 init 期栈上局部移到解码侧、播放期存活；`nextPacket()` 单包迭代取代全量 vector；`getPacketizedMetaData()` 的全量 vector 语义废止。
- `getMetaData()` 退出生产链（仅测试量测仪器可用）；`getPacketizedMetaData()` 保留测试专用；`AudioDecodeProcess` 保留为量测仪器，改为遍历包工厂拼整段；**不新增同步 `decodeNext` / `pullPacket` 入口**（#18 继续成立）。
- `positionMs = (segmentBaseBytes + m_readPos) / bytesPerMs`：`segmentBaseBytes` 是消费端 O(1) 段基准、seek / 切歌时更新，**不是 ring 成员**（否则 `reset()` 归零 `m_readPos` 会让位置回 0）。`durationMs` 由提取器 init 期给出、播放期间不变、允许 `0` 表示未知。
- 背压只停在解码线程（写 ring 处等待，连带暂停 demux）；首播阈值 4096 B；**不改消费端 10 ms 定时 push**；高 / 低水位具体数值属实现参数。
- 首帧实测 spec 无条件覆盖声明 spec、惰性重采样器留解码器；失败与 EOS 由解码侧**单一汇总**。
- 五种「不可增量」格式（m4a / asf / wma / ape / mkv）统一契约、init 期可只读扫描，但**索引与逐包时间轴产物必须 O(1)**，播放期按 offset 现场重算包边界。

## 4. 生命周期、所有权与线程约束（#51）

### 4.1 状态迁移表

| 事件 | 前置 → 后置 / 动作 | 拒绝与合并规则 |
|---|---|---|
| `play` | IDLE / STOPPED → PLAYING：设备 `open` → `start` → `startConsumer`；创建当前任务 ring + decoder，`start()` 先 stop+join 旧线程 | shuttingDown 全拒；stop 期间排队到 stop 完成后执行 |
| `stop` | PLAYING / PAUSED / SEEKING → STOPPED：`stopConsumer` → `clearQueue` → 设备 `stop` / `close`；清 ring、**不 drain** | 幂等；旧 PCM 全丢弃 |
| 切歌（含自动跳轨） | PLAYING / ERROR → SWITCHING → PLAYING：`clearQueue` → 更新 `segmentBaseBytes` → **新任务 `start()` 之前销毁旧 ring**；新曲首帧后重新起算稳态窗口 | switching 期间 seek 显式丢弃；EOS / 自动跳轨沿用一次门闩，按 `ErrorAction::SkipTrack` 触发（**不看 recoverable**） |
| `seek` | PLAYING → SEEKING → PLAYING：控制线程置 `m_abortDecode` / `m_seekRequestMs`，解码线程在 consumer 静默窗口内 `reset()` ring 并更新 `segmentBaseBytes`，同线程轮询重放、**不 join** | 连续 seek 后一次覆盖前一次；`durationMs==0` → 显式 `INVALID_OPERATION`；IDLE / ERROR 早退 |
| EOS 汇总 | 解码侧单一汇总，包耗尽 + ring 排空后发一次 | 不新增错误码 / 时机；已消费 PCM 算数、不回滚不重播 |

基准 = `MusicPlayerState`；`PlayerState` 与 `StreamDecoderState` 只作投影 / 诊断，**不新增状态机**。

### 4.2 生命周期硬约束

控制事件经 WorkQueue 单线程串行、每次 `start()` 新建解码线程且先 stop/join、seek 同线程轮询不 join，是唯一形态。`m_abortDecode` 是**唯一取消信号**（`RunDecodeResult::Abort` 只是结果），只由控制事件所在 WorkQueue 线程置位；**join 必须由发起新任务的同一线程在新任务启动前完成，禁止 detach / 后台 join**。自动跳轨不新增并发机制：旧解码线程 join 允许延后到下一次切歌或析构，但旧任务必须在 EOS 汇总后停止写 ring、且不得持有新曲 ring。

### 4.3 所有权清单

| 对象 | 创建者 | 拥有者 | 销毁 / 清理时机 | 跨曲目延续 |
|---|---|---|---|---|
| `AudioRingBuffer`（262144 B） | 消费端 | **当前播放任务独占** | 新任务 `start()` 之前引用计数归零；stop / 切歌 / seek 时 `reset()` 清数据 | **否**，不得复用实例 |
| `AudioStreamDecoder` | `MusicPlayList` | 当前任务 | 随任务；join 由发起新任务的线程在启动前完成 | 否 |
| demux 句柄 | 解码侧、播放期创建 | 解码器 | 解码任务退出时释放 | 否 |
| `MusicProperties` / prev+next 预缓冲 | `MusicPlayList` | `m_musicListProperties` + `m_curMusicProperties` | 引用计数归零 | **只留曲目元数据**（路径 / spec / 标称时长），不留 ring / decoder 实例 |
| 设备队列 / `AudioDevice` | `MusicPlayer` | 播放器 | `clearQueue` + `close` | 否 |
| `segmentBaseBytes` | 消费端 | 消费端 O(1) 段基准、**非 ring 成员** | 随任务 | 否；seek / 切歌时更新 |

旧任务软取消后已写进 ring 的 PCM **一律丢弃、不播完**（seek / 切歌 / stop 都清 ring 与设备队列）。切歌窗口不进首播判据，单独记「切换开销」、不设阈值（与 init 扫描耗时同构），稳态窗口从新曲首帧交付后重新起算。

### 4.4 线程约束清单

| 接口 / 状态 | 允许线程 |
|---|---|
| ring `write()` / `nextPacket()` / demux 推进 | **解码线程**（唯一生产者） |
| ring `read()` / 10 ms `consumeAudioData` / 静音填充 | **WorkQueue 消费线程**（唯一消费者） |
| ring `reset()` | **仅解码线程**，且 consumer 处于 SEEKING / switching 静默窗口、不读 ring；与 `segmentBaseBytes` 更新同窗口完成 |
| 控制事件 `play` / `stop` / 切歌 / `seek` / 自动跳轨 | **WorkQueue 线程**（串行）；UI 线程只投递 |
| `AudioDevice` `open` / `start` / `stop` / `close` / `clearQueue` | **WorkQueue 线程**（析构例外） |
| `m_abortDecode` 置位 / `join()` | 控制事件所在 WorkQueue 线程；join 在新任务启动前 |
| `MusicProperties` / signal 字段 / decoder state / `errorDetail` | `m_curMusicMutex`、atomic 字段、`m_errorMutex`；ring 本身无 mutex（SPSC） |

不改 10 ms push 驱动模型、不给 ring 加 mutex。`AudioRingBuffer.h:7-9` 的「reset 仅两线程静止」注释与现状不符，须由图外执行票更新。

## 5. 判据（#48）

| 判据 | 量测对象 | 表达式 / 失败条件 |
|---|---|---|
| 常驻上界 | 分配记账峰值：音频数据 + **一切随包数增长的结构**（索引 / 页表 / seek 表） | `peak ≤ 262144(ring) + P + 64KB(AVIO) + O(1) 摘要`；且 15 行任意长短素材对 `\|peak_long − peak_short\| ≤ P`。`P` = 15 行实测最大包字节数 ×2。超式或差值 > P 即红 |
| 并行 | 首帧 PCM 交付时刻的解码状态 | 交付首帧时解码未整体结束即绿；必须先解完才有 PCM 即红 |
| 首播延迟 | `AudioStreamDecoder::start()` → 设备首次 `write()` | 长短素材对该延迟差 `≤ 150ms`（复用既有绝对项；1% 相对项因延迟无标称基准不适用，**不另立容差**）。init 只读扫描耗时单独记「启动开销」，不进判据、不设阈值 |
| 欠流 | 稳态窗口（首帧后 → EOS 前，排除启动 / seek / 切歌）静音填充次数 | `== 0` 即绿；窗口外显式豁免 |

- 量测靠**新增分配记账**，不用进程 RSS；素材只用现有 15 行长短配对（3.7 s ogg ↔ 259 s wma，约 70 倍），不合成长音频。
- 判据落独立 suite（如 `StreamingBudget`，预计 ≤3 用例），由 `ctest -R sdk_` 覆盖，与 `MediaCoverage`（好文件 1%）/`BadInput`（坏文件 5pp）**三分**，不缠既有基线。
- 规格要求新增四个观测点：分配记账峰值、稳态静音填充计数器、start→首帧时间戳、init 扫描耗时；实现交图外执行票。
- **观测点覆盖边界（已知代价）**：`shouldFillSilence()` 不含「ring 空」条件，`got==0` 时静默跳过本轮、不填静音。因此静音计数器只覆盖 shuttingDown / switching / SEEKING 三类窗口，欠流判据实际判的是「稳态窗口内静音填充为零」，**不是「无欠流」**。不推翻 #48 定案，但规格必须写明，避免实现者误以为计数器抓得到欠流。
- 高 / 低水位具体数值属实现参数；m4a / mkv 现场重算包边界的 CPU 成本上界不在本规格设阈值，只记为独立指标；端到端 drift 仍未判，留作后续 effort 输入。

## 6. seek 策略（#49）

| 档 | 格式 | 入口 | 精度 | 重解整首 | 内存约束 |
|---|---|---|---|---|---|
| native RA | FLAC / ogg-FLAC / Vorbis | `seekToSample` / `ov_time_seek`，播放期解码器实例原地定位 | 样本级 / 秒级 | 否 | demux 播放期存活、索引 O(1) |
| 顺序重放 | 其余 9 种 | 重启解码任务，`nextPacket()` 从起点按包迭代到目标、丢弃目标前 PCM | 受下述判据约束 | 是（不重建整首 payload 缓存） | 峰值不随目标位置增长（常驻上界） |

- 对外只保留按时间 `seekToMs`，目标 = 期望的已消费 PCM 时间（与 `positionMs` 同源）；不引入按包 / 按采样的第二套 API，也不引入相对定位。
- `durationMs == 0` → 显式返回现有 `INVALID_OPERATION`（`pos == 0` 例外）；目标超出可解内容 → EOS 汇总 `DataTruncated`。**降级只有这两类，不写「不支持 seek」**，seek 不新增错误码。
- 精度：① `|实际首帧时间 − target| ≤ max(target×1%, 150ms)`（复用 `RealMediaFixture::coverageToleranceMs()` 形式、不新造数值）；② `|seek 后交付 PCM 字节数 − 整段解码从目标点起的期望字节数| ≤ 同一容差折算字节数`。**不逐位一致**（现有零容差差分比的是累计标量，PCM 在 `drainUntilSettled()` 丢弃；逐位需新增能力且有损格式会假红）。
- SEEKING 静音沿用 #48 豁免；ring reset 时序、join、`Abort` 传播归第 4 节；EOS 用 `m_seekBaseMs + 解出时长` 与标称时长比，`durationMs == 0` 时 `haveNominal=false` 不报 `DataTruncated`。

## 7. 对坏输入规格的影响（#50）

[`doc/bad-input-spec.md`](bad-input-spec.md) 在增量路径下**整体成立、四条基石零改写**（B1 按解出多少帧不按格式分叉 / B3 错误码只表达原因 / B2 拒绝唯一出口 `createExtractor()==nullptr` / B4 量测走播放路径）。B4 反被加强：#47 后生产链与量测仪器同构。

- **唯一硬约束**：#47 新增的 init 期只读扫描**不得把可解性当判据** —— 坏包跳过继续扫、标称时长允许 `0`、扫描失败不返回 `nullptr`；工厂仍只按 `initCheck()` + 两个硬不变量拒绝。否则会给 B2 开第二个拒绝出口。
- 原 init 期判死的 m4a `moov` 断 / ogg 页链断，改判「播放期包耗尽 + ring 累计 0 字节 + EOS 汇总」，属 B1 兜底不是第二出口。
- EOS 原样成立、不新增时机，已消费 PCM 算数、不回滚不重播；零容差差分原样成立、不增能力（不扩展到 PCM 逐位）；「不可检测」清单零增删；**#48 的新观测点禁用作坏输入旁证**。
- §2.1 覆盖对照表不增行、不增用例，只补两处注（结构性不覆盖段的增量接缝说明；「上报链路与交互」行断言列补 `m_seekBaseMs + 解出时长`）。55 用例 **53 条原样、2 条只移断言位置**，零条换量测仪器、零条新增。

## 8. 回归基线迁移（#52）

| 原判据 / 基线 | 是否仍成立 | 替代 / 保持 | 用例改动 |
|---|---|---|---|
| 15 行 declared/measured 双向容差 `max(1%,150ms)` 且 ≤25% | 成立 | 仪器保留 `AudioDecodeProcess`，改为遍历包工厂拼整段；`measuredMs` 仍由 `getDecodeSpec()` 换算 | 0 |
| `good` 本次运行内实测 / `r × good ± max(5%×good,150ms)` / 5pp / 零容差差分 | 成立 | 数值全部原样 | 0 |
| P2 组 `ogg truncation` / `m4a moov` 的「工厂拒绝」断言 | 不成立 | 播放期包耗尽 + ring 0 字节 + EOS 汇总 | **2 条只移断言位置** |
| 其余 53 条 BadInput | 成立 | 不动 | 0 |
| #48 四条判据 / #49 seek 精度 | 新增 | 落独立 suite + 四个观测点 | 新增 ≤3 条 |

- **迁移顺序 = 线性四步，不并行双跑**：① 加四个观测点 → ② 切增量实现 → ③ 迁移 2 条断言 → ④ 全量回归。仓库无 feature flag / 双跑机制，新增机制成本高于收益。
- **CI 不拆快集 / 全量集**（沿用「决定不削用例」）；`TIMEOUT 180` 只挂在 `sdk_unit_tests`，保持不动；不新增耗时 gate；`Report coverage matrix numbers` 不改。
- **事实更正**：`sdk-coverage.yml` 实际跑的是 `cmake --build build --target coverage` + `UnitTests -ts=MediaCoverage -s`，**没有** `ctest -R sdk_` 命令；8 组 `sdk_*` 是本地 CTest 注册。
- 新增 suite 只需在 `UNIT_TEST_SOURCES` 加一个 .cpp（doctest 自动注册），不新增 `add_test` 分组。

## 9. 实现清单（按优先级分级并编号）

> 本表是**唯一的编号表**，由 #53 统一给出；#54 只提供顺序依据，不另造编号。每条交图外执行票，不在本图内落地。

| 编号 | 优先级 | 条目 | 主要依据 | 阻塞于 |
|---|---|---|---|---|
| E1 | P0 | 包工厂与 `nextPacket()` 单包迭代；全量 vector 语义废止、`getMetaData()` 退生产（测试专用保留） | #47 | — |
| E2 | P0 | demux 句柄移解码侧播放期存活；init 期只读扫描容错 + 只留 O(1) 摘要 | #47 / #50 | E1 |
| E3 | P0 | `positionMs` 单轨 + `segmentBaseBytes` 落地（消费端 O(1) 段基准） | #47 / #49 / #51 | E1 |
| E4 | P0 | ring 任务独占 + prev/next 预缓冲退化为只留元数据 + 静默窗口 `reset()` + 状态门槛与 join 约束 | #51 | E1 |
| E5 | P0 | 四个观测点（分配记账峰值 / 稳态静音计数器 / start→首帧时间戳 / init 扫描耗时） | #48 | E1 |
| E6 | P0 | 独立 suite `StreamingBudget`（≤3 用例）与 #49 seek 精度用例 | #48 / #52 | E5 |
| E7 | P1 | `.flac` 增量落地（首张 MVP，验 #46 两反例 + 常驻上界与并行） | #54 | E1–E6 |
| E8 | P1 | `.wav` / `.aiff` | #54 | E7 |
| E9 | P1 | `.mp3` / `.aac` / `.ogg` | #54 | E7 |
| E10 | P1 | `.mkv` | #54 | E7 |
| E11 | P1 | `.m4a` / `.ape` / `.asf` + `.wma`（现场重算包边界 + 扫描容错） | #54 | E10 |
| E12 | P2 | 2 条 BadInput 断言位置迁移（`ogg truncation` / `m4a moov`） | #50 / #52 | E2 |
| E13 | P2 | 按格式 seek 策略落地（native RA / 顺序重放两档） | #49 | E7 |
| E14 | P2 | `AudioRingBuffer.h:7-9` 注释更新、`doc/流式方案介绍.md` 偏差清理 | #51 / #53 | — |
| E15 | P2 | CI 观测与端到端 drift 测量（不设阈值，后续 effort 输入） | #48 / #52 / #54 | E6 |

`nextPacket()` 全面替换 vector、`m_readPos` 语义变更属于**宽重构**：按 expand–contract 分批（先并存、再逐目录迁移、最后删除旧形式），不要硬塞进单个垂直切片。

## 10. 从决策到实现：后续路线

本图是**规划图**（wayfinder 的 Plan 默认），规格是决策折叠的终点，不是构建票。要把上面的清单变成代码，按以下三段接上主流程：

### 10.1 `/to-spec` —— 先把 linked decisions 折叠成可建设计划

以本文件与地图 #42 的 Decisions so far 为输入，产出一份面向实现的 spec（Problem Statement / Solution / User Stories / Implementation Decisions / Testing Decisions / Out of Scope / Further Notes），发布到 tracker 并打 `ready-for-agent` 标签。用 `GLOSSARY.md` 的词汇，遵守 `docs/adr/` 里的 ADR。**已发布为 [#55](https://github.com/wt159/SoundBridge/issues/55)**；测试接缝定为「单一播放路径接缝」（`AudioStreamDecoder::start()` → 消费端 PCM 交付），四个观测点只作同一接缝上的仪表。

**不要把 wayfinder 地图直接 loop 进 `/implement`**：地图里十一张票的 linked detail（为什么索引必须 O(1)、为什么 init 扫描不能判可解性、为什么 `segmentBaseBytes` 不是 ring 成员）会在直接跳进实现时丢掉。`/to-spec` 的作用正是把这些约束压进一份可独立阅读的计划。

### 10.2 `/to-tickets` —— 拆成带阻塞边的 tracer-bullet 票

把 spec 拆成**垂直切片**（每条自成一个可验证的行为，穿过接口与测试），而不是按层横切（不是「先改所有 extractor，再改所有测试」）。每条票声明 **Blocked by**，用 GitHub 原生 blocking / sub-issue 关系，并打 `ready-for-agent`。可以把第 9 节的 E1–E15 当作骨架，但需按垂直切片重新分组：例如「FLAC 一首曲目从 play 到首帧 PCM，峰值内存满足上界」是一条 tracer bullet，而「实现 `nextPacket()`」不是。宽重构按 9 节的 expand–contract 顺序单独排。

发布前与人类确认三件事：粒度是否合适、阻塞边是否只保留真正门禁的边、是否有票需要合并或再拆。

### 10.3 `/implement-spec` —— 一次并行跑完实现

读 spec 与 ticket **task graph**，在 ready frontier 上**并行**派 implementer 子 agent：

1. 建 integration branch；每个 implementer 在自己的 worktree / 分支上工作，确认基于 integration branch tip；
2. 每个 implementer 调 `/tdd` 逐红绿切片实现，报告前把 integration tip 合进自己的分支；
3. 子 agent 完成后由 merger 子 agent 合入 integration branch；frontier 变化就继续派发新子 agent；
4. 全部完成后在 integration branch 上跑**一次** `/code-review`（Standards + Spec 两轴），把问题交给单个 implementer 子 agent 一次修完；
5. 打开 / 标记 PR、按 tracker 的方式关票、报告 integration branch，最后清理所有 worktree。

### 10.4 前置条件与边界

- 地图 #42 已关闭、第 9 节编号已落地、`GLOSSARY.md` 的「边解边播」词条已提交。
- 跨平台改动（条件编译 / POSIX 头 / 文件路径 API）必须等 Linux `sdk-coverage` 编译过；本地 Windows 全绿不算完成（根 `AGENTS.md` 约定）。
- CI 仍按 #52 定案：不拆快集、`TIMEOUT 180` 不动、不新增耗时 gate。
- 实现属**图外新 effort**：本图 Notes 明确把用例与断言实现、播放器状态机、网络流划到界外；这些不要混进 `/to-tickets` 的票里。
- 仓库约定：CRLF、`git add` + `commit` + `push`、提交信息前缀 `feat:`/`fix:`/`build:`/`docs:`/`chore:`；Grep 必须限定 `path`（`sdk/3rdparty` 有 8 万+ 已追踪文件）。

## 11. 已知代价与事实更正

- 「边解边播」是规范术语，但代码符号仍是 `startStreaming` / `AudioStreamDecoder`；不回改，记为已知代价。
- `segmentBaseBytes` 全仓业务代码**未落地**（现状走 `m_consumedBytes` / `curDataOffset`）；它不是现状 bug，而是按 #47 契约改造后的缺口，归 E3。
- `shouldFillSilence()` 不含「ring 空」条件、`got==0` 静默跳过；`m_seekDiscardBytes` 仅通用解码分支设值（FLAC / Vorbis 分支清零）；`AudioRingBuffer.h:7-9` 注释与现状不符。
- 首播判据不含 init 扫描与切歌窗口，端到端体验仍有随文件大小增长的**启动开销**与**切换开销**；两个指标只记录、不设阈值，drift 测量留作后续 effort 输入。
- 稳态欠流判据假定测试环境消费端不按实时墙钟节流；若将来改水位驱动或出现真实解码慢于消费的用例，须回来重开此判据。
- 两处文档偏差已回填：`doc/bad-input-spec.md` 的「1 ms 轮询」→ 5 ms、`#8` 的「网络流 / 边下边播」措辞更正。
