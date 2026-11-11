# 全谱系协议形态：一手规范、授权与合规边界复核

> 复核日期：**2026-11-11**（时点快照）
> 对应：GitHub issue #120；背景 `docs/research/network-audio-sources-survey.md`（2026-11-09 快照）§7 十四条未核实项中会改变规格裁决的部分。
> 性质：**纯取证、只读**。未修改任何源码 / 用例 / CI / 规格；未下载大文件（仅响应头 / 文本片段，`curl -I`、`-o /dev/null`、`--max-time` 短截断）；本文件是本次唯一新增文件。
> 方法：一手优先 —— 官方规范（RFC）、FFmpeg 4.4 官方源码（tag `n4.4.4` raw + 版本化 doxygen）、仓库本地 `config.h` / `build.sh` / `configure`、站点自有条款页；`WebFetch` 抓页面、`curl` 抓原文逐字 `grep`/`sed`。
> 纪律：URL 可达性与 ToS 随时会变；**任何公网 URL 都不得写成 CI 门禁依赖**；授权结论按「可否进 CI / 可否进产品默认列表 / 仅手工冒烟」三档归类；SomaFM 红线不推翻。
> 事实与推断分开：每条以 **【事实】** 标注一手逐字证据，**【推断】** 标注推论，**【未核实】** 标注不可知（附失败原因），不编造 URL 与授权条款。

---

## 1. 静态点播素材站的完整 ToS / 热链 / 自动化高频访问

### 1.1 SampleLib
- 【事实】`https://samplelib.com/` 逐字：*"A completely free library of test files available to download **without licensing restrictions**. The files are designed for developers, designers, testers, advertisers, and anyone else who needs demos and testing files."* 页面所列链接均为各格式素材页，**未出现任何 Terms / Privacy / License 页面链接**（WebFetch 2026-11-11）。
- 【事实】`https://download.samplelib.com/sample-mp3.html` 逐字：*"Below are sample music files available for download **with no license restrictions**"*；同样无独立 ToS 链接。
- 【未核实】"是否允许热链与自动化高频访问"：两页均未提及 hotlinking / automated / bot / rate limit，且无独立条款页可查 ⇒ 仍不可知（失败原因：站点未提供条款页）。
- 【推断】"no license restrictions / without licensing restrictions" 是面向开发测试的营销措辞，可支撑「下载后作为本地固定素材」；但**不能**据此认定「允许热链或高频自动化访问公网 URL」。

### 1.2 File Examples Download
- 【事实】`https://file-examples-download.com/?page_id=9` 逐字：*"Are you tired of searching for audio files with the right license to **test your app**? ... You can download free .mp3, .wav, .ac3, .aiff, .amr, .au, .flac, .mid, .mka, .ogg, .ra, .voc, .wma ..."*；页面未返回任何 Terms / License / Privacy 链接（WebFetch 2026-11-11）。
- 【未核实】完整 ToS、热链、自动化高频访问：同上，无条款页 ⇒ 未核实（失败原因：站点未提供条款页）。
- 【推断】仅有「test your app」的测试用途定位，不覆盖产品分发。

### 1.3 MDN learning-area 仓库素材
- 【事实】`https://github.com/mdn/learning-area/blob/main/LICENSE` 逐字开头 *"CC0 1.0 Universal ... Statement of Purpose ..."*，即仓库以 **CC0 1.0 Universal** 发布；`https://raw.githubusercontent.com/mdn/learning-area/main/LICENSE` 实测 `200`（2026-11-11）。
- 【事实】CC0 第 2 节 Waiver 逐字含 *"...for any purpose whatsoever, **including without limitation commercial**, advertising or promotional purposes"*。
- 【推断】仓库级许可覆盖 `html/multimedia-and-embedding/tasks/audio-video/audio.mp3`（survey §2.1.5 引用的直链）等随仓库分发的素材；这是三站点中**许可最明确、最稳**的一个（PD 级）。
- 【未核实】未逐文件核实该 mp3 是否含第三方来源 / 额外署名要求；MDN 文档页未为每个音频素材单独声明许可。
- 【事实】survey §2.1.5 记录的 jPlayer 素材已失效（`200 text/html` / 连接失败），授权未核实 —— 本次未再探测。

**小结（会改变裁决的部分）**：三站**都没有**「允许热链 / 自动化高频访问」的正面授权；samplelib 与 file-examples 连独立 ToS 都没有。⇒ 公网直链**不进 CI 门禁、不进产品默认列表**；素材只应下载成本地固定文件后用于本地 mock。

---

## 2. HLS VOD / DASH 测试流的正式条款：自动化测试与产品内置

逐站一手核查（2026-11-11 WebFetch）：

