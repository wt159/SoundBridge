# 网络音频源接入规格

> 来源：wayfinder 地图 [地图：网络音频源（URL）接入规格与全谱系架构决策](https://github.com/wt159/SoundBridge/issues/118)，由 12 张子票的决策折叠而成。2026-11-11 收口。
>
> **本文件是规格，不是实现。** 代码、用例与 CI 改动走图外执行票；本图未改 `sdk/` / `app/` / CI / 用例 / 任何既有判据数值。
> 术语以 [`GLOSSARY.md`](../GLOSSARY.md) 为准；难逆转的架构决策见 [`docs/adr/0005`](../docs/adr/0005-network-source-two-paths.md)。
> 事实基线：[`docs/research/network-datasource-facts.md`](../docs/research/network-datasource-facts.md)（`research/network-datasource-facts` 分支 `c5c19fefa`）、[`docs/research/network-protocol-facts.md`](../docs/research/network-protocol-facts.md)（`research/network-protocol-facts` 分支 `5538ff280`）、[`docs/research/network-audio-sources-survey.md`](../docs/research/network-audio-sources-survey.md)（2026-11-09 时点快照）。**行号会漂移，冲突一律以源码与现行规格为准。**

## 1. 目的地与范围

把**静态 HTTP 点播、HLS VOD、HLS LIVE、ICY 直播、DASH 等全谱系形态**，以及「自定义 AVIO 扩展」与「FFmpeg 原生 URL 层」两条接入路径，逐一裁成可判定的数据源契约、能力边界、错误语义、测试门禁与分阶段落地顺序。走到这里，后续实现 effort 不需要再为「URL 数据源接什么、怎么接、哪些本版不做、为什么」做任何决定。

- **「全谱系都进规格」的验收含义**：§4 的每种形态都必须有一条显式裁决（纳入 / 本版不做 + 理由 + 重开触发条件）。把没做的形态以「界外」一笔带过或留成 fog，视为未通过。
- **范围只含规格与决策**：不改生产代码 / 用例 / CI，不动既有判据数值、容差、基线与 `TIMEOUT`，实现另开图外执行票（§11）。
- **不可推翻前提**逐条核对见 §12；本图唯一触碰的一条（能力位）已在 §5.4 / §8.2 显式登记并化解。
- **时点快照纪律**：URL 可达性、服务条款、源码行号随时会变。本规格不写死任何公网 URL 与授权结论，只写准入检查与复核责任（§10）。
- **界外**（地图 Out of scope）：生产代码实现 / 用例实现 / CI 改动、改既有判据数值与基线、播放器状态机与 app UI 实现、公开 API 签名演进、SomaFM 等第三方直播进 CI 门禁或产品默认列表、既有 CI flake / SIGSEGV 根因 / 基线假红、公网 URL 长期可用性与 ToS 的日常复核。

## 2. 术语

术语真源是 `GLOSSARY.md`，本图新增四条（见「网络音频源」分组）：**网络音频源**、**静态点播源**、**清单类源**、**无限流源**。

- 「边解边播」词条**不改**：它是一种数据供给契约，不表达音频来源是否来自网络；其 `_Avoid_` 已含「网络流」「流式播放」「流式解码」「网络流」。因此本规格与后续票一律停用「网络流 / 流式播放 / 在线流」指代网络来源，改用「网络音频源」。
- 「播放列表」继续专指 `MusicPlayList` 的曲目列表；HLS playlist / DASH MPD 一律叫「清单」或「清单类源」。
- 「已知长度」「可 seek」「能力位」「超时」是通用编程概念或实现细节，**不进 `GLOSSARY.md`**，只在本规格 §5 定义。
- 判定用的协议术语：`Range/206` 与 `Content-Range` 依据 **RFC 9110**（RFC 7233 已废弃）；HLS 的 VOD / LIVE 界定依据 **RFC 8216** 的 `EXT-X-ENDLIST` / `PLAYLIST-TYPE` / 滑动窗口措辞。RFC 8216 **没有**「LIVE 不可 seek」的逐字条款，「有界可 seek」是工程推论，不得写成 RFC 授权。

## 3. 现状事实基线（2026-11-11 只读核对）

以下均为源码 / 构建产物实证，不是方案；用来给每条裁决定位「改哪里」。

| 层 | 事实 |
|---|---|
| 数据源接口 | `DataSource` **无独立 `seek()`**；`readAt(off64_t, void*, size_t)` 按绝对字节偏移随机读，返回 0 = 越过末尾、`-1` = 失败；`getSize()` 基类默认 `UNKNOWN_ERROR`；`flags()` 预留 `kIsHTTPBasedSource` 等枚举位。 |
| 唯一生产子类 | `FileSource` 构造时缓存 `m_length`，`getSize()` 是 **O(1)**（不是全量扫描）；`reconnectAtOffset` 沿用基类 `ERROR_UNSUPPORTED`。 |
| 生产链第一层 | `MusicPlayList::_addMusic()` 把整串 `new FileSource(musicPath.c_str())`，URL 字符串被当本地路径 `_wfopen` / `ifstream::open` 打开失败 → `initCheck() != OK` → **`FileOpenFailed` 拒绝**，根本到不了 `ExtractorFactory`。 |
| 工厂 | 注册扩展名 **11 个**（10 个带 sniff + `.wma`），四级选择路径（扩展名 → 扩展名失败后 magic 回退 → 未注册时 magic sniff → 遍历全部 sniffer 兜底）；没有 `.m3u8` / `.mpd` / ICY / `#EXTM3U` / `<?xml` 识别。 |
| 自定义 AVIO | 4 个 `PacketReader`（ape / mkv / asf / m4a）用 `avio_alloc_context` + `AVFMT_FLAG_CUSTOM_IO` + `avformat_open_input(&m_fmt, nullptr, nullptr, nullptr)`；`open()` 要求 `size > 0`；`AVSEEK_SIZE` 返回构造时传入的 `fileSize`；库侧因非空 seek 回调把 `AVIOContext.seekable` 置为 `AVIO_SEEKABLE_NORMAL`；`avioRead` 把 `readAt` 的 `n <= 0` 一律译作 `AVERROR_EOF`。 |
| 取消 | `m_abortDecode` 只经 `setAbortFlag` 贯通 `FLACDecode` / `VorbisDecode` 读回调，**未贯通 4 个 `avioRead` 阻塞点**；全自有 `sdk/` 源码 `interrupt_callback` **零命中**。 |
| 构建 | `config.h`（Windows MinGW 一次真实 configure 产物）`CONFIG_DASH_DEMUXER 0`（`libxml2` 未启用）、`protocol_whitelist` 项目未设置（默认 NULL）；`CONFIG_NETWORK / HTTP / HTTPS / TLS / HLS / RTMP / TCP / UDP = 1` 是 configure 默认产物，非项目显式开关；`CONFIG_FFPROBE 0`。**Linux 侧无 configure 产物可实证**，「dash 同样未启用」是推断，属图外实现 effort 的准入检查。 |
| 时长与 seek | `durationMs == 0` 语义是「标称时长未知」；`_setPosition` 的 `if (pos > durationMs) return;` 使 `pos > 0` 的 seek 成为空操作，`pos == 0` 仍会走 `seekToMs(0)`；UI 由 `setRange(0, durationMs/1000)` 间接禁用拖动。 |

**对 11-09 调研快照的更正**（不得再拿快照当现状）：`getSize` 非全量扫描、格式 11 个非 12 个、URL 在第一层即被拒、`m_abortDecode` 贯通范围更窄、`hls.c` 空 base 的真实失败机制、Radio Paradise 伪 `Content-Range` 是 OpenResty 前端特定行为、MDN 素材 CC0。详见两份 facts 的「与调研快照不一致之处」章节。

## 4. 全谱系形态清单与裁决表

### 4.1 切分维度

按三维度叉乘后合并成互不重叠的形态：**负载形态**（单体文件 / 清单 / 无限字节流）× **长度与可 seek**（已知且可 seek / 有界窗口 / 未知不可 seek）× **授权档位**（可进 CI / 可进产品默认列表 / 仅手工冒烟）。

「302 重定向」「TLS」「伪 `Content-Range`」「无扩展名 URL」「带查询串」是**静态点播源的判定边界**（§4.3），不单列形态，避免清单重漏。「曲目列表」「本地文件」不是网络形态。

### 4.2 裁决表

| # | 形态 | 负载 | 长度 / 可 seek | 接入路径 | 本版裁决 | 门禁归属 | 授权档位 |
|---|---|---|---|---|---|---|---|
| S1 | 静态 HTTP 点播 | 单体文件 | 已知、可 seek | 自定义 AVIO 扩展 | **纳入（P0）** | 本地 mock 可红可绿 | 可进 CI（仅本地副本） |
| S2 | HLS VOD | 清单 | 有界窗口（`ENDLIST` / `PLAYLIST-TYPE:VOD`） | FFmpeg 原生 URL 层 | **本版不做（P2）** | 本版仅手工冒烟 | 仅手工冒烟 |
| S3 | HLS LIVE | 清单（滑动窗口） | 未知、不可 seek | FFmpeg 原生 URL 层 | **本版不做（P3）** | 不进门禁 | 仅一次性人工协议观测 / 禁止 |
| S4 | ICY 直播 | 无限字节流 | 未知、不可 seek | FFmpeg 原生 URL 层 | **本版不做（P3）** | 不进门禁 | 仅一次性人工协议观测 / 禁止 |
| S5 | DASH VOD | 清单（MPD） | 依 MPD | FFmpeg 原生 URL 层 | **本版不做（P4）** | 本版仅手工冒烟 | 仅手工冒烟 |
| S6 | DASH LIVE | 清单（MPD + 滑动窗口） | 未知、不可 seek | FFmpeg 原生 URL 层 | **本版不做（P4）** | 不进门禁 | 仅手工冒烟 |

**理由与重开触发条件**（每行必填，缺一即验收不通过）：

- **S1 静态 HTTP 点播**：与既有「已知长度 + 可随机 seek」契约天然兼容，`Range: bytes=...` 可直接映射 `readAt(off, …)`，风险最低。重开条件不适用（本版纳入）。
- **S2 HLS VOD**：自定义 `pb` 只承载初始 playlist 字节，之后每个 segment / variant 经 `s->io_open` 用 playlist 内相对 URL 重开，空 base 下相对路径无法解析协议（§6.2）。重开条件 = P1 能力落地 + 清单类本地 mock 判据就绪 + `protocol_whitelist` / `interrupt_callback` 契约定稿。
- **S3 HLS LIVE / S4 ICY 直播**：长度未知不可 seek、无自然终点、取消信号未贯通到阻塞读点，且公开电台直播服务有明确 ToS 红线。重开条件 = 独立连接 / 重连状态机 effort + 授权复核通过 + 无限流 EOS / UI 语义定稿。
- **S5 / S6 DASH**：当前构建 `CONFIG_DASH_DEMUXER 0`（`libxml2` 未启用）；DASH-IF 测试向量为 CC BY-NC-ND 4.0，明确「测试向量本身不得为商业目的分发」；纯音频测试流少。重开条件 = 显式启用 dash demuxer 并评估依赖与授权 + 清单类接入路径落地。

### 4.3 判定边界（静态点播源）

| 边界 | 判定规则 |
|---|---|
| `302` 重定向 / TLS | 按最终响应的 `Content-Length` + `Accept-Ranges` 判定；重定向链本身不改变形态归类。 |
| 伪 `Content-Range` 总长 | **不能只凭「有 206 / 有 `Content-Range`」判为静态点播**。必须结合 `Icy-*` 响应头、`Content-Length` 是否随数据增长、连接是否自然终止；命中则归 S4 无限流并拒绝。RFC 9110 规定未知 complete-length 应写 `*`。 |
| 无扩展名 / 带查询串 / `.m3u8` / `.mpd` | 现有 `parseFileName()` 只按最后一个 `.` 切、`normalizeExtension()` 不解析 URL；规格要求**显式判定**：清单类按 S2/S3/S5/S6 的「本版不做」裁决行明确拒绝，无扩展名且内容为本版格式时按 S1 处理，否则拒绝。不得静默落到 magic sniff 兜底。 |
| 服务端不支持 Range | 归数据源层失败（§7），不降级为「边下边播」；`Accept-Ranges: none` 等价于本版不接入。 |

### 4.4 裁决模板

每条形态裁决必填 8 字段：形态名、负载形态、长度 / 可 seek、接入路径、本版裁决、理由、重开触发条件、门禁归属 + 授权档位。**本版不做的形态也必须填满**，且理由若属合规而非技术，必须写合规依据、重开条件必须包含「授权复核通过」。

## 5. 数据源契约与能力模型

### 5.1 能力只放数据源层内部

复用现成虚接缝，不新造 API：

- `getSize()` 返回 `UNKNOWN_ERROR`（基类默认）= 长度未知；`AVSEEK_SIZE` 返回 `<0` = 不支持取大小；`AVIOContext.seekable = 0` = 流不可 seek（三者语义由协议事实票用 FFmpeg 4.4 官方 doxygen 逐字确认）；`flags()` 已有 `kIsHTTPBasedSource` / `kStreamedFromLocalHost` 预留位。
- **`read_packet` 对流式数据不得返回 0、须返回 AVERROR**（FFmpeg `avio.h` 官方措辞）；现有 `avioRead` 把 `readAt` 的 0 直接译作 `AVERROR_EOF`，只适用于已知长度的静态点播源，无限流源不适用。

### 5.2 静态点播总长

取首个 `206` 的 `Content-Range: bytes …/<total>` 或 HEAD 的 `Content-Length`（依据 RFC 9110）。**禁止全量扫描**：`FileSource::getSize` 本就是 O(1) 缓存，网络源更不应为了取长度先下载一遍。伪总长的识别见 §4.3。

### 5.3 取消与超时

- `m_abortDecode` 仍是**唯一取消信号**（ADR 0003）；规格不引入第二个取消面。
- 「贯通到 `avioRead` 阻塞点 + `AVFormatContext.interrupt_callback` + 连接 / 读超时」登记为 **P1 前置能力，属图外实现 effort**；本图不改生产代码。在这项落地之前，任何无限流形态都不具备可判定的取消语义，这是 S3/S4 本版不做的硬理由之一。
- 超时归**数据源层**（连接 / 读超时），不进播放任务状态；`seekToMs` 仍只保留单槽语义，不新增定位入口。

### 5.4 与播放器状态契约的冲突与化解

`doc/player-state-contract.md` §7 裁「UI 从 `duration()==0` 单一信号得知不可 seek，**不引入能力位 / `isSeekable()`**」，并列为本图 Notes 的不可推翻前提。本规格因此**不新增任何公开能力位**：§5.1 的能力表达全部收在数据源层内部，公开面继续沿用 `duration()==0`（§8.2）。这是本图唯一触碰不可推翻前提之处，已在能力模型票与公开 API 票的 resolution 中显式登记并化解，§12.2 再列一次。

## 6. 接入路径分派

### 6.1 两条路径（并存、互斥）

| 路径 | 适用形态 | 装配要点 | 本版 |
|---|---|---|---|
| **自定义 AVIO 扩展** | S1 静态点播源 | 新增数据源实现把 `Range: bytes=<off>-<off+size-1>` 映射为 `DataSource::readAt(off, buf, size)`，继续走 4 个 `PacketReader` 的 `AVFMT_FLAG_CUSTOM_IO` + `avformat_open_input(&fmt, nullptr, ...)` | 实现（P0） |
| **FFmpeg 原生 URL 层** | S2–S6 清单类 / 无限流 | `avformat_open_input(&fmt, url, ...)` + 显式 `protocol_whitelist` + `interrupt_callback`；不适用自定义 AVIO 的已知长度、可 seek、包为唯一层间单元等条款 | 只写规格，不实现 |

### 6.2 为什么清单类不能塞进自定义 AVIO（采用更正后的机制）

FFmpeg 4.4 `utils.c` 在有自定义 `pb` 时 `init_input()` 直接返回、不打开 URL，`s->url` 为空串；`hls_read_header()` 用 `parse_playlist(c, s->url, NULL, s->pb)`，自定义 pb 只承载**初始 playlist** 字节；之后每个 segment / variant 经 `hls.c → s->io_open → io_open_default(options.c:191) → ffio_open_whitelist` 用 playlist 内的 URL 重开独立 AVIOContext。空 base 下相对 URI 无 scheme，`ffurl_open_whitelist` 无法解析协议 ⇒ 打开失败。

**措辞更正**：`ff_make_absolute_url2` 对 NULL / 空 base + 相对 URI 是**原样返回（非空）**，`if (!pls->url[0])` 不因空 base 触发；11-09 快照所写「`nullptr` base ⇒ `ff_make_absolute_url` 返回空 ⇒ 直接失败」与 4.4 源码不吻合。工程结论不变，但引用机制必须用本节表述。

### 6.3 分派点与失败处理

- 分派点在**数据源构造层**（`MusicPlayList::_addMusic`），按「是否清单类」判定（扩展名 / Content-Type / `#EXTM3U` / `<?xml` 起始），**不按 URL scheme 分派**。
- **分派失败一律明确拒绝，不静默回退到另一条路径**：两条路径的长度、可 seek、取消与错误语义不同，静默回退会让判据失去意义。
- `protocol_whitelist` 当前默认 NULL；走原生 URL 层时须显式设置（如 `file,http,https,tcp,tls,crypto`）并配 `interrupt_callback`，这是规格要求，不是既有实现。

## 7. 错误语义

### 7.1 分层表

| 层 | 判定对象 | 出口 | 依据 |
|---|---|---|---|
| 数据源层 | URL 打开失败、DNS / TLS、重定向链、超时、断流、服务端不支持 Range、清单分片过期、伪长度 | 复用既有 `FileOpenFailed` 一族 | `doc/bad-input-spec.md` §12 已把「读盘中途 IO 失败、权限、路径编码」归数据源层；网络失败是同一层的延伸 |
| 内容层 | 文件内容坏（截断 / 坏帧 / 头坏 / 容器声明撒谎） | `createExtractor()==nullptr` 仍是**拒绝唯一出口** | `doc/bad-input-spec.md` §2.1 |

两层不互相顶替。细粒度网络原因（上表左列）**只进日志与本规格归类表**，不改变对外错误码集合，不新增「有码但不承诺上报」的条目，不改 `onError` 签名，不动错误码体系（`doc/player-state-contract.md` §6 / §8 界外）。

### 7.2 处置与回调

- 「能解出多少就播多少」只适用于**截断**；网络断流不满足「坏帧之后还有完好数据」，按既有 error→state 与 `m_autoSkipOnError` 语义处理，不新造跳轨规则。
- **错误不进状态面、回调单线程串行、曲目三连回调顺序**全部不变。
- 无限流源没有自然终点，不产生「曲目结束」回调分支（§8.3），因此不新增回调类型或结束事件。

## 8. 公开契约与 UI 行为约束

### 8.1 入口

URL 通过**既有播放列表字符串入口**进入，不新增 / 不修改 `soundbridge::Player` 公开签名；七个控制入口保持 `void`。URL 识别与分派发生在数据源构造层，对上不可见。

### 8.2 不可 seek 的单一信号

无限流源 / 不可 seek 源一律把**标称时长报 0**，UI 从 `duration()==0` 这一个信号得知不可 seek（由 `setRange(0,0)` 隐式禁用拖动）。**不引入能力位 / `isSeekable()`**。`durationMs==0 && pos>0` 的 `seek()` 保持空操作（不回退 `position()`、不发 `onPositionChanged`、不改状态、不发 `onError`），`pos==0` 归零例外保留；`position()` / `onPositionChanged` 仍按已消费 PCM 推进。规格只写行为约束与验收口径，**不写 app UI 实现**。

### 8.3 无限流没有自然终点

不主动产生 EOS、不发曲目结束回调、不触发自动跳轨；终止只来自用户 stop / 切歌 / 错误。曲目三连回调序列在无限流下只发生前两连，这一点写进 §13 验收口径。

### 8.4 混合列表

本地文件 + URL 混排时沿用既有切歌 / 跳轨 / autoSkip 语义，不新造 URL 专属跳轨规则；「曲目结束」的差异只体现在无限流不主动结束。

## 9. 测试门禁形态

### 9.1 唯一可进 CI 的形态

**本地 mock HTTP server + 预先落到本地的固定素材**。素材授权优先级：MDN learning-area（CC0 1.0）> samplelib（"no license restrictions"，无独立 ToS）> file-examples-download（"test your app"，无正式条款）。**任何公网 URL 不得写成 CI 门禁依赖**；公网项一律仅手工冒烟，SomaFM 等红线项连冒烟都不进。

### 9.2 可红可绿的判据形态（契约 × 判据形态 × 落点）

| 契约 | 判据形态 | 落点 |
|---|---|---|
| `Range/206` → `readAt` 映射 | `Content-Length` + `Accept-Ranges: bytes` + `Range: bytes=<off>-…` 的 206 与绝对偏移读一致 | 图外 P0 用例 |
| 重定向 / TLS 变体 | 按最终响应头判定，重定向链不改形态归类 | 图外 P0 用例 |
| 伪长度识别 | 结合 `Icy-*` 头 + 长度是否随数据增长 + 连接是否自然终止；命中即拒绝 | 图外 P0 用例 |
| 清单类 / 无扩展名 URL | 按 §4 裁决行**明确拒绝**，不是静默失败 | 图外 P0 用例 |
| 取消与超时贯通 | 登记为 P1 前置能力，尚未落地前不作为门禁 | 图外 P1 effort |
| 不可 seek 的公开信号 | 沿用既有 `durationMs==0` 的 seek 空操作判据，**数值一字不动** | `sdk_interaction_tests`（既有） |

### 9.3 稳定性与零改动

- mock server 用**进程私有端口**，日志 / 临时目录带 pid，不写固定名字的共享路径（`AGENTS.md` 的 `ctest -j4` 教训）。
- 不改 14 个 `sdk_*` 目标、`TIMEOUT`、快 / 全量集划分、既有判据数值、BadInput 55 例 / 396 断言、覆盖度 15 行；不新增耗时 gate。
- 本版不做的形态（S2–S6）只做手工冒烟，其门禁判据随各自重开 effort 一起定义。
- 本规格只写判据形态，**用例实现属图外**。

## 10. 合规与产品边界

四条规范性措辞（准入规则，不是背景描述）：

1. **公网第三方服务不得作为 CI 门禁依赖**；门禁只认本地 mock HTTP + 固定素材。
2. SomaFM 等 ToS 明确拒绝第三方客户端 / 自动化的源**不进 CI、不进产品默认列表**。
3. **产品不内置公开电台列表**，只提供「用户手动输入 URL」；Radio Browser / `yp.xml` 等目录只作人工查候选，不预置。
4. **三档归类**（可进 CI / 可进产品默认列表 / 仅手工冒烟）是 §4 每条形态裁决的**必填字段**，缺一行即验收不通过。

时点快照纪律：ToS、URL 可达性、源码行号都会漂移。规格不写死具体 URL 与授权结论，只写「准入检查 + 复核责任归图外实现 effort」，并指向 `docs/research/network-protocol-facts.md`（2026-11-11）与 `docs/research/network-audio-sources-survey.md`（2026-11-09）。**「无条款」不得反推「允许」**。因合规而非技术不做的形态，理由须写合规依据、重开条件须含「授权复核通过 + 条款页可得」。

不新增记录面、不新增观测仪表；`doc/observability-contract.md` 的「不作为失败门禁」与「不设任何数值阈值」永久裁决继续适用于网络量。

## 11. 分阶段落地与图外清单

| 阶段 | 内容 | 本版 | 前置 / 重开触发条件 |
|---|---|---|---|
| **P0** | 静态 HTTP 点播：`HttpSource` + 自定义 AVIO 扩展 + 本地 mock 门禁 | **做** | 与既有随机读契约兼容，无额外前置 |
| **P1** | 能力模型落地：`m_abortDecode` → `avioRead` + `interrupt_callback` + 连接 / 读超时 | 不做 | 独立实现 effort；是 P2/P3 的前置能力，不改公开签名 |
| **P2** | HLS VOD（原生 URL 层） | 不做 | P1 落地 + 清单类本地 mock 判据就绪 + `protocol_whitelist` / `interrupt_callback` 契约定稿 |
| **P3** | HLS LIVE / ICY 直播 | 不做 | 独立连接 / 重连状态机 effort + 授权明确 + 无限流 EOS / UI 语义定稿 |
| **P4** | DASH VOD / LIVE | 不做 | 显式启用 dash demuxer 并评估 libxml2 依赖与 DASH-IF 授权 |

**图外实现 effort**：按一票一决策切票，本图只产规格与决策、不带执行；GitHub 票正文是历史留痕不改，只在需要更正的旧票上加 comment。

**文档回填清单**：`doc/streaming-playback-spec.md` §1、`doc/bad-input-spec.md` §12、`doc/player-state-contract.md` §7 / §9、`doc/observability-contract.md` §1、`doc/format-support.md` 维护规则、`GLOSSARY.md`、`doc/开发方向建议.md`。原判据、表格与数值一字不动。

## 12. 不可推翻前提核对与文档接缝

### 12.1 逐条核对

| 前提（来源） | 本图处置 |
|---|---|
| 包为唯一层间单元、索引产物 O(1)、`positionMs` 归消费端（#42 / ADR 0003） | **一字不动**；S1 走自定义 AVIO 正是延续该链路 |
| `m_abortDecode` 唯一取消信号、seek 只留 `seekToMs`（#42 / ADR 0003） | **一字不动**；贯通到 AVIO 登记为 P1 图外能力，不引入第二取消面 |
| 三态、错误不进状态面、回调单线程串行、七个控制入口 `void`（#75 / ADR 0004） | **一字不动**；§7 / §8 沿用 |
| `durationMs==0` 的 seek 空操作、**不引入能力位 / `isSeekable()`**（#75 §7） | **一字不动**；唯一冲突见 §12.2 |
| 坏输入内容层拒绝唯一出口 `createExtractor()==nullptr`、55 例 / 396 断言（#22） | **一字不动**；网络失败另归数据源层（§7.1），两层不混 |
| 观测「不作为失败门禁」、不设任何数值阈值（#94 / #104） | **一字不动**；网络量不新增记录面 |
| 14 个 `sdk_*` 目标 / `TIMEOUT` / 快全量集 / 覆盖度 15 行 / `ctest -j` 每进程只写自己的文件 | **一字不动**；§9.3 复核 |
| 既有规格界外章节「网络流 / URL 属界外」 | **补指针 + 措辞更正**，原判据与表格不动（§12.3） |

### 12.2 唯一冲突与化解

「长度 / 可 seek 显式可选能力」与 #75 §7「不引入能力位」相冲突。化解：能力表达**只放数据源层内部**（`getSize` / `AVSEEK_SIZE` / `seekable` / `flags()`），公开面继续用 `duration()==0` 单一信号，不新增 `isSeekable()` 或任何公开能力位。登记票据：能力模型票与公开 API 票的 resolution comment。

### 12.3 文档接缝

| 文档 | 处置 |
|---|---|
| `doc/streaming-playback-spec.md` §1 | 界外声明后加指向本规格的指针；本地文件范围与判据不动 |
| `doc/bad-input-spec.md` §12 | 加「网络失败仍归数据源层、内容层拒绝唯一出口不变」的指针 |
| `doc/player-state-contract.md` §7 / §9 | 加「本图沿用 `duration()==0` 单一信号、能力只放数据源层」的指针 |
| `doc/observability-contract.md` §1 | 界外清单加「已由本图单独收口，不新增记录面 / 不设阈值继续适用」的指针 |
| `doc/format-support.md` 维护规则 | 加「URL 入口不改变受支持格式定义，清单类 URL 不注册提取器」 |
| `GLOSSARY.md` | 新增「网络音频源」分组四条术语（已就地写入） |
| `doc/开发方向建议.md` | 加注：网络音频源方向已由本规格收口，方向一 / 方向三的原建议保留作历史记录 |
| GitHub 票正文 | 历史留痕不改，只加更正 comment |

## 13. 验收判据

| 契约 | 判据 | 落点 |
|---|---|---|
| 全谱系裁决完整 | §4 六行每行 8 字段齐全，「本版不做」行含理由 + 重开触发条件 | 本规格 §4（文档判据） |
| 接入路径分派 | 静态点播走自定义 AVIO；清单类 / 无限流按裁决行明确拒绝，不静默回退 | 图外 P0 用例 |
| 单一不可 seek 信号 | 沿用既有 `durationMs==0` 的 seek 空操作判据，数值一字不动 | `sdk_interaction_tests`（既有） |
| 错误分层 | 数据源层失败不改对外码集合；内容层拒绝唯一出口不变 | 图外 P0 用例 |
| 本地 mock 门禁 | 可红可绿判据形态见 §9.2，公网 URL 零门禁依赖 | 图外 P0 用例 |
| 合规 | 三档归类为每行必填，规格不含写死的公网 URL 与授权结论 | 本规格 §10（文档判据） |
| 不可推翻前提 | §12.1 逐条「一字不动」、§12.2 唯一冲突已登记化解 | 本规格 §12（文档判据） |

**零改动声明**：本图未改 `sdk/` / `app/` / CI 配置 / 用例 / 任何既有判据数值、容差、基线、`TIMEOUT`。规格、ADR、`GLOSSARY.md` 与旧规格指针均为文档产物。

**fog 清空记录**：① 网络专属观测口径 → 裁「不新增记录面，沿用观测契约规格与不设数值阈值的永久裁决」（§10）；② 断线重连 / 连接生命周期状态机 → 裁「属 P3 独立 effort，不在本规格内，写进该形态的重开触发条件」（§11）；③ URL 与本地文件混排语义 → 裁「沿用既有切歌 / 跳轨 / autoSkip，不新造规则」（§8.4）。
