# research #166：停发分片（冻结 playlist 窗口）注入形态与 hls 无包重载规避

- 票：[#166](https://github.com/wt159/SoundBridge/issues/166)（地图 [#164](https://github.com/wt159/SoundBridge/issues/164) 子票）
- 执行票：[#171](https://github.com/wt159/SoundBridge/issues/171)（本票只调研，实证与落地留 #171）
- 基线：master HEAD `ab5be374d`，工作树干净；本票**严格只读**（不修改仓库任何文件、不改规格/票正文/旧 resolution、不关票、不提交、不建分支、不 push、不做代码实测）
- 范围约束：零规格改动、零公开 API、零新增 ctest 目标、零 `TIMEOUT` 放宽、零数值门禁/观测字段；`Grep/Glob` 限定 path 且排除 `sdk/3rdparty`；不用 CodeGraph MCP
- 产出纪律：不输出长篇原文，只给关键行与判断；无法确认项显式标注「**未确认**」；每条判断回指一手来源（源码 / 规格 / 票面）

---

## 0. 规格锚点与口径基线

- `doc/live-source-spec.md:289` §10.1② 原文三类可注入断流形态：**「停发分片 / 分片 404 / 发完响应头不发 body 触发 3 s 墙钟）」**，并写「覆盖 §5.3 的两个失败面」。
- P3-3（票 [#162](https://github.com/wt159/SoundBridge/issues/162)，`ab5be374d`）只补齐后两类 + 首次 open 超时面；「停发分片」自 P3 定稿 `4eab701e8` 起就是**既有覆盖缺口，非新规格要求**（#166 body、#171 body、#162 body 均如此声明）。
- §5.2（`:127-133`）定义重连**单一入口触发集**：`nextPacket()==false` 后查一次诊断状态，判为「供给停滞（LIVE 的 `AVERROR_EOF`）」或「数据源层失败（EIO / HTTP 错 / playlist 重载失败）」才进重连；`AVERROR_EXIT` 取消优先、不进重连；判定**只能建立在读失败事件上**。
- §5.3（`:135-149`）是「预算与停止上界」表（3 次 / 1 s 间隔 / 单次 ≤4 s / 总预算 ≤12 s / 100 ms 取消粒度），`:147` 明确「不得写成在 12 s 内返回或捕获到 ETIMEDOUT」；`:149`「两个失败面共用同一预算」下展开为三个子面：首次 open 超时→工厂 `FileOpenFailed`、重连原语 `open()` 失败计入预算、播放中途读超时→EOF→供给停滞→同一入口。
- §6.2（`:184`）把 `m3u8_hold_counters=1000` / `max_reload=1000` 登记为「**外部供给停滞上界**，不写成流的自然终点」。
- §6.4（`:197-203`）：`rw_timeout=3000000`（3 s）是播放主路径唯一墙钟，一处设置同时覆盖 playlist 与 segment；3 s 到点表现为 EOF / 结束序列，不是可上报的超时错误。
- §10.2（`:294-301`）：判据载体 = 不注册 ctest 的独立可执行或手工冒烟；**事件计数为主、墙钟只作防挂死守卫**；可红可绿 = 去掉注入 / 去掉预算计数 / 去掉 `nullptr` 后查诊断状态 / 去掉 ICY 请求头必红，还原绿。

> 口径偏差提示（只读登记，供地图裁定，本票不改规格）：§10.1② 写「覆盖 §5.3 的两个失败面」，但触发集两类其实定义在 §5.2；§5.3:149「两个失败面」下却列了三个子面。属规格内锚点措辞偏差，与 #164「第二轮勘察 3 处口径偏差」相关，本票只登记不动字面。

---

## 1. 必答①：现有端点能否组合出「窗口冻结」语义

### 1.1 现有端点逐条语义（一手：`sdk/test/mocks/HttpMocks.hpp`）

| 端点 | 定义 | 命中锚点 | 响应 | 计数 | 释放 serve 线程 |
|---|---|---|---|---|---|
| `setPlaylistFailWindow(fromSeq,count)` | `:379-383`、判定 `:505-516` | **playlist 取回序号** `m_liveSeq`（`:488`） | `404 + Content-Length:0 + Connection:close` | `m_failCount`（`:509`） | 立即 return，同步释放 |
| `setSegmentFailWindow(fromSeq,count)` | `:391-395`、判定 `:548-555` | **segment 请求序号** `m_segmentRequests`（`:542`） | `404 + Content-Length:0` | `m_segmentFailCount` | 立即 return，同步释放 |
| `setSegmentHangWindow(fromSeq,count)` | `:396-400`、判定 `:558-568` | **segment 请求序号** | `200 + Content-Length` 后不发 body | `m_segmentHangCount` | 走 `holdOpenWithoutBody` |
| `setPlaylistHangWindow(fromSeq,count)` | `:405-409`、判定 `:491-502` | **playlist 取回序号** | `200 + Content-Length:4096` 后不发 body | `m_playlistHangCount` | 走 `holdOpenWithoutBody` |
| `holdOpenWithoutBody(s)` | `:801-823` | — | 发头不发 body、保持连接 | — | `select` 可读探测：对端 FIN（`n==0`）或 5 s 上限释放串行 serve 线程；`n<0`（2 s `SO_RCVTIMEO`）不当断开 |

窗口化 LIVE 正常分支（`:517-529`）每次 playlist 取回都 `m_liveSeq.fetch_add(1)`（`:488`），生成 `#EXT-X-MEDIA-SEQUENCE:<seq>` + `seg<seq>.ts` + `seg<seq+1>.ts`，即**窗口持续递进**；segment 分支（`:531-580`）按 `seg<N>.ts` 回独立完整 wav。

### 1.2 结论：**不能组合出「窗口冻结」语义**

三条硬理由：

1. **语义维度不对**：五个端点全是「失败」（404）或「挂起」（hang），没有任何端点表达「**取回成功但内容不变**」。窗口冻结的本质是 playlist 一直返回同一个 `MEDIA-SEQUENCE`、不再追加新分片、HTTP 层全程 200 成功——恰是「无错误」形态，与现有注入面正交。
2. **`m_liveSeq` 无条件推进**：`:488` 的 `fetch_add(1)` 没有任何冻结开关；所有注入窗口都是有限 `[fromSeq, fromSeq+count)` 且到点恢复（`:379-409` 的 `store` 语义），不存在「持续停在锚点」的状态。把 `setPlaylistFailWindow(S, 1000000)`（P3-3 用例 `test_live_source.cpp:177`）设成超长窗口也只得到「持续 404」，不是「持续同一份 200 清单」。
3. **组合即退化**：把 fail/hang 端点叠在冻结意图上（如冻结 N 次后 404、或 playlist hang→rw_timeout EOF），得到的是 §5.2 已覆盖的「playlist 重载失败 / 读超时 EOF」面，语义上退化为 P3-3 已建判据的形态，不再是「停发分片」。#171 body 与 #162 实证均已点明判据「必须让 playlist 侧触发重连或让首次 open 直接失败」——这恰恰说明纯冻结形态**无法**直接进既有重连入口。

### 1.3 最小新增端点设计（供 #171 采纳/否决）

> 设计目标：以最小改动表达「playlist 成功 200、窗口内容冻结、不推进序号」，且不让 mock 自锁。

**端点签名**（与既有 `setPlaylistFailWindow(fromSeq,count)` 形状对齐，测试可用同一套 S 取数）：

```
void setPlaylistFreezeWindow(int fromSeq, int count);   // 命中 [fromSeq, fromSeq+count) 的 playlist 取回
long long freezeServedCount() const;                    // 冻结期间已服务的 playlist 取回次数（事件计数观测点）
```

**状态字段**（追加到 `:840-853` 原子字段区，风格与 `m_failPlaylistFromSeq/m_failPlaylistFetches` 一致）：

```
std::atomic<int>      m_freezePlaylistFromSeq{-1};   // 冻结起点，-1 = 关闭
std::atomic<int>      m_freezePlaylistFetches{0};    // 冻结持续取回数，0 = 关闭
std::atomic<int>      m_freezeAnchorSeq{0};         // 冻结锚点：首次命中时锁存当前 seq，此后恒定
std::atomic<long long> m_freezeServedCount{0};      // 冻结期间已回的 playlist 取回次数
```

**请求序号锚点**：按 **playlist 取回序号** `seq = m_liveSeq.fetch_add(1)`（`:488`），**不是**全局请求序号 `index`（`:443`）、**不是** segment 序号（`:542`）。理由逐字沿用 `HttpMocks.hpp:374-378`：扫描期 `find_stream_info` 的 reload 次数随素材分析时长浮动，无法用请求总数锚定「播放期首个 reload」；测试在扫描完成后用 `requestCountFor(livePath)` 取 S 设窗，首个播放期 reload 的 `seq` 恰好 = S。冻结**必须落在播放期首个 reload**：若落在扫描期，`avformat_find_stream_info` 会卡在无新分片的清单上 → 首次 open 直接失败，退化成 P3-3 判据③（`test_live_source.cpp:438-455`），不是冻结形态。

**`handle` 分支位置与优先级**（插入 `:488-529` 区间，排在 hang（`:491`）与 404（`:505`）判定**之后**、正常窗口文本生成之前）：`hang > 404 > freeze > 正常推进`。命中 freeze 时：

- `m_freezeServedCount.fetch_add(1)`；首次命中把 `seq` 锁存进 `m_freezeAnchorSeq`，此后所有命中取回都忽略 `seq` 的实际值；
- **不推进窗口内容**：返回固定文本 `#EXT-X-MEDIA-SEQUENCE:<anchor>` + `seg<anchor>.ts` + `seg<anchor+1>.ts`（与冻结瞬间逐字一致），`Content-Length` 取该文本实际长度，`Connection: close`；
- 冻结窗口引用的两个 segment 仍按 `:531-580` 正常分支供给（「停发新分片、已发布分片仍在」的真实停播语义）；
- 命中判定与既有窗口同形：运行时可改（原子 `store` / `relaxed load`），不干扰 `m_failCount` 既有精确断言。

**释放串行 `serve` 线程的时机（关键设计点）**：

- `serve()` 是**单线程串行** `accept → handle → close`（`:418-431`）。冻结端点**必须同步 return**：`sendAll` 发完完整 200 响应（`Connection: close`）后立即返回，让 `serve` 立刻回到 `accept` 接下一次 demuxer reload / segment 请求。
- **不得**复用 `holdOpenWithoutBody`（`:801-823`）的「发头不发 body + `select` 5 s 上限 / 对端 FIN 释放」路径：那会占住唯一的串行 serve 线程，后续 playlist reload 与 segment 请求全部排队，mock 自锁、析构 `join()`（`:341-356`）卡死。冻结是「成功但内容不变」，**不持有连接、不进任何等待循环、不发 hang**。
- demuxer 的持续重载由客户端按自身 reload 节奏另起新连接发起，每次都是独立一次 `handle`，**不占用 mock 线程**。
- 线程释放兜底仍只依赖既有析构（`:341-356`：`m_stop=true` + 自连接唤醒 `accept` + `join`）；冻结端点**不新增** 5 s 上限、`holdOpenWithoutBody` 调用或任何 `sleep`。与 P3-3 hang 端点刻意分工：hang 靠 `rw_timeout=3s` 让客户端关闭、`select` 探测 `n==0` 释放；冻结不制造 hang，故无此问题。

---

## 2. 必答②：冻结窗口后 FFmpeg 4.4 hls demuxer 行为与重连状态机

### 2.1 依据链（一手源码）

- `sdk/extractor/hls/HlsExtractor.cpp:201`：`packetAt()` 逐包入口就是 `ret = av_read_frame(m_fmt, m_pkt)`；`:202-211` 仅在 `ret<0` 时把负返回记入 `m_lastReadError` / `m_diagValid` 并 `return nullptr`；任一成功读包即清零（`:227-230`）。
- `sdk/extractor/hls/HlsExtractor.cpp:234-243`：`consumeLastReadError` 读一次即失效；无 `m_diagValid` 时返回 `false`。
- `sdk/audio/decode/AudioStreamDecoder.cpp:719-751`（PCM/wav 分支）与 `:1074-1119`（ffmpeg 逐包分支）：重连**唯一入口** = `cursor->nextPacket(packet)==false` 后 `consumeLastReadError(&readErr)`，且 `hasDiag && isLivePassthrough() && readErr != AVERROR_EXIT && readErr != AVERROR_INVALIDDATA` 才进 `reconnectForLiveCore()` / `reconnectForLive()`。
- `AudioStreamDecoder.cpp:1141-1201`：重连状态机 = 3 次尝试、每次前 1 s 分片等待、`m_reconnectAttempts` 只数「真正走到重开一步」；耗尽 → `setError(FileOpenFailed, "live source reconnect budget exhausted")`；成功静默续播。
- `HlsExtractor.cpp:100-113`：`interrupt_callback`（`m_abortDecode`）+ `protocol_whitelist=http,tcp` + `rw_timeout=3000000`；**`m3u8_hold_counters` / `max_reload` 未在 open opts 显式设置**（沿用 FFmpeg 默认值）。
- `HttpMocks.hpp:209-213`（hls.c 实证注释）：「hls.c 对整条 LIVE 流只建一个 demuxer 会话，首段探测格式，`pls->pb` 由 `read_data` 跨段续喂」；`HttpMocks.hpp:535`：窗口序号与消费速度失步时「FFmpeg 陷入**无包重载死循环**」；`HttpMocks.hpp:201-204`：段音频必须盖过 `find_stream_info` 的 4 s 分析窗，否则「扫描会无限拉段挂死」。
- P3-3 既有实证（#162 body / #171 body / `test_live_source.cpp:331-337`）：「hls demuxer 跳过失败 segment 并持续推进 LIVE 窗口，`nullptr` 不上提 ⇒ 单段失败不触发解码层重连、全窗口 404 会让重开后无限跳段」。

### 2.2 判断

| 问题 | 判断 | 依据 |
|---|---|---|
| 是否持续重载 playlist | **是** | 无 `ENDLIST` 的 LIVE 在读完当前窗口分片后按 reload 节奏反复取回 playlist；冻结时每次取回都是同一清单（`MEDIA-SEQUENCE` 不变、无新 seg URI），hls 判「无新分片」后继续等待/重载。`HttpMocks.hpp:535` 已把此类失步登记为「无包重载死循环」；§6.2:184 把 `m3u8_hold_counters=1000` / `max_reload=1000` 登记为「外部供给停滞上界」 |
| 是否出现无包空转 | **是** | `av_read_frame` 阻塞在 hls `read_data` 内部等待新分片，既不返回包、也不返回负错误 ⇒ `packetAt` 不 `return nullptr`、不设 `m_lastReadError`（`HlsExtractor.cpp:201-211`）⇒ 解码线程停在 `cursor->nextPacket()`。与 P3-3「`nullptr` 不上提」同源，冻结比单段 404 更彻底（连可推进的段都没有） |
| 解码线程重连状态机是否触发 | **不会**（在 `m3u8_hold_counters` 耗尽 / 某次 rw_timeout 到点之前） | 重连唯一入口要求 `nextPacket()==false` + 诊断旁路有非 EXIT/非 INVALIDDATA 负返回（§5.2、`AudioStreamDecoder.cpp:719-751`/`:1074-1102`）。冻结回的是**成功 200、内容不变**，不产生 404 / 不产生 hang→EOF / 不产生 open 失败 ⇒ 不落 §5.2 触发集任一类 ⇒ `consumeLastReadError` 无值 ⇒ `liveReconnectAttempts()` 恒 0、`failCount`/`segment*Count` 恒 0、无 `FileOpenFailed`、无 EOS |
| `rw_timeout=3s` 是否会兜底 | **不会** | 冻结端点立即发完整 200 + `Connection: close`，不制造「发头不发 body」；`rw_timeout` 只在单次读阻塞 3 s 才到点（§6.4、#162 判据②）。冻结端点刻意不 hang，故不触发 EOF 上提 |
| `interrupt_callback` 能否打断冻结空转 | **未确认** | `m_abortDecode` 为 false 时回调不置中断；hls `read_data` 内部等待是否逐次检查回调、D10「100 ms 取消粒度」在冻结下是否仍成立，需 #171 实证 |

### 2.3 关键推论（衔接判据）

纯窗口冻结 = 「**无错误上提的静默停滞**」，产品链路按 ADR 0006/0007 + §6.2 设计**不**对这类停滞做主动处置（规格把它归为「外部供给停滞上界」而非自然终点）。票面判据要求「让 playlist 侧触发重连或让首次 open 直接失败」，但那两条路径恰是 404 / hang / open 超时，**已被 P3-3 全覆盖**；一旦采用，形态就不再是「停发分片」。这是本票的核心矛盾。

**未确认项**（不读 `sdk/3rdparty` 下本地 `hls.c`，依据已知行为 + 仓库内实证注释 + 票面实证推断，均留 #171 实证）：
1. FFmpeg 4.4 `hls.c` 冻结下的精确 reload 间隔（`EXT-X-TARGETDURATION:1` / `EXTINF:0.5` 下的取回频率）、`m3u8_hold_counters` / `max_reload` 默认值、是否及何时把 `AVERROR(EAGAIN)` / EOF 上提给 `av_read_frame`。
2. `EXT-X-MEDIA-SEQUENCE` 不变时，hls 是否重复拉同一 `seg<N>.ts`（`HttpMocks.hpp:209-213` 单 demuxer 会话跨段续喂）→ 表现为「无包」还是「重复消费同一段 PCM」。
3. `interrupt_callback` 在 hls 内部等待期间是否检查（关系 `stop()` 能否打断冻结空转）。

---

## 3. 必答③：判据形态与可红变异点

### 3.1 沿用形态（一手：`test_live_source.cpp` / §10.2）

- **事件计数为主**：`LiveWatch`（`test_live_source.cpp:88-124`）只记「发生过什么」（`sawError` / `sawSeekingOrEos` / `errorThenDecoding` / `durationNonZero` / `posRegressed` / `maxPosMs`）；取数点 `decoder.liveReconnectAttempts()`（`:196`、`:279`）、`server.failCount()`（`:198-200`）、`server.requestCountFor("/live")`（`:197`、`:278`）、`segmentFailCount()` / `segmentHangCount()` / `playlistHangCount()`（`:372`、`:422`、`:453`）。守恒式判据范例 `:200`：`fails == liveFetches - scanLiveFetches`。
- **墙钟只作防挂死守卫**：`pollUntil(pred, guardMs)`（`:126-139`）到点返回 false = 挂死证据，**不是行为断言**；现有守卫 20000 / 30000 ms（`:186`、`:260`、`:370`、`:420`），`loadPlayList` 15 s 只防死锁（`:52`、`:71`）。
- **测试内部守卫必须宽于被测链路超时预算**：被测链路预算 ≤12 s（§5.3 3×4 s）+ 单次 `rw_timeout=3s`（§6.4）；现有 20–30 s 守卫已宽于之。冻结形态若 demuxer 进入 `m3u8_hold_counters=1000` 的重载等待，耗时可能远超 30 s ⇒ **守卫不得为迁就它而放宽**（§5.3:145「12 s/4 s 是策略参数不是门禁阈值」、#164 out of scope「零 TIMEOUT 放宽」）。
- **不写**「12 s 内返回」「捕获 ETIMEDOUT」类断言（§5.3:147、§10.2:300、#162 判据②③ 注释 `:383-387` / `:432-437`）。

### 3.2 可红变异点（沿用 #162 三类，逐条给还原复绿）

| 变异点 | 做法 | 预期必红 | 还原复绿 |
|---|---|---|---|
| 去掉注入 | 不调 `setPlaylistFreezeWindow`（或退化路径里不调 `setPlaylistFailWindow` / `setPlaylistHangWindow`） | 冻结侧 `freezeServedCount()` / `requestCountFor(livePath)` 的「重复取回固定窗口」特征消失；退化路径的事件计数守恒式不成立 | 恢复注入调用 |
| 改动重连计数 | 把 `m_reconnectAttempts.fetch_add`（`AudioStreamDecoder.cpp:1171`）去掉 / 改断言期望值 | `liveReconnectAttempts()` 精确断言（如 `:196` 的 `==3`）必红 | 恢复计数累加与断言值 |
| 去掉诊断状态查询 | 去掉 `consumeLastReadError`（`HlsExtractor.cpp:234` / `AudioStreamDecoder.cpp:727`、`:1084`） | 供给停滞/失败不再进重连通道 ⇒ 终局 `FileOpenFailed` / 续播判据必红 | 恢复诊断旁路查询 |

**重要限制（如实标注）**：上述三个变异点**只在「判据依赖 playlist 侧触发重连或首次 open 失败」时成立**（与 #171 body 决策范围 3 一致）。若判据真的只针对「纯冻结形态」，则产品链路对纯冻结**无可观测终局事件**（`liveReconnectAttempts`=0、无 404、无 hang、无 EOS、无 `FileOpenFailed`，position 停滞与正常稳态 8 s 不可区分），**不存在可红变异点**——这本身就是「不可稳定注入」的直接证据。冻结侧唯一可计数的 `m_freezeServedCount`（playlist 重载次数）只能证明「注入生效 + demuxer 在重载」，不能证明产品链路对停发分片的处置；且重载次数受 hls reload 节奏与扫描期抖动影响，无法写成 `:200` 那种守恒等式。

### 3.3 还原复绿纪律

变异改动只写临时改动、取证后零残留（#162 / #171 边界）；不新增 ctest 目标、不放宽 `sdk_unit_tests` 的 `TIMEOUT`、不新增数值门禁。

---

## 4. 必答④：不可稳定注入时的处置建议与 §10.1② 覆盖口径读法

### 4.1 处置建议：**按地图 Fog 条款如实登记为遗留、不硬凑判据、不放宽守卫**

理由（逐条回指）：

1. **形态与触发集正交**：「停发分片」是「成功 200、内容不变的静默停滞」，不落 §5.2 单一入口触发集（供给停滞 EOF / 数据源层失败）的任一类；§6.2:184 已把这类「清单不推进」归为 `m3u8_hold_counters=1000` 的**外部供给停滞上界**，规格明确「不写成流的自然终点」，产品链路按设计不主动处置它。
2. **硬凑即退化且污染既有判据**：要产出可判定判据只能把冻结改造成 404 / hang / open 超时，三者均被 P3-3 建判据（#162、`test_live_source.cpp:338-455`），语义上不再是「停发分片」；且会破坏 P3-3 的事件计数守恒（`fails == liveFetches - S`，`:200`）。
3. **不放宽守卫**：`m3u8_hold_counters` / `max_reload` 在 `HlsExtractor::open()`（`:103-114`）未显式设置、默认值与 hls.c 重载节奏**未确认**；在实证前把守卫放宽到覆盖 1000 次 reload，违反 §5.3:145 与 #164「零 TIMEOUT 放宽」。
4. 结论措辞建议（供主会话写 resolution）：**「不可稳定注入为『事件计数为主』的可红可绿判据」**，不是「不可注入端点」——最小冻结端点在技术上可实现（见 §1.3），但实现后产品链路无可观测终局，故不产判据；#171 依其决策范围第 1 条「若调研判为不可稳定注入，按 Fog 条款如实登记为遗留、不硬凑判据、不放宽守卫，本票以裁定收口而不产代码」。

### 4.2 对 §10.1② 覆盖口径读法的影响

- §10.1②「三类端点形态…覆盖 §5.3 的两个失败面」中，**三个失败面是注入手段、两个失败面是处置目标，不是一一对应**：两个失败面 = §5.2 触发集两类（供给停滞 `AVERROR_EOF` / 数据源层失败 EIO·HTTP错·playlist重载失败），在 §5.3:149「共用同一预算」下展开为首次 open 超时 / 重连 open 失败 / 播放中途读超时三个子面。
- P3-3 已用「分片 404 + playlist 404 触发重连」「发头不发 body → 3 s EOF」「首次 open 超时 → 工厂 `FileOpenFailed`」把两个失败面**都**建了判据。「停发分片」登记为遗留 = **端点形态清单**少一项实现，**不构成第三个失败面、不减少已覆盖的失败面数**。
- 因此：**§10.1②「覆盖两个失败面」的口径不变**；只需在 #162 更正 comment / 本票 resolution / #164 地图 Decisions 里把「三类端点形态中第三类无独立判据、按 Fog 登记遗留」作为**实现完整度时点注记**如实登记——不改 §10.1 字面、不改判据数值、不改 `TIMEOUT`、不改 §11 界外裁决、不改 `GLOSSARY.md`（#164 时点快照纪律）。
- 附带登记 §0 的锚点措辞偏差（§10.1② 指向 §5.3、但触发集两类在 §5.2、§5.3:149 又列三个子面），供地图裁定，本票只读不改规格。

---

## 5. 留给 #171 的实证清单（本票不做代码实测）

1. **冻结端点实跑取证**（若 #171 决定先实现最小端点再判）：冻结后 `requestCountFor(livePath)` 是否持续增长（持续重载证据）、`segmentRequestCount()` 是否停止增长（无新分片）、`av_read_frame` 是否阻塞不返回（无包空转证据）、`liveReconnectAttempts()` 是否恒 0、`failCount` / `segmentFailCount` / `segmentHangCount` / `playlistHangCount` 是否恒 0、有无 `FileOpenFailed` / EOS、`state()` 是否恒 `DECODING`。
2. **hls.c 上提边界实测**：冻结下是否最终上提 EOF 并进重连，以及耗时；记录 `m3u8_hold_counters` 触发次数 / 墙钟；若 > 30 s 守卫 → 佐证「不可稳定注入、不放宽守卫」。
3. **取消贯通实测**：`interrupt_callback` 能否在冻结空转中打断 `stop()`（D10「100 ms 取消粒度」是否仍成立），`decoder.stop()` 的 `join()` 能否在守卫内返回。
4. **退化路径等价性实测**：「冻结 N 次后转 404 / hang」的事件计数是否与 P3-3 既有用例等价；若等价 → 证明无独立形态价值，支持 Fog 登记。
5. **纯冻结可红性实测**：对纯冻结形态尝试构造三个变异点（去掉注入 / 改重连计数 / 去掉诊断查询），确认是否**均无**可红差异（若确无 → 「不可稳定注入」定论）。
6. **若判不可稳定注入**：#171 按 Fog 条款以裁定收口不产代码；做时点快照更正回扫（#162 更正 comment、#164 地图 Decisions、§10.1② 相关纯新增时点注记，CRLF 保持、`git diff --numstat` 为 `N 0`，判定结论未变不堆叠注记）；票正文与旧 resolution 不改，只挂更正 comment。
7. **若判可稳定注入**：按 §1.3 端点 + §3 判据形态落地，变异验证只写临时改动、取证后零残留；`sdk/**` 改动触发两个 workflow，须与同批注释补漏合并提交；双端 attempt 1 `success`、Linux 覆盖率 ≥33%。

---

## 6. 未确认项汇总

- FFmpeg 4.4 `hls.c` 冻结下的精确 reload 间隔、`m3u8_hold_counters` / `max_reload` 默认值、是否/何时把 `AVERROR(EAGAIN)` 或 EOF 上提给 `av_read_frame`（未读本地 `hls.c`，`sdk/3rdparty` 按约束排除）。
- 冻结时 hls 是否重复拉同一 `seg<N>.ts`（单 demuxer 会话跨段续喂）→ 无包 vs 重复消费同一段 PCM。
- `interrupt_callback` 在 hls 内部 `av_usleep` 等待期间是否检查（关系 `stop()` 打断冻结空转）。
- 以上均**未确认**，留 #171 本地实证；本票所有 hls.c 内部行为判断均标注为「依据已知行为 + 仓库内实证注释 + 票面既有实证推断」。