| 来源 | 一手 URL | 页面事实（逐字） | 自动化测试 | 产品内置 |
|---|---|---|---|---|
| Apple | `https://developer.apple.com/streaming/examples/` | *"**Note: This example is for testing only.**"* | 未核实（无条款） | 未核实，倾向否（"for testing only"） |
| Mux | `https://test-streams.mux.dev/` | *"There's not much to see here, this is just some **test HLS streams, primarily used by hls.js** and hosted with 💖 by Mux."* | 未核实（无条款） | 未核实（无条款） |
| Unified Streaming | `https://demo.unified-streaming.com/` | 仅 demo 导航链接，无任何条款 | 未核实（无条款） | 未核实（无条款） |
| hls.js 测试清单 | `https://github.com/video-dev/hls.js/blob/master/tests/test-streams.js` | `createTestStream(url, description, live, abr, skip_ua)`、`skipFunctionalTests`、`MuxLowLatencyHls` 标 `live: true`、`mp3Audio: 'MP3 VOD demo'` —— 明确是功能测试清单 | 是（其自身功能测试用途） | 否（第三方流 URL 不在清单仓库许可范围） |
| hls.js 仓库许可 | `https://github.com/video-dev/hls.js/blob/master/LICENSE` | *"Copyright (c) 2017 Dailymotion ... **Licensed under the Apache License, Version 2.0**"* | — | 【推断】Apache-2.0 只覆盖 hls.js **代码**，不覆盖清单里引用的 Mux / JW / Apple / Bitmovin / Shaka 等第三方流 URL 与媒体内容 |
| DASH-IF tools 页 | `https://dashif.org/tools/testassets` | *"Find the test vectors ... at https://testassets.dashif.org/."*（无条款） | — | — |
| **DASH-IF testassets（正式条款）** | `https://testassets.dashif.org/` | *"These test vectors are being made available under the **Creative Commons Attribution-NonCommercial-NoDerivatives 4.0 International** license. DASH-IF considers the use of these test vectors for the **development of commercial DASH clients and services to be a non-commercial use** ... **provided that the test vectors themselves are not distributed for commercial purposes**."* | 【推断】开发/测试（含商业客户端开发）视为非商业 ⇒ 可 | 【事实+推断】CC BY-NC-ND 4.0 含 NC + ND，且明确「测试向量本身不得为商业目的分发」 ⇒ **不可**产品内置 / 再分发 |
| Shaka Player 仓库 | `https://github.com/shaka-project/shaka-player/blob/main/LICENSE` | *"Apache License Version 2.0, January 2004 ..."* | — | 【推断】仅覆盖代码；demo 媒体资产未见条款 |
| Shaka demo 页 | `https://shaka-player-demo.appspot.com/` → `https://shaka-project.github.io/shaka-player-release/` | 仅为自动重定向提示页（*"You should be automatically redirected to the demo."*），**未找到条款** | 未核实 | 未核实（失败原因：demo 页无条款文本） |

- 【关键纠正 / 新事实】survey §7 第 10 项原把 DASH-IF 列为「正式 ToS 未核实」；本次在 `https://testassets.dashif.org/` 找到**正式的一手许可声明**（CC BY-NC-ND 4.0 + DASH-IF 对「商业客户端开发视为非商业」的解释）。这是本次唯一找到明确条款的测试流提供方，且其 ND/NC 限制明确**禁止把测试向量本身随产品分发**。
- 【未核实】Apple / Mux / Unified Streaming / Bitmovin / Akamai / Shaka demo assets 的正式 ToS 与「是否允许自动化测试、产品内置」：各站页面均无条款页（失败原因：站点未提供 ToS 页），仍不可知。注意 **「无条款」≠「允许」**，不得反推授权。

---

## 3. FFmpeg 4.4 `hls.c` 对 `nullptr` base URL 的实际调用链（§7 第 8 项逐字核对）

一手：FFmpeg 官方仓库 tag `n4.4.4` 源码（`https://github.com/FFmpeg/FFmpeg/tree/n4.4.4`，以 `raw.githubusercontent.com` 逐字抓取）；本地项目亦为 4.4.4（`sdk/3rdparty/CMakeLists.txt:69`、`build.sh`）。

### 3.1 调用链（逐字）
- 【事实】`https://raw.githubusercontent.com/FFmpeg/FFmpeg/n4.4.4/libavformat/utils.c:538` `if (!(s->url = av_strdup(filename ? filename : "")))` ⇒ `avformat_open_input(&fmt, nullptr, ...)` 时 **`s->url = ""`（空串，不是 NULL）**。
- 【事实】`utils.c:430-438` `init_input()`：`if (s->pb) { s->flags |= AVFMT_FLAG_CUSTOM_IO; ... return 0; }` —— 有自定义 `pb` 时**直接返回，不打开 URL**；同段 `:436-437` 逐字日志 *"Custom AVIOContext makes no sense and will be ignored with AVFMT_NOFILE format."*（**§7 第 8 项所说「来自二手引用」的日志，现已从官方源码逐字核对**；但仅对 `AVFMT_NOFILE` 格式打印，HLS demuxer 不是 NOFILE，故不触发）。
- 【事实】`hls.c:1865-1886` `hls_read_header()`：`parse_playlist(c, s->url, NULL, s->pb)` —— **base URL 就是 `s->url`**（空串），第 4 参 `s->pb` 是自定义 AVIO，仅用于喂**初始 playlist** 的字节流。
- 【事实】`hls.c:712` `static int parse_playlist(HLSContext *c, const char *url, struct playlist *pls, AVIOContext *in)`；`:935` `ff_make_absolute_url(tmp_str, sizeof(tmp_str), url, line)` 用 `url`（=空串 base）解析每个相对 segment URI；`:862` / `:919` 对 key URI 同理。
- 【事实】`hls.c:297-313` `new_playlist(c, url, base)`：`ff_make_absolute_url(pls->url, sizeof(pls->url), base, url); if (!pls->url[0]) { ... return NULL; }`。
- 【事实】`hls.c:625-692` `open_url()`：`ret = s->io_open(s, pb, url, AVIO_FLAG_READ, &tmp);`（`:688`/`:691`）—— 每个 segment 通过 `AVFormatContext.io_open` 打开**独立** AVIOContext。
- 【事实】`hls.c:2000-2023`：对每个 playlist 设 `pls->ctx->io_open = nested_io_open`、`ff_copy_whiteblacklists(pls->ctx, s)`，然后 `avformat_open_input(&pls->ctx, pls->segments[0]->url, in_fmt, NULL)` —— 用 **segment URL** 重新打开子 demuxer。
- 【事实】`https://raw.githubusercontent.com/FFmpeg/FFmpeg/n4.4.4/libavformat/options.c:169-191` `io_open_default()`：`return ffio_open_whitelist(pb, url, flags, &s->interrupt_callback, options, s->protocol_whitelist, s->protocol_blacklist);`；`:205-206` `avformat_get_context_defaults()` 设 `s->io_open = io_open_default`。
- 【事实】`https://raw.githubusercontent.com/FFmpeg/FFmpeg/n4.4.4/libavformat/aviobuf.c:1145-1169`：`ffio_open_whitelist(...)` 调用 `ffurl_open_whitelist(..., whitelist, blacklist, NULL)`；`avio_open2()` 是 `ffio_open_whitelist(s, filename, flags, int_cb, options, NULL, NULL)`（whitelist/blacklist 传 NULL 的薄封装）。`:69` AVOption *"protocol_whitelist", "List of protocols that are allowed to be used", ..., { .str = NULL }*。
- 【事实】`https://raw.githubusercontent.com/FFmpeg/FFmpeg/n4.4.4/libavformat/avformat.h:1783-1793` `io_open` 字段：*"Called to open further IO contexts when needed for demuxing. ... The function should behave like avio_open2() ... If NULL then some simple checks are used together with avio_open2()"*。

### 3.2 `ff_make_absolute_url` 对空 base 的真实行为（**纠正 survey 的简化表述**）
- 【事实】`https://raw.githubusercontent.com/FFmpeg/FFmpeg/n4.4.4/libavformat/url.c:319-323` `ff_make_absolute_url()` 直接转调 `ff_make_absolute_url2(buf, size, base, rel, HAVE_DOS_PATHS)`；`url.c:224-225` `if (!base) base = "";`；`url.c:89-148` `ff_url_decompose()` 对空串返回 `0` 且各组件指针都等于 `end`（无 scheme/authority/path）。
- 【推断 · 源码逻辑】据此，`ff_make_absolute_url2` 对 **NULL / 空 base + 相对 URL**：KEEP 宏不复制任何 base 前缀，`use_base_path = 0`，else 分支 `COPY(uc.path, ...)` ⇒ **输出 = 相对 URI 本身，buf 非空，返回 0**（`url.c:255-316` 逐字）。`if (!pls->url[0])`（`hls.c:309`）**不会**因「空 base」被触发；该检查针对结果字符串为空的情形（如 `rel` 为空）。此为静态源码阅读推论，未构造实际输入运行验证。
- 【推断 · 对规格的修正建议】survey §2.3.5 / §5 P2 把失败机制写成「`nullptr` base ⇒ `if (!pls->url[0]) return NULL` 直接失败」，与 4.4 源码**不完全吻合**。真实约束是：自定义 `pb` 只承载**初始 playlist** 字节；之后每个 segment / variant 都通过 `s->io_open` → `ffio_open_whitelist`，或 `avformat_open_input(&pls->ctx, pls->segments[0]->url, ...)` 用 playlist 内的 URL（缺 base 时是**相对路径**）重新打开独立资源；无 scheme 的相对路径无法在 `ffurl_open_whitelist` 中解析协议 ⇒ 打开失败；若 playlist 内恰为绝对 URL 则能打开，但那已绕过自定义 AVIO。**工程结论不变**（自定义 AVIO + `nullptr` 接不了 HLS，须走 FFmpeg 原生 URL 层并给真实 URL），但**失败机制的表述应修正**。规格文档若引用了错误机制，属「会改变规格裁决措辞」的部分，建议由编排层统一收口（本子代理不修改规格）。

### 3.3 `ff_make_absolute_url` 与 `avio_open2` / `ffio_open_whitelist` 的关系（回答票内问法）
- 【事实】三者不在同一层：`ff_make_absolute_url`（`url.c`）只做 **URL 字符串拼接 / 解析**；`avio_open2`（`aviobuf.c:1166`）是公开 API，内部 `ffio_open_whitelist(..., NULL, NULL)`；`ffio_open_whitelist`（`aviobuf.c:1145`）才是带 `whitelist/blacklist` 的实际打开函数，被 `io_open_default`（`options.c:191`）以 `s->protocol_whitelist` 调用。
- 【事实】`hls.c` **不直接调用** `avio_open2` 或 `ffio_open_whitelist`（全文 grep 未出现），而是调 `s->io_open`（`hls.c:688` / `:691` / `:756`），其默认值 `io_open_default` 再落到 `ffio_open_whitelist`。§7 第 8 项「实际 `avio_open2` / `ffio_open_whitelist` 调用未逐字核对」至此闭合：关系是 `hls.c → s->io_open → io_open_default → ffio_open_whitelist`，`avio_open2` 是同底层函数的「无白名单」公开入口。

---

## 4. `protocol_whitelist` 具体取值与 `dash` demuxer 是否启用

### 4.1 本地一手（时点快照 2026-11-11，只读）
- 【事实】`D:/workspace/SoundBridge/sdk/3rdparty/CMakeLists.txt:69` `set(FFMPEG_ROOT ${3RDPARTY_DIR}/ffmpeg-4.4.4)`；`:79-84` 执行 `ffmpeg-4.4.4/build.sh`。
- 【事实】`D:/workspace/SoundBridge/sdk/3rdparty/ffmpeg-4.4.4/build.sh:28-41`：`./configure --prefix=... --ln_s="cp -f" --enable-shared --enable-gpl --enable-version3 --enable-nonfree --disable-postproc --disable-debug --disable-doc --disable-ffmpeg --disable-ffplay --disable-ffprobe --disable-asm` —— **未传任何 `--enable-demuxer=` / `--disable-demuxer=` / `--enable-protocol=` / `--disable-everything`**，即采用 configure 默认组件集。
- 【事实】`D:/workspace/SoundBridge/sdk/3rdparty/ffmpeg-4.4.4/configure:3318` `dash_demuxer_deps="libxml2"`。
- 【事实】`D:/workspace/SoundBridge/sdk/3rdparty/ffmpeg-4.4.4/config.h`（configure 生成，仓库内已提交）：
  - `:503` `#define CONFIG_LIBXML2 0`
  - `:2217` **`#define CONFIG_DASH_DEMUXER 0`** ← **dash demuxer 未启用**
  - `:2266` `#define CONFIG_HLS_DEMUXER 1`；`:2315` `#define CONFIG_MPEGTS_DEMUXER 1`
  - `:590` `#define CONFIG_NETWORK 1`
  - `:2665` `CONFIG_FILE_PROTOCOL 1`、`:2670` `CONFIG_HTTP_PROTOCOL 1`、`:2672` `CONFIG_HTTPS_PROTOCOL 1`、`:2679` `CONFIG_RTMP_PROTOCOL 1`、`:2690` `CONFIG_TCP_PROTOCOL 1`、`:2691` `CONFIG_TLS_PROTOCOL 1`、`:2692` `CONFIG_UDP_PROTOCOL 1`
- 【推断】因果链：build.sh 未启用 libxml2 ⇒ `CONFIG_LIBXML2 0` ⇒ `dash_demuxer_deps` 不满足 ⇒ `CONFIG_DASH_DEMUXER 0`。这直接坐实 survey §7 第 7 项「是否启用 dash demuxer」= **否**；也支持 §2.4.2「DASH 投入产出比最低 / P3 暂不做」。
- 【注意】`config.h` 是仓库内某次 configure 产物，**不同平台 / 重新 configure 后可能变化**；本结论是「2026-11-11 仓库内该文件」的时点快照。

### 4.2 `protocol_whitelist` 具体取值
- 【事实】`D:/workspace/SoundBridge/sdk/extractor/` 多个提取器（`ape/APEPacketReader.cpp:142`、`asf/ASFPacketReader.cpp:142`、`m4a/M4APacketReader.cpp:146`、`mkv/MKVPacketReader.cpp:142` 等）逐字：`m_fmt->flags |= AVFMT_FLAG_CUSTOM_IO; int ret = avformat_open_input(&m_fmt, nullptr, nullptr, nullptr);`。
- 【事实】全仓 grep（排除 `3rdparty` / `.git`）`protocol_whitelist` **零匹配** ⇒ 项目**未显式设置**该字段，也未通过 `AVDictionary` 传 `protocol_whitelist`。
- 【事实】`https://raw.githubusercontent.com/FFmpeg/FFmpeg/n4.4.4/libavformat/avformat.h:1805-1808` 字段 `char *protocol_whitelist;`（*"decoding: set by user"*）；`https://raw.githubusercontent.com/FFmpeg/FFmpeg/n4.4.4/libavformat/aviobuf.c:69` AVOption 默认 `{ .str = NULL }`（*"List of protocols that are allowed to be used"*）。
- 【结论】`protocol_whitelist` 具体取值 = **NULL（未设置 / 不限制协议）**；白名单检查由 `io_open_default → ffio_open_whitelist(..., s->protocol_whitelist=NULL, ...)` 执行。若将来走 FFmpeg 原生 URL 层打开清单类 URL，应显式设置 `protocol_whitelist`（如 `file,http,https,tcp,tls,crypto`）并配 `interrupt_callback` —— 这是**建议**，非既有实现。

---

## 5. ICY `Icy-MetaData` 官方规范与 Radio Paradise 伪 `Content-Range`

### 5.1 是否有单一官方规范
- 【事实】Icecast 官方 YP 文档 `https://icecast.org/docs/icecast-2.0.2/yp.html` 逐字内容只讲 YP 目录（`http://dir.xiph.org`、`<yp-url>`、源客户端 `Ice-Public: 1`），**未涉及** `Icy-MetaData` / `icy-metaint` / `StreamTitle` / HTTP Range / 206 / Content-Range。
- 【事实】Icecast 官网 `http://www.icecast.org/` 自述 *"supports open standards for communication and interaction"*，但站内无 ICY metadata 协议规范页（WebSearch 2026-11-11 未找到官方规范页）。
- 【事实】权威描述均为**社区抄本**，非官方规范：
  - `http://www.smackfu.com/stuff/programming/shoutcast.html`（原始文档已下线，经 `https://blog.csdn.net/xiaogazhang/article/details/47415079` 转载）逐字：*"The protocol isn't documented anywhere officially (of course)."*
  - `http://ample.sourceforge.net/developers.shtml` "ShoutCast metadata protocol"：`Icy-MetaData: 1` → `ICY 200 OK` + `icy-metaint:16000`，长度字节 ×16、`StreamTitle='...';`。
  - `http://gist.githubusercontent.com/niko/2a1d7b2d109ebe7f7ca2f860c3505ef0` 的 `icy_meta.md` 逐字：*"The client adds an HTTP header ... Icy-Metadata: 1. The server may then splice the metadata into the stream... icy-metaint: 16000."*，并注明 *"I haven't found that in any spec though"*，引用 Icecast 源码 `src/format_mp3.c#L291` 作为 metaint 上限依据。
- 【事实】Icecast 官方源码确有 ICY 元数据实现：`https://gitlab.xiph.org/xiph/icecast-server/-/raw/master/src/format_mp3.c:117/234/739/748` 出现 `icy-metaint` / `icy-metadata` / `snprintf(..., "icy-metaint:%u\r\n", ...)`。
- 【结论】**没有单一官方规范**。`Icy-MetaData` 源自 SHOUTcast（Nullsoft 专有协议），Icecast 以兼容方式实现（源码逐字可见），但不发布 IETF RFC 或官方协议规范页；survey §7 第 6 项结论成立，本次以官方 YP 文档 + 官方源码 + 社区抄本交叉确认。

### 5.2 Radio Paradise 伪 `Content-Range`：服务端特定 vs Icecast 通用
- 【事实 · 本次实测 2026-11-11，只读响应头、body 丢弃、`--max-time 5`】对 `http://stream.radioparadise.com/aac-128` 发 `Range: bytes=0-1023` + `Icy-MetaData: 1`，返回：
  ```
  HTTP/1.1 206 Partial Content
  Content-Length: 1024
  Accept-Ranges: bytes
  Content-Range: bytes 0-1023/1073741823
  Content-Type: audio/aac
  Icy-Br: 128 / Icy-Description / Icy-Genre / Icy-Metaint: 16000 / Icy-Name / Icy-Pub: 1 / Icy-Url
  Ice-Audio-Info: channels=2;samplerate=44100;bitrate=128
  Server: openresty/1.17.8.2
  ```
  （`1073741823 = 2^30 − 1`，伪总长）
- 【事实】`Server: openresty/1.17.8.2` ⇒ 前端是 **OpenResty / Nginx**，不是 Icecast（对照 survey §2.2.2 实测 SomaFM `Server: Icecast 2.4.0-kh22-Soma1.7`，无 `Content-Length`、无 `Accept-Ranges`）。
- 【事实】Icecast 官方源码 4 个核心文件 `connection.c` / `client.c` / `source.c` / `format_mp3.c`（`https://gitlab.xiph.org/xiph/icecast-server/-/raw/master/src/<file>`）grep `content-range|206|partial content|accept-ranges|range request` **零匹配**；只有 `format_mp3.c` 的 `icy-metaint` / `icy-metadata`。⇒ Icecast 服务端不实现 HTTP Range / 206 / Content-Range 响应。
- 【事实】RFC 9110 §14.1 / §14.4（`https://www.rfc-editor.org/rfc/rfc9110.txt:6616-6652`）逐字：*"For byte ranges, a sender SHOULD indicate the complete length ... An asterisk character ("*") in place of the complete-length indicates that the representation length was unknown"*；*"The Content-Range header field has no meaning for status codes that do not explicitly describe its semantic. For this specification, only the 206 ... and 416 ... status codes describe a meaning for Content-Range."* ⇒ 对无限直播流，规范做法是未知长度用 `*`，而非编造一个 1 GiB 的 complete-length。
- 【结论 · 高置信度推断】Radio Paradise 的 `206 + Content-Range: bytes 0-1023/1073741823` 是**其 OpenResty 前端的服务端特定行为**，**不是 Icecast 通用做法**。这是把 survey §7 第 14 项从「未核实」推进为「可定性」的部分。
- 【未核实 · 保留】未在「纯净的 Icecast 实例」上做 Range 对照实测（SomaFM ToS 明确拒绝第三方客户端 / 自动化，遵守已知红线故不再探测；亦未找到其他干净的 Icecast 对照实例）；Icecast 源码只查了 4 个核心文件，未穷尽全仓。故「不是 Icecast 通用做法」为**基于服务端类型 + 源码 + 规范的推断**，非穷尽实测。

---

## 6. RFC 8216 / RFC 9110 对 VOD / LIVE 的界定，及 `AVSEEK_SIZE <0` / `seekable=0` 官方语义

### 6.1 RFC 8216（HLS，Informational）逐字措辞
一手 `https://www.rfc-editor.org/rfc/rfc8216.txt`（2026-11-11 抓取逐字）：
- 【事实】Abstract `:18-21` *"This document describes a protocol for transferring **unbounded streams** of multimedia data. ... It describes version 7 of this protocol."*；Status of This Memo 标注 **Informational**（非 Standards Track）。
- 【事实】VOD（有界 / 不可变）：
  - `:1308-1310` *"... If the ... value is Video On Demand (VOD), **the Media Playlist cannot change**."*
  - `:2135-2137` *"An EXT-X-PLAYLIST-TYPE tag with a value of VOD indicates that **the Playlist file MUST NOT change**."*
  - `:2198-2200` *"If the Media Playlist contains the final Media Segment of the presentation, then the Playlist file **MUST contain the EXT-X-ENDLIST tag**"*。
  - `:1273-1279` `#EXT-X-ENDLIST` *"indicates that **no more Media Segments will be added** to the Media Playlist file."*
- 【事实】LIVE（滑动窗口）：
  - `:2202-2205` *"If a Media Playlist does not contain the EXT-X-ENDLIST tag, the server **MUST make a new version of the Playlist file available** that contains at least one new Media Segment."*
  - `:2218-2221` *"6.2.2. Live Playlists — The server **MAY limit the availability of Media Segments by removing Media Segments** from the Playlist file"*。
  - `:2232-2235` *"The server **MUST NOT remove a Media Segment** from a Playlist file without an EXT-X-ENDLIST tag if that would produce a Playlist whose duration is less than three times the target duration."*
  - `:2272-2273` *"**A Live Playlist MUST NOT contain the EXT-X-PLAYLIST-TYPE tag**, as no value of that tag allows Media Segments to be removed."*
  - 客户端 `:2458-2461` *"... the client **SHOULD assume that each Media Segment in it will become unavailable** at the time that the Playlist file was loaded plus the duration of the Playlist file."*
  - `:2089-2090` *"Any Media Segment that is specified in a Playlist loaded by a client **MUST be available for immediate download**"*；`:2431-2433` *"All clients **MUST support HTTP schemes**"*；`:1997-1999` *"If the Playlist does not contain the EXT-X-ENDLIST tag, the TIME-OFFSET **SHOULD NOT be within three target durations of the end**"*。
- 【事实 · 措辞边界（会改变规格裁决的澄清）】RFC 8216 **没有**「VOD 可 seek / LIVE 不可 seek」或「clients MUST NOT seek/rewind beyond the window」的逐字 MUST 条款。它用的是「Playlist 是否可变」「是否可移除分片」「是否必须带 ENDLIST」来界定 VOD 与 LIVE；「有界、可 seek」是**工程推论**（VOD 分片固定 ⇒ 可随机访问）。survey §2.3.1 已注明此点，本次逐字核对确认。规格文档引用时应写「RFC 8216 界定 VOD/LIVE 的措辞是 ENDLIST / PLAYLIST-TYPE / 滑动窗口」，**不要**写成「RFC 规定 LIVE 不可 seek」。

### 6.2 RFC 9110 的角色（澄清票内并列措辞）
- 【事实】`https://www.rfc-editor.org/rfc/rfc9110.txt`（2026-11-11 抓取）全文 grep `VOD` / `live stream` **零匹配** ⇒ RFC 9110 **不界定 VOD / LIVE**，它是 HTTP Semantics 通用规范，只界定 Range / 206 / Content-Range / Accept-Ranges。
- 【事实】`:6301-6305` *"**Range requests are an OPTIONAL feature of HTTP**, designed so that recipients not implementing this feature ... can respond as if it is a normal GET request"*；`:6544-6576` §14.3 `Accept-Ranges`（不支持可发 `Accept-Ranges: none`）；`:6586-6652` §14.4 `Content-Range`（未知 complete-length 用 `*`；Content-Range 仅对 206/416 有意义）。
- 【事实】RFC 7233 已被 RFC 9110 废弃（survey §2.5.2 引用的 `https://www.rfc-editor.org/rfc/rfc7233` 页面标注 obsolete）⇒ 现行合规判断应引用 **RFC 9110**，而非 RFC 7233。这是「会改变规格裁决」的版本修正。
- 【结论】票把「RFC 8216 / RFC 9110 对 VOD 与 LIVE 的界定」并列：实际**只有 RFC 8216 界定 VOD/LIVE**；RFC 9110 只提供「点播可 seek 依赖 Range/206 而直播流无 Range」的 HTTP 层依据。VOD（有界、可 seek）与 LIVE（滑动窗口、不可 seek）的协议依据是 RFC 8216 + RFC 9110 组合，但「可 seek」本身不是 RFC 逐字授权。

### 6.3 FFmpeg doxygen / 源码对 `AVSEEK_SIZE <0` 与 `seekable=0` 的语义
一手 `https://raw.githubusercontent.com/FFmpeg/FFmpeg/n4.4.4/libavformat/avio.h` 与版本化 doxygen `https://ffmpeg.org/doxygen/4.4/avio_8h.html` / `https://ffmpeg.org/doxygen/4.4/avio_8h_source.html`：
- 【事实】`avio.h:528-531` 逐字：*"return the filesize without seeking anywhere. **Supporting this is optional. If it is not supported then the seek function will return <0.**"* / `#define AVSEEK_SIZE 0x10000`。
- 【事实】`avio.h:534-539` `AVSEEK_FORCE`：*"seek by any means (like **reopening and linear reading**) or other normally unreasonable means that can be **extremely slow**. This may be ignored by the seek code."* / `#define AVSEEK_FORCE 0x20000`。
- 【事实】`avio.h:258-260` 逐字：*"**A combination of AVIO_SEEKABLE_ flags or 0 when the stream is not seekable.**"* / `int seekable;`；`:38-40` `AVIO_SEEKABLE_NORMAL (1 << 0)` — *"**Seeking works like for a local file.**"*
- 【事实】`avio.h:445-450`（`avio_alloc_context` 注释）逐字：*"A function for refilling the buffer, may be NULL. **For stream protocols, must never return 0 but rather a proper AVERROR code.**"*（对应 `read_packet`）。
- 【事实】`https://raw.githubusercontent.com/FFmpeg/FFmpeg/n4.4.4/libavformat/aviobuf.c:960` 逐字 `(*s)->seekable = h->is_streamed ? 0 : AVIO_SEEKABLE_NORMAL;` —— 官方源码里 `is_streamed` 为真时 `seekable` 置 0，与「流不可 seek」一致。
- 【结论】`AVSEEK_SIZE` 返回 `<0` = 不支持取文件大小（官方措辞逐字）；`AVIOContext.seekable = 0` = 流不可 seek（官方措辞逐字）。二者语义与 survey §2.5.1 / §5 P1 的引用一致，本次逐字核对确认，可作为规格依据。

---

## 7. 三档归类表（时点快照 2026-11-11）

> 三档定义：① **可进 CI** = 可作为自动化门禁依赖；② **可进产品默认列表** = 可随产品预置 / 内置分发；③ **仅手工冒烟** = 人工一次性验证 / 协议观测，不进门禁、不进产品。
> **硬约束：任何公网 URL 都不得写成 CI 门禁依赖**（票要求 + survey §3.2）。因此第 ① 档中**没有任何公网 URL**；CI 门禁只能依赖「本地 mock HTTP server + 固定素材（素材文件预先落到本地）」。

| # | 对象 | 一手依据（URL / 本地路径） | ① 可进 CI | ② 可进产品默认列表 | ③ 仅手工冒烟 |
|---|---|---|---|---|---|
| 1 | 本地 mock HTTP + 固定素材（离线） | survey §3.2 L0；素材授权见下 2/3/4 | **是**（唯一可进门禁的形态；公网项一律排除） | 不适用 | — |
| 2 | samplelib 素材（下载成本地固定文件后） | `https://samplelib.com/` ; `https://download.samplelib.com/sample-mp3.html` | **是**（仅作为本地素材副本；措辞 "no license restrictions"，无独立 ToS / 热链授权） | **否**（未获产品分发授权；无条款页） | 是（公网直链） |
| 3 | MDN learning-area 素材（audio.mp3） | `https://github.com/mdn/learning-area/blob/main/LICENSE`（CC0 1.0） | **是**（许可最明确，PD 级；建议首选本地素材） | **否**（未逐文件核实第三方来源；产品预置列表另有法律边界） | 是（raw 直链） |
| 4 | file-examples-download 素材（本地副本） | `https://file-examples-download.com/?page_id=9` | **是**（仅开发测试用途 "test your app"；无正式条款） | **否** | 是（公网直链） |
| 5 | 公网点播直链（samplelib / file-examples / archive.org / Wikimedia / MDN raw） | survey §0.1 A、§4 | **否**（公网 URL 不进门禁） | **否** | **是** |
| 6 | Apple bipbop / advanced VOD | `https://developer.apple.com/streaming/examples/`（"for testing only"） | **否** | **否**（"for testing only"） | **是** |
| 7 | Mux test-streams VOD / LL-HLS | `https://test-streams.mux.dev/`（无条款） | **否** | **否**（无授权，不可推断允许） | **是** |
| 8 | Unified Streaming demo VOD | `https://demo.unified-streaming.com/`（无条款） | **否** | **否**（无授权） | **是** |
| 9 | hls.js 官方纯音频测试流（mp3Audio 等） | `https://github.com/video-dev/hls.js/blob/master/tests/test-streams.js` ; `LICENSE` Apache-2.0（仅代码） | **否**（公网 URL） | **否**（第三方流不在仓库许可范围） | **是** |
| 10 | DASH-IF test vectors | `https://testassets.dashif.org/`（CC BY-NC-ND 4.0 + 开发视为非商业） | **否**（公网 URL 不进门禁） | **否**（NC + ND，明确「测试向量本身不得为商业目的分发」） | **是**（开发 / 手工测试） |
| 11 | Shaka demo assets（Angel One 等） | `https://github.com/shaka-project/shaka-player/blob/main/LICENSE`（Apache-2.0 仅代码）; demo 页无条款 | **否** | **否**（媒体资产条款未核实） | **是** |
| 12 | Akamai / Bitmovin / JW Player 测试流 | survey §2.3.3、§2.4.1（部分 403） | **否** | **否** | **是** |
| 13 | ICY / HLS LIVE 滑动窗口（SomaFM、Radio Paradise、Mux LL-HLS、Akamai live、SomaFM HLS） | survey §2.2/§2.3；`https://somafm.com/legal/tos.html` | **禁止** | **禁止** | 仅一次性人工协议观测 |
| 14 | SomaFM（红线，不推翻） | `https://somafm.com/legal/tos.html`（2026-09-04：*"We can't grant permission for third-party SomaFM clients or applications, even noncommercial ones"*；*"not approving new third-party clients"*） | **禁止** | **禁止** | 不进（仅历史一次性观测记录） |
| 15 | 产品内置「公开电台列表」 | survey §3.4 | 不适用 | **禁止 / 不建议**（只提供用户手动输入 URL） | — |

**「可进 CI」的项（严格口径）**：只有 **#1 本地 mock HTTP + 固定素材**（离线、进程私有端口 / 临时目录，遵循 `D:/workspace/SoundBridge/AGENTS.md:111-117`）；其中素材来源按授权优先级推荐 **#3 MDN learning-area（CC0 1.0）> #2 samplelib（no license restrictions，无独立 ToS）> #4 file-examples（test your app，无正式条款）**。**没有任何公网 URL 可以写进 CI 门禁**。

---

## 8. 仍未核实项（明确留白，勿当作结论）

本次 6 项复核后仍不可知 / 未闭合的条目（附失败原因）：

1. **samplelib / file-examples-download 是否允许热链与自动化高频访问**：两站均无独立 ToS 页，页面未提 hotlinking / automated / rate limit ⇒ 未核实（失败原因：站点未提供条款页）。「无条款」不得反推「允许」。
2. **Apple / Mux / Unified Streaming / Akamai / Bitmovin / Shaka demo assets 的正式 ToS，及「是否允许自动化测试 / 产品内置」**：各站页面均无条款页（Shaka demo 页重定向至发布页，仍无条款）⇒ 未核实（失败原因：无公开条款文本）。仅 DASH-IF 找到正式条款（见 §2）。
3. **MDN learning-area 音频素材逐文件来源 / 是否含第三方权利要求**：仓库级 LICENSE 为 CC0 1.0，但未逐文件核实 `audio.mp3` 等素材的来源与署名 ⇒ 未核实。
4. **Radio Paradise 伪 `Content-Range` 是否在纯净 Icecast 实例上绝不出现**：已定性为 OpenResty 前端特定行为（服务端头 + Icecast 源码 + RFC 9110 三重证据），但受 SomaFM ToS 红线约束未对纯 Icecast 实例做 Range 对照实测，Icecast 源码仅查 4 个核心文件未穷尽 ⇒ 部分未核实（推断，非穷尽实测）。
5. **jPlayer 素材授权**（survey §7 第 2 项）：实测已失效（`200 text/html` / 连接失败），授权未核实。
6. **Chrome/Chromium、W3C 官方测试音频直链与授权**（survey §7 第 3 项）：本次复核范围外（票只列 6 项），未核实。
7. **Radio Browser 数据许可「CC0」措辞**（survey §7 第 4 项）：官网原文为 "public domain"，本次未纳入 6 项范围，未核实。
8. **dir.xiph.org `yp.xml` 数据许可与自动化访问条款**（survey §7 第 5 项）：本次未纳入范围，未核实。
9. **SomaFM 旧直链 `ice.somafm.com/groovesalad` 404 的确切原因**（survey §7 第 9 项，地域 / UA / 挂载迁移）：本次未纳入范围，未核实。
10. **HLS `#EXT-X-BYTERANGE` 与 FFmpeg 4.4 `hls.c` 的具体交互细节**（survey §7 第 11 项）：本次逐字核对了 `hls.c` 的 `open_input`（`offset` / `end_offset` AVDictionary，`:1248-1249`）但**未逐行核对该标签的完整处理链** ⇒ 未核实。
11. **DASH 规范 ISO/IEC 23009-1 原文**（survey §7 第 12 项，付费标准）：未引用原文，未核实。
12. **各站 `robots.txt` 内容**（survey §7 第 13 项）：本次未取，未核实。
13. **Apple "for testing only" 是否包含「允许自动化测试」的明确授权**：页面只写 "for testing only"，未区分人工 / 自动化 ⇒ 未核实（不可由「测试用途」反推「允许高频自动化」）。

**已由本次复核闭合的 §7 条目**（供编排层对照）：
- §7 第 1 项（samplelib / file-examples 完整 ToS / 热链）：部分闭合 —— 确认「无独立 ToS 页、无热链 / 自动化措辞」，但「是否允许」仍不可知（见上 1）。
- §7 第 2 项（MDN learning-area 素材许可证）：闭合为 **CC0 1.0 Universal**（仓库级 LICENSE），逐文件来源仍留白。
- §7 第 6 项（ICY 单一官方规范）：闭合 —— **不存在单一官方规范**。
- §7 第 7 项（dash demuxer / protocol_whitelist）：闭合 —— `CONFIG_DASH_DEMUXER 0`（未启用）；`protocol_whitelist` 默认 **NULL**（项目未显式设置）。
- §7 第 8 项（hls.c 调用链逐字）：闭合 —— 见 §3，并**修正了「空 base 直接 return NULL」的简化表述**。
- §7 第 10 项（各测试流正式 ToS）：部分闭合 —— DASH-IF 找到正式条款（CC BY-NC-ND 4.0），其余仍无条款页。
- §7 第 14 项（伪 Content-Range 定性）：部分闭合 —— 定性为服务端特定行为（高置信度推断，见上 4 保留）。

---

## 9. 引用的一手来源清单（完整 URL / 本地路径，2026-11-11 可达性已记）

**FFmpeg 4.4.4 官方源码**（tag `n4.4.4`，`https://github.com/FFmpeg/FFmpeg`）
- `https://raw.githubusercontent.com/FFmpeg/FFmpeg/n4.4.4/libavformat/hls.c`
- `https://raw.githubusercontent.com/FFmpeg/FFmpeg/n4.4.4/libavformat/utils.c`
- `https://raw.githubusercontent.com/FFmpeg/FFmpeg/n4.4.4/libavformat/options.c`
- `https://raw.githubusercontent.com/FFmpeg/FFmpeg/n4.4.4/libavformat/aviobuf.c`
- `https://raw.githubusercontent.com/FFmpeg/FFmpeg/n4.4.4/libavformat/url.c`
- `https://raw.githubusercontent.com/FFmpeg/FFmpeg/n4.4.4/libavformat/avio.h`
- `https://raw.githubusercontent.com/FFmpeg/FFmpeg/n4.4.4/libavformat/avformat.h`
- 版本化 doxygen（交叉引用）：`https://ffmpeg.org/doxygen/4.4/avio_8h.html` ; `https://ffmpeg.org/doxygen/4.4/avio_8h_source.html` ; `https://ffmpeg.org/doxygen/4.4/hls_8c_source.html` ; `https://ffmpeg.org/doxygen/4.4/avformat_8h.html`

**本地一手（只读）**
- `D:/workspace/SoundBridge/sdk/3rdparty/CMakeLists.txt:69,79-84`
- `D:/workspace/SoundBridge/sdk/3rdparty/build_option.cmake:5-8`
- `D:/workspace/SoundBridge/sdk/3rdparty/ffmpeg-4.4.4/build.sh:28-41`
- `D:/workspace/SoundBridge/sdk/3rdparty/ffmpeg-4.4.4/configure:3318`
- `D:/workspace/SoundBridge/sdk/3rdparty/ffmpeg-4.4.4/config.h:503,590,2217,2266,2315,2665-2692`
- `D:/workspace/SoundBridge/sdk/extractor/ape/APEPacketReader.cpp:142` ; `D:/workspace/SoundBridge/sdk/extractor/asf/ASFPacketReader.cpp:142` ; `D:/workspace/SoundBridge/sdk/extractor/m4a/M4APacketReader.cpp:146` ; `D:/workspace/SoundBridge/sdk/extractor/mkv/MKVPacketReader.cpp:142`
- `D:/workspace/SoundBridge/AGENTS.md:111-117`（测试并发与写入约定，引用自 survey）
- `D:/workspace/SoundBridge/docs/research/network-audio-sources-survey.md`（2026-11-09 快照，本地只读背景）

**规范**
- `https://www.rfc-editor.org/rfc/rfc8216.txt`
- `https://www.rfc-editor.org/rfc/rfc9110.txt`
- `https://www.rfc-editor.org/rfc/rfc7233`（已 obsolete，仅作版本说明，不作现行依据）

**授权 / 条款页**
- `https://samplelib.com/` ; `https://download.samplelib.com/sample-mp3.html`
- `https://file-examples-download.com/?page_id=9`
- `https://github.com/mdn/learning-area/blob/main/LICENSE` ; `https://raw.githubusercontent.com/mdn/learning-area/main/LICENSE`
- `https://developer.apple.com/streaming/examples/`
- `https://test-streams.mux.dev/`
- `https://demo.unified-streaming.com/`
- `https://github.com/video-dev/hls.js/blob/master/tests/test-streams.js` ; `https://github.com/video-dev/hls.js/blob/master/LICENSE`
- `https://dashif.org/tools/testassets` ; `https://testassets.dashif.org/`
- `https://github.com/shaka-project/shaka-player/blob/main/LICENSE` ; `https://shaka-player-demo.appspot.com/` ; `https://shaka-project.github.io/shaka-player-release/`
- `https://somafm.com/legal/tos.html`（红线，未推翻）

**ICY / Icecast**
- `https://icecast.org/docs/icecast-2.0.2/yp.html` ; `http://www.icecast.org/`
- `https://gitlab.xiph.org/xiph/icecast-server/-/raw/master/src/connection.c` ; `.../client.c` ; `.../source.c` ; `.../format_mp3.c`
- `http://www.smackfu.com/stuff/programming/shoutcast.html`（经 `https://blog.csdn.net/xiaogazhang/article/details/47415079` 转载，社区抄本）
- `http://ample.sourceforge.net/developers.shtml` ; `http://gist.githubusercontent.com/niko/2a1d7b2d109ebe7f7ca2f860c3505ef0`（社区抄本，非官方规范）
- `http://stream.radioparadise.com/aac-128`（2026-11-11 只读响应头实测）
