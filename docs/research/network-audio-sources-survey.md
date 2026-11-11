# SoundBridge 网络音频流（URL 数据源）接入前调研笔记

> 调研日期：2026-11-09（同日完成第二轮独立 curl 复核，见 §0.1）
> 快照声明：URL 可达性与服务条款随时会变、文中源码行号也会漂移；与现行规格冲突时一律以规格为准。
> 性质：**纯调研**，未修改 SoundBridge 任何源码；仅新增本笔记文件。
> 方法：`WebSearch` / `WebFetch` 抓官方一手文档、API、ToS、规范；对候选 URL 做**只读**探测（`curl -I`、`curl --range`、小文本 GET 取 `.m3u8` 头几行），**未下载任何大文件**。ICY 直播流仅取响应头并用 `--max-time` 截断，未落盘音频。
> 引用格式：每条事实性结论后以 `[来源: <完整 URL>]` 标注一手来源；不确定的一律写「未核实」，不编造 URL 与授权条款。

---

## 0. 结论速览

1. **本项目架构与直播流根本不兼容**：自定义 `AVIOContext` + `AVFMT_FLAG_CUSTOM_IO` + `avformat_open_input(&fmt, nullptr, ...)` 假定「已知长度 + 可随机 seek」；而 ICY / HLS LIVE 是「无 Content-Length、不可 seek、无限长」。这不是调参问题，是契约不匹配 [来源: https://ffmpeg.org/doxygen/4.4/avio_8h_source.html] [来源: https://www.rfc-editor.org/rfc/rfc8216]。
2. **FFmpeg 4.4 的 HLS demuxer 会绕开自定义 AVIO**：每个 playlist 有自己的 demuxer / `AVIOContext`，并用 `ff_make_absolute_url(..., base, url)` 解析子 URL；`nullptr` base URL 无法满足 [来源: https://ffmpeg.org/doxygen/4.4/hls_8c_source.html]。
3. **最合规、最稳的候选是「明确供测试的小文件」**（samplelib、file-examples-download、Apple/Mux/hls.js/DASH-IF 测试流），但它们一般**只授权测试用途**，不等于可进产品默认列表 [来源: https://developer.apple.com/streaming/examples/] [来源: https://github.com/video-dev/hls.js/blob/master/tests/test-streams.js]。
4. **SomaFM 2026-09-04 更新的 ToS 明确拒绝第三方客户端**，并把流内容限定为「individual/personal use only」；禁止 embedding / restreaming / ripping / scrobbling。因此它**不能进自动化 CI、不能进产品默认列表** [来源: https://somafm.com/legal/tos.html]。
5. **建议优先级**：本地 mock HTTP + 固定素材（P0）→ 公网静态点播（P1）→ 重构 `DataSource` 契约表达「无长度/不可 seek」（P1）→ HLS 走 FFmpeg 原生 URL 层（P2）→ DASH 暂不做（P3）；产品只提供「用户手动输入 URL」，不内置电台列表。

## 0.1 可直接复制的 URL 速查（2026-11-09 第二轮复核）

> 只列**本轮亲自探测、当前 200/206** 的条目。「能否进 CI」由授权列决定（详见 §3）。公网条目一律**不写进自动化门禁**，只作手工冒烟；门禁请用本地 mock server + 固定素材。

**A. 静态点播** —— 有 `Content-Length` + `Accept-Ranges: bytes`，`Range` 返回 `206`，与现有 `DataSource::readAt(offset, size)` 契约天然兼容，是唯一推荐的 P0 接入形态。

| URL | 格式 / 大小 | 复核结果 | 授权 |
|---|---|---|---|
| `https://samplelib.com/mp3/sample-15s.mp3` | MP3 / 307453 B | 200 `audio/mpeg`；`Range 0-1023` → 206 | 页面称 "no license restrictions"，未见独立 ToS |
| `https://samplelib.com/wav/sample-3s.wav` | WAV / 563756 B | 200 `audio/x-wav`，支持 Range | 同上 |
| `https://file-examples-download.com/wp-content/uploads/filr/63/sample.flac` | FLAC / 68443 B | 200，Range → 206 | 站点声明「test your app」，条款未核实 |
| `https://file-examples-download.com/wp-content/uploads/filr/67/sample.ogg` | OGG / 98185 B | 200 `audio/ogg` | 同上 |
| `https://file-examples-download.com/wp-content/uploads/filr/58/sample.aac` | AAC / 33704 B | 200 `audio/x-aac` | 同上 |
| `https://archive.org/download/island_moreau_librivox/island_dr_moreau_00_wells.mp3` | MP3 / 2828494 B | 302 → 200 `audio/mpeg` | LibriVox PD（仅美国）；archive.org ToS 限 research |
| `https://upload.wikimedia.org/wikipedia/commons/5/53/Rossini_-_Il_Barbiere_di_Siviglia_-_La_calunnia_-_Francesco_Navarini_%281903%29_version_30secondes.ogg` | OGG / 1196609 B | 200 `application/ogg` | 逐文件 CC/PD，需署名；官方不推荐热链 |

**B. 纯音频 / 音视频 HLS VOD** —— Playlist 含 `#EXT-X-ENDLIST`，有界可 seek；但清单类 URL 绕不开自定义 AVIO，必须另走 FFmpeg 原生 URL 层（见 §5 P2）。

| URL | 说明 | 复核 |
|---|---|---|
| `https://playertest.longtailvideo.com/adaptive/vod-with-mp3/manifest.m3u8` | hls.js 官方 "MP3 VOD demo"，纯音频 | 200 `application/x-mpegurl` |
| `https://pl.streamingvideoprovider.com/mp3-playlist/playlist.m3u8` | hls.js 官方 "MPEG Audio Only demo" | 200 |
| `https://playertest.longtailvideo.com/adaptive/alt-audio-no-video/sintel/playlist.m3u8` | 纯音频 + 备用音轨 | 200 |
| `https://devstreaming-cdn.apple.com/videos/streaming/examples/bipbop_adv_example_hevc/master.m3u8` | Apple 官方 "for testing only"，VOD 76 分片 | 200 |
| `https://test-streams.mux.dev/x36xhzz/x36xhzz.m3u8` | Mux Big Buck Bunny，VOD 64 分片 | 200 |
| `https://demo.unified-streaming.com/k8s/features/stable/video/tears-of-steel/tears-of-steel.ism/.m3u8` | 含纯音频 variant `mp4a.40.2` | 200 |

**C. 直播流** —— 仅一次性人工协议观测，**禁止进 CI、禁止进产品默认列表**。

| URL | 协议 | 复核 | 红线 |
|---|---|---|---|
| `https://ice2.somafm.com/groovesalad-128-mp3` | ICY / MP3 | 200 `audio/mpeg`，`icy-metaint:45000`，无 `Content-Length` | SomaFM ToS 明确拒绝第三方客户端 |
| `http://stream.radioparadise.com/aac-128` | ICY / AAC | 200 `audio/aac` + `Icy-*`；见下方「伪长度陷阱」 | 受版权保护的音乐广播，第三方应用条款未核实 |

**D. 目录 / API（只取元数据，不内置）**

| URL | 复核 |
|---|---|
| `https://de1.api.radio-browser.info/json/stations/search?limit=1&hidebroken=true` | 200 `application/json`（数据 PD，需描述性 User-Agent） |
| `https://dir.xiph.org/yp.xml` | 本环境偶发连接超时（`curl` 000）；单次数 MB、大量空 `listen_url` |

### 第二轮复核补遗（正文之外的新发现）

1. **Radio Paradise 的「伪长度」陷阱（重要）**：对 `http://stream.radioparadise.com/aac-128` 发 `Range: bytes=0-1023`，返回 `HTTP/1.1 206 Partial Content` + `Content-Range: bytes 0-1023/1073741823`（1 GiB − 1 的伪总长），同时带齐 `Icy-Br` / `Icy-Name` / `Icy-Pub` 头。⇒ **不能只凭「有 206 / 有 Content-Range」判定为静态可 seek 点播**；必须结合 `Icy-*` 响应头、`Content-Length` 是否随数据增长、连接是否自然终止来区分。若把该伪长度喂给 `getSize()` / `AVSEEK_SIZE`，会把无限直播流误判成 1 GiB 有限文件 [来源: http://stream.radioparadise.com/aac-128]（本笔记 curl 实测）。
2. **SomaFM HLS 演示流已失效**：正文 §2.3.3 记录的 `https://hls.somafm.com/hls/groovesalad/128k/program.m3u8` 在第二轮复核返回 **404**（首轮为 200）；官方 HLS 页同时声明 2026-10-20 已停用实验性 lossless 流 ⇒ 公网 LIVE 流连「一次性观测」都不保证可复现。
3. **Radio Browser 泛域名不可直接用**：`https://api.radio-browser.info/json/stations/search?limit=1` 本轮 **404**，而 `https://de1.api.radio-browser.info/...` 为 200；与官方文档「不要硬编码单个服务器、用 DNS `all.api.radio-browser.info` 发现并随机重试」一致 [来源: https://api.radio-browser.info/]。
4. 纯音频 HLS VOD 四条、Apple / Mux / Unified Streaming VOD、DASH `MultiRatePatched.mpd`、samplelib 与 file-examples 五条静态文件、archive.org LibriVox、Wikimedia OGG 均逐条复核为 200/206，与正文一致；`bitdash-a.akamaihd.net/.../playlist.m3u8` 复核仍为 403。`https://devstreaming-cdn.apple.com/videos/streaming/examples/img_bipbop_adv_example_fmp4/master.m3u8` 与 `https://dash.akamaized.net/akamai/bbb_30fps/bbb_30fps.mpd` 另补测为 200，可作备选。

---

## 1. 调研基线：本项目架构约束（来自已确认背景，作为论证前提）

- FFmpeg 4.4.4 通过自定义 `AVIOContext` 回调（`AVFMT_FLAG_CUSTOM_IO` + `avformat_open_input(&fmt, nullptr, ...)`）从 `DataSource::readAt(offset, buf, size)` 读数据，所有提取器假定「已知长度 + 可随机 seek」。
- `getSize` 走全量扫描；`durationMs == 0` 已允许（元数据层能表达「时长未知」）；`m_abortDecode` 是唯一取消信号；seek 只保留 `seekToMs`。
- 支持 12 种本地格式（wav/mp3/aac/flac/ogg/aiff/ape/m4a/mkv/wma/asf），按扩展名 + magic sniff 选提取器。
- FFmpeg 构建带 `CONFIG_NETWORK=1` / HTTP / HTTPS / TLS(Schannel) / HLS / RTMP / TCP / UDP 协议，但**没编 ffmpeg / ffprobe 命令行工具**。
- 现有规格明确把「网络流 / URL / 边下边播 / 断线重连」划为界外，本次只做接入前调研。

> 项目测试约定：`ctest` 可并行（`-j4`），前提是**每个测试进程只写自己的文件**；「新增用例若往固定名字的共享路径写东西，会把这个稳定性还回去」[来源: d:/workspace/SoundBridge/AGENTS.md:111-117]（本地一手文件，行号 111–117）。

---

## 2. 五类候选 URL 调研

### 2.1 HTTP 渐进式点播文件（progressive download）

> 判定标准：`200 OK` + `Content-Length` + `Accept-Ranges: bytes`，`Range: bytes=0-1023` 返回 `206 Partial Content` + `Content-Range`，即「已知长度 + 可随机 seek」，与 `DataSource::readAt` 契约一致。

#### 2.1.1 SampleLib（推荐作为本地 mock 固定素材首选）

- 站点定位：「A completely free library of test files available to download **without licensing restrictions**」，明确面向 developers / designers / testers [来源: https://samplelib.com/]。
- MP3 页面措辞：「available for download **with no license restrictions**」 [来源: https://download.samplelib.com/sample-mp3.html]；WAV 页面同措辞 [来源: https://download.samplelib.com/sample-wav.html]。
- 确切直链（页面 href 原样）：
  - `https://samplelib.com/mp3/sample-3s.mp3`、`sample-6s.mp3`、`sample-9s.mp3`、`sample-12s.mp3`、`sample-15s.mp3`、`sample-5s.mp3` ~ `sample-55s.mp3`、`sample-speech-1m.mp3` ~ `sample-speech-30m.mp3`、`sample-15s-cbr-64kbps.mp3`、`sample-15s-cbr-128kbps.mp3`、`sample-15s-cbr-192kbps.mp3`、`sample-15s-cbr-320kbps.mp3`、`sample-15s-vbr-v0.mp3`、`sample-15s-id3v2.mp3`、`sample-15s-id3v1.mp3`、`sample-15s-no-tags.mp3` [来源: https://download.samplelib.com/sample-mp3.html]
  - `https://samplelib.com/wav/sample-3s.wav`、`sample-6s.wav`、`sample-9s.wav`、`sample-12s.wav`、`sample-15s.wav`、`sample-speech-1m.wav` ~ `sample-speech-30m.wav`、`sample-3s-8khz.wav`、`sample-3s-16khz.wav`、`sample-3s-48khz.wav`、`sample-3s-96khz.wav`、`sample-3s-stereo.wav` [来源: https://download.samplelib.com/sample-wav.html]
- 实测（2026-11-09）：`https://samplelib.com/mp3/sample-15s.mp3` → `200 OK`，`Content-Length: 307453`，`Accept-Ranges: bytes`，`Content-Type: audio/mpeg`；`Range: bytes=0-1023` → `206 Partial Content` + `Content-Range: bytes 0-1023/307453`。`https://samplelib.com/wav/sample-3s.wav` → `200`，`Content-Length: 563756`，`audio/x-wav` [来源: https://samplelib.com/mp3/sample-15s.mp3]（本笔记 curl 实测）。
- 授权/热链：页面措辞授权「无许可限制下载」，但**未见独立 ToS 页面，未核实是否允许热链与自动化高频访问**。

#### 2.1.2 File Examples / File Examples Download（补齐 FLAC / OGG / AAC / WAV 直链）

- 站点措辞：「Are you tired of searching for audio files with the right license to **test your app**? … You can download free .mp3, .wav, .ac3, .aiff, .amr, .au, **.flac**, .mid, .mka, **.ogg**, .ra, .voc, .wma …」 [来源: https://file-examples-download.com/?page_id=9]；主站另有「Test .mp3 and .wav or other audio files for free」 [来源: http://file-examples.com/index.php/sample-audio-files/]。
- 确切直链（页面 href 原样）：
  - `https://file-examples-download.com/wp-content/uploads/filr/66/sample.mp3`
  - `https://file-examples-download.com/wp-content/uploads/filr/70/sample.wav`
  - `https://file-examples-download.com/wp-content/uploads/filr/63/sample.flac`
  - `https://file-examples-download.com/wp-content/uploads/filr/67/sample.ogg`
  - `https://file-examples-download.com/wp-content/uploads/filr/58/sample.aac`
  - [来源: https://file-examples-download.com/?page_id=9]
- 实测：以上 5 条均 `200`（分别 `audio/mpeg` / `audio/x-wav` / `application/octet-stream` / `audio/ogg` / `audio/x-aac`），文件仅几十 KB；FLAC 直链 `Range: bytes=0-1023` → `206` + `Content-Range: bytes 0-1023/68443` [来源: https://file-examples-download.com/wp-content/uploads/filr/63/sample.flac]（本笔记 curl 实测）。
- **稳定性反例**：`https://file-examples.com/index.php/sample-audio-files/` 以自定义 UA 探测返回 `403` [来源: http://file-examples.com/index.php/sample-audio-files/]（本笔记 curl 实测）。两个域名/站点关系与条款差异未核实。
- 授权：**站点仅声明「供测试」，未核实有无独立 ToS 或 CC/PD 声明**。

#### 2.1.3 Internet Archive + LibriVox（公共领域，但 ToS 限制自动化用途）

- Internet Archive ToS：访问「provided at no cost … granted for **scholarship and research purposes only**」，且用户「agree not to interfere with the work of … servers, or resources」；对带 CC 声明的条目按声明许可使用 [来源: https://archive.org/about/terms]。
- LibriVox：「audiobooks are free … You may use them for **whatever purpose you like**」；但明确注明「our files are free and **public domain in the USA, but not necessarily everywhere in the world**」 [来源: https://librivox.org/?p=31376/]。
- `librivoxaudio` 集合条目使用 `http://creativecommons.org/publicdomain/mark/1.0/` 标记 [来源: https://archive.org/services/collection-rss.php?collection=librivoxaudio]。
- 确切直链（标准 `/download/<identifier>/<file>` 形式）：
  - `https://archive.org/download/island_moreau_librivox/island_dr_moreau_00_wells.mp3`（该形式由第三方教材页逐章列出，URL 本身是 archive.org 官方路径格式）[来源: https://pressbooks.library.torontomu.ca/doctormoreau/?p=90]
  - `https://archive.org/download/island_moreau_librivox/island_dr_moreau_01_wells.mp3` 等同模式章节文件 [来源: https://pressbooks.library.torontomu.ca/doctormoreau/?p=90]
  - 另一变体是带具体存储节点的 `https://ia802808.us.archive.org/2/items/wuthering_heights_rg_librivox/wutheringheights_01_bronte.mp3` [来源: https://pressbooks.library.torontomu.ca/wutheringheights/back-matter/librivox-audio-book/]
- 实测：`https://archive.org/download/island_moreau_librivox/island_dr_moreau_00_wells.mp3` → `302 Found` 跳到 `https://dn711304.ca.archive.org/0/items/island_moreau_librivox/island_dr_moreau_00_wells.mp3`；跟随重定向后 `Range: bytes=0-1023` → `206` + `Content-Range: bytes 0-1023/2828494` [来源: https://archive.org/download/island_moreau_librivox/island_dr_moreau_00_wells.mp3]（本笔记 curl 实测）。
- **重定向风险**：`dn*.us.archive.org` / `ia*.us.archive.org` 是具体存储节点，会随扩容/迁移变化，硬编码节点 URL 不稳 [来源: https://archive.org/download/island_moreau_librivox/island_dr_moreau_00_wells.mp3]（本笔记 curl 实测 302 Location）。
- 热链/自动化：ToS 的「scholarship and research purposes only」+「不得干扰服务器」意味着**不宜作为自动化 CI 的高频依赖**，更不能作为产品默认列表 [来源: https://archive.org/about/terms]。

#### 2.1.4 Wikimedia Commons（CC / PD，逐文件核实；官方不推荐热链）

- 官方复用政策：允许在 Wikimedia 之外使用，「Everyone is allowed to copy, use and modify any files here freely for any purpose **including commercial ones** as long as the source and the authors are credited and, in many cases, … release your copies/improvements under the same freedom」；数据库文本为 CC-BY-SA / GFDL，但**每个媒体文件的许可必须看其描述页，Wikimedia 不保证信息正确性** [来源: https://commons.wikimedia.org/wiki/Commons:First_steps/Reuse]。
- 官方明确：「Directly using a Commons file via embedding its URL ("hotlinking") is also possible, **but is not recommended**」 [来源: https://commons.wikimedia.org/wiki/Commons:Reusing_content_outside_Wikimedia]。
- 确切直链（通过 Commons 官方 API `imageinfo` 取到的真实文件 URL，去掉跟踪 query 后）：
  - `https://upload.wikimedia.org/wikipedia/commons/5/53/Rossini_-_Il_Barbiere_di_Siviglia_-_La_calunnia_-_Francesco_Navarini_%281903%29_version_30secondes.ogg` [来源: https://commons.wikimedia.org/w/api.php?action=query&generator=categorymembers&gcmtitle=Category:Ogg_sound_files&gcmtype=file&gcmlimit=3&prop=imageinfo&iiprop=url&format=json]
  - 实测：`200`，`Content-Type: application/ogg`，`Content-Length: 1196609`，`Accept-Ranges: bytes` [来源: https://upload.wikimedia.org/wikipedia/commons/5/53/Rossini_-_Il_Barbiere_di_Siviglia_-_La_calunnia_-_Francesco_Navarini_%281903%29_version_30secondes.ogg]（本笔记 curl 实测）。
- 通用获取机制：`https://commons.wikimedia.org/wiki/Special:FilePath/<文件名>` 会重定向到 `upload.wikimedia.org`，无需编造哈希路径 [来源: https://commons.wikimedia.org/wiki/Commons:First_steps/Reuse]。

#### 2.1.5 MDN / W3C / Chrome 系测试素材（多为二手引用，稳定性差）

- MDN《Audio and video delivery》文档直接列出 `http://jPlayer.org/audio/mp3/Miaow-01-Tempered-song.mp3`、`.m4a`、`http://jPlayer.org/audio/ogg/Miaow-01-Tempered-song.ogg` 作为 `<source>` 示例 [来源: https://developer.mozilla.org/en-US/docs/Web/Media/Audio_and_video_delivery]。
- **实测失效**：`http://jPlayer.org/audio/mp3/Miaow-01-Tempered-song.mp3` 返回 `200` 但 `Content-Type: text/html`（不是音频）；`https://jplayer.org/audio/ogg/Miaow-01-Tempered-song.ogg` 连接失败 [来源: http://jPlayer.org/audio/mp3/Miaow-01-Tempered-song.mp3]（本笔记 curl 实测）。**jPlayer 素材授权未核实**。
- MDN 自有仓库素材（可用）：`https://github.com/mdn/learning-area/raw/refs/heads/main/html/multimedia-and-embedding/tasks/audio-video/audio.mp3` —— 官方技能测试文档给出，实测 `200 audio/mpeg` [来源: https://developer.mozilla.org/en-US/docs/Learn_web_development/Core/Structuring_content/Test_your_skills/Audio_and_video]（本笔记 curl 实测）。**该仓库音频素材的具体许可证未核实**。
- Google Chrome / Chromium 官方测试素材、`W3C` 官方音频测试文件的确切直链与授权：**未核实**（本轮未找到可确认的一手页面，不编造 URL）。

#### 2.1.6 FFmpeg 官方 FATE 样本（适合作为「本地固定素材」，不是 HTTP 直链）

- 官方 FATE 文档：需要样本时用 `make fate-rsync SAMPLES=fate-suite/`，或 `configure --samples=<path>` [来源: https://ffmpeg.org/fate.html]；文档源码同样说明 [来源: https://git.ffmpeg.org/gitweb/ffmpeg.git/blob/26148e923613e718787c6fc4bf3f64e8909f597c:/doc/fate.texi]。
- 官方 `tests/Makefile` 里 `fate-rsync` 的目标是 `rsync://fate-suite.ffmpeg.org/fate-suite/` [来源: https://ffmpeg.org/pipermail/ffmpeg-devel/2025-April/342876.html]（该 2025-04 官方 patch 引用了 Makefile 原文）。
- 说明：**FATE 样本是 rsync 批量同步、不是 HTTP 渐进式直链**；样本版权状态各异（多为回归测试用的媒体片段），**逐个文件的授权未核实**。因此它更适合作为「本地 mock + 固定素材」的来源，而非公网 URL 候选。
- 结果站 `http://fate.ffmpeg.org/` 只展示测试结果，不提供素材直链 [来源: https://ffmpeg.org/fate.html]。

**2.1 小结（≥3 个确切可用直链）**：`https://samplelib.com/mp3/sample-15s.mp3`（MP3）、`https://samplelib.com/wav/sample-3s.wav`（WAV）、`https://file-examples-download.com/wp-content/uploads/filr/63/sample.flac`（FLAC）、`/67/sample.ogg`（OGG）、`/58/sample.aac`（AAC）、`https://archive.org/download/island_moreau_librivox/island_dr_moreau_00_wells.mp3`（MP3，PD）、`https://upload.wikimedia.org/wikipedia/commons/5/53/...ogg`（OGG）、`https://github.com/mdn/learning-area/raw/refs/heads/main/.../audio.mp3`（MP3）—— 覆盖 MP3 / WAV / FLAC / OGG / AAC，均实测可达且（除 Commons 需逐文件核实许可外）为明确供测试或 PD 素材。

---

### 2.2 Icecast / Shoutcast 直播流（ICY 协议）

#### 2.2.1 ICY 与纯 HTTP 的区别（协议特征）

- 客户端请求头 `Icy-MetaData: 1`，服务器响应 `icy-metaint:<N>`，元数据块按固定字节间隔插入音频流；块首字节 ×16 为长度，内容形如 `StreamTitle='...';` [来源: https://gist.github.com/ericchianti/cd2af56379c4d581e7e666ced556b90f/95d6388049779e52aaf96e3176a8b5830b9f618b]（**社区协议抄本，非官方规范**；Icecast 官方没有单一 ICY metadata 规范页，官方 YP/配置文档只零散涉及 `Ice-Public: 1` 等 [来源: https://www.icecast.org/docs/icecast-trunk/yp/]）。
- 直播流是 `HTTP/1.0 200 OK` 或 `HTTP/1.1 200 OK` + **无 `Content-Length`** + 连接持续推送，不是 `206`，也**没有 `Accept-Ranges`** [来源: https://grokipedia.com/page/Icecast]（二手；以下 SomaFM 实测为一手证据）。
- 官方 PLS 播放列表里 `Length1=-1` 明确表示「长度未知/无限」 [来源: https://somafm.com/groovesalad.pls]（本笔记 curl 实测）。
- 对照：渐进式点播依赖 `Range` / `206` / `Content-Range`，而 `Range` 在 HTTP 里是 **OPTIONAL**，服务器 `MAY ignore` 并按普通 `200 OK` 返回完整表示；不支持时可发 `Accept-Ranges: none` [来源: https://www.rfc-editor.org/rfc/rfc7233]（第 1、2.3、3.1、4.4 节）。⇒ 对直播流发 `Range` 不会得到 `206`，`getSize` 无从取得总长。

#### 2.2.2 SomaFM（重点：官方直链目录 + 2026 最新 ToS）

- 官方「Direct Stream Links」列表（Sonos 自定义页）列出主 PLS 与 Direct URL，例如 `https://somafm.com/groovesalad.pls` / `https://ice.somafm.com/groovesalad`、`https://somafm.com/dronezone.pls` / `https://ice.somafm.com/dronezone`、`https://somafm.com/indiepop.pls` / `https://ice.somafm.com/indiepop`、`https://ice.somafm.com/deepspaceone`、`https://ice.somafm.com/defcon` 等 [来源: https://www.soma.fm/listen/sonoscustom.html]。
- **实测（2026-11-09）这些旧直链已失效**：`https://ice.somafm.com/groovesalad` 与 `https://ice.somafm.com/dronezone` 均返回 `404 File Not Found`（带 CORS 头） [来源: https://ice.somafm.com/groovesalad]（本笔记 curl 实测）。**404 的确切原因（地域限制 / UA / 挂载迁移）未核实**。
- 官方 PLS 现给出的实际挂载点（实测 PLS 内容）：`https://ice2.somafm.com/groovesalad-128-mp3`、`https://ice6.somafm.com/groovesalad-128-mp3`、`https://ice5.somafm.com/groovesalad-128-mp3`，且 `Length1=-1` [来源: https://somafm.com/groovesalad.pls]（本笔记 curl 实测）。
- 对新挂载点的实测响应头：`HTTP/1.0 200 OK`、`Server: Icecast 2.4.0-kh22-Soma1.7`、`Content-Type: audio/mpeg`、`icy-br: 128`、`icy-name: Groove Salad ...`、`icy-metaint: 45000`、**无 `Content-Length`、无 `Accept-Ranges`**；`curl --max-time 5` 超时前已收到 319858 字节且仍在持续 [来源: https://ice2.somafm.com/groovesalad-128-mp3]（本笔记 curl 实测）。这是「无限长度、不可 seek」的一手证据。
- SomaFM 另有官方 HLS 演示/测试页，列出 `https://hls.somafm.com/hls/groovesalad/320k/program.m3u8`、`https://hls.somafm.com/hls/groovesalad/128k/program.m3u8`、`https://hls.somafm.com/hls/groovesalad/64k/program.m3u8`；并注明「As of October 20, 2026, We have discontinued the experimental HLS-lossless format stream (… was being **abused by bots and streamrippers**)」 [来源: http://hls.somafm.com/]。
- **ToS（Last Updated Sep 4, 2026）逐字要点**：
  - 「*We can't grant permission for third-party SomaFM clients or applications, **even noncommercial ones**.*」「***As a result, we're not approving new third-party clients.***」 [来源: https://somafm.com/legal/tos.html]
  - 「You are **not allowed to Embed or restream** our content … "**Scrobbling**" and/or automated re-posting of playlists and now playing information is **against our terms of use**」 [来源: https://somafm.com/legal/tos.html]
  - 「The streaming audio content … is intended for **individual/personal use by end users only**」；禁止 Embedding、Re-streaming、**Downloading / ripping / recording / capturing**、衍生作品 [来源: https://somafm.com/legal/tos.html]
  - 理由：音乐授权协议不允许其授权第三方使用，运营方还可能承担 BMI / SESAC / SoundExchange 年费 [来源: https://somafm.com/legal/tos.html]
  - ⇒ **SomaFM 直链不能进自动化 CI、不能进产品默认列表、不能内置电台目录**；本笔记仅做一次性只读协议探测。

#### 2.2.3 dir.xiph.org（Icecast 官方 YP 目录）

- Icecast 官方文档：「Icecast has it own YP directory located at `http://dir.xiph.org`」，服务端配置为 `<yp-url>http://dir.xiph.org/cgi-bin/yp-cgi</yp-url>`，源客户端需带 `Ice-Public: 1` 才会被收录 [来源: https://www.icecast.org/docs/icecast-trunk/yp/] [来源: https://icecast.org/docs/icecast-2.0.2/yp.html]。
- 官方邮件列表给出的客户端取列表方式：「To get a current list for your player, load `http://dir.xiph.org/yp.xml`. Please make sure you client sends the `Accept-Encoding: gzip` http header」 [来源: https://lists.xiph.org/pipermail/icecast-dev/2013-February/002123.html]。
- 目录首页 `https://dir.xiph.org/` 展示实时电台 [来源: https://dir.xiph.org/]。
- 实测：本环境 GET `https://dir.xiph.org/yp.xml` 返回 `<directory><entry>…` 结构，但**体积达数 MB**，且很多 `<listen_url>` 为空 [来源: https://dir.xiph.org/yp.xml]（本笔记 curl 实测）⇒ 不适合作为 CI 依赖，也不宜在客户端全量拉取。
- **yp.xml 的数据许可 / 自动化访问条款未核实**。

#### 2.2.4 Radio Browser API（数据层最开放）

- 官网：「**Data license: public domain**, software license: GPL」「**Free for ALL! Open API for usage in own apps**」「I give all the rights I have at the **accumulated data** to the public domain」 [来源: https://www.radio-browser.info/]。
  - 注：二手资料常写作「CC0」，但**官网原文是 "public domain"**，该措辞未在官网核实。数据许可只覆盖「电台名称/tag/链接等元数据」，**不覆盖流内容本身的版权** [来源: https://www.radio-browser.info/]。
- **第二轮复核**：泛域名 `https://api.radio-browser.info/json/stations/search?limit=1` 返回 `404`，而 `https://de1.api.radio-browser.info/json/stations/search?limit=1&hidebroken=true` 返回 `200 application/json`；与官方「勿硬编码单服务器、用 DNS `all.api.radio-browser.info` 发现并随机重试」的要求一致 [来源: https://api.radio-browser.info/]。
- 官方 API 文档：「completely free and open source」「You may use it in free and non free software. You may install it on your own server and **mirror all its data**」；要求「**Send a speaking http agent string** (e.g. mycoolapp/1.4)」；服务器列表通过 DNS 查 `all.api.radio-browser.info` 获取，**不要硬编码单个服务器**（如 `https://de1.api.radio-browser.info`），并建议失败时随机重试下一个 [来源: https://api.radio-browser.info/]。
- 「This webservice can be used **freely but without guarantee to work** … downtimes are possible」；请求示例 `GET /json/stations/search?tag=rock&codec=mp3&order=country&reverse=true&limit=1000`，并再次要求描述性 `User-Agent` [来源: http://docs.radio-browser.info/]。
- 确切 endpoint 例子：`https://de1.api.radio-browser.info/json/stations/search`、`https://api.radio-browser.info/json/stations/search?limit=10`、`/json/stations/byuuid/{uuid}`、`/json/url`（点击上报） [来源: https://api.radio-browser.info/] [来源: http://docs.radio-browser.info/]。
- 实测：`https://de1.api.radio-browser.info/json/stations/search?limit=1&hidebroken=true&codec=MP3&order=clickcount&reverse=true` 返回真实 station JSON（含 `url` / `url_resolved` / `codec` / `lastcheckok` 等字段） [来源: https://de1.api.radio-browser.info/json/stations/search]（本笔记 curl 实测）。

#### 2.2.5 公共广播电台官方 ICY 直链（第 2 个候选）

- Radio Paradise 官方「Stream Links」页：「**Stream URL for WiFi Radios & other players** — Use the options below to change the channel, bitrate, and codec. Then copy the generated URL and paste it into your player」，页面给出 `http://stream.radioparadise.com/aac-128`，并提供 32k/64k/128k/192k AAC+、192k Ogg Vorbis+Metadata、192k MP3、320k AAC、FLAC(+Metadata) 等选项 [来源: https://radioparadise.com/listen/stream-links]。
- Wikidata 条目引用官方 stream-links，列出的官方直链：`http://stream.radioparadise.com/aac-320`、`/mellow-320`、`/rock-320`、`/eclectic-320` [来源: https://www.wikidata.org/wiki/Q3416792]。
- FLAC 直播流 `http://stream.radioparadise.com/flac`（及 `/mellow-flac`、`/rock-flac`、`/global-flac`）由第三方资料列出，并注明「These links currently do not include stream metadata」及官方称 FLAC 流「still somewhat experimental」 [来源: https://www.hiresaudio.online/radio-paradise/]（**二手来源，官方 FLAC 直链与条款未逐条核实**）。
- 说明：Radio Paradise 相比 SomaFM 对「其他播放器」更友好（官方主动提供粘贴用 URL），但其播放内容仍是受版权保护的**音乐广播**，仅供个人收听；**是否允许第三方应用内置/自动化访问未核实**。
- **第二轮复核（重要，伪长度陷阱）**：对 `http://stream.radioparadise.com/aac-128` 发 `Range: bytes=0-1023` 返回 `HTTP/1.1 206 Partial Content` + `Content-Range: bytes 0-1023/1073741823`（1 GiB − 1 的**伪总长**），响应同时带 `Icy-Br:128` / `Icy-Name` / `Icy-Pub:1`。⇒ **不能只凭「有 206 / 有 Content-Range」判定为静态可 seek 点播**；必须结合 `Icy-*` 头、`Content-Length` 是否随数据增长、连接是否自然终止来区分。若把该伪长度喂给 `getSize()` / `AVSEEK_SIZE`，会把无限直播流误判成 1 GiB 有限文件 [来源: http://stream.radioparadise.com/aac-128]（本笔记 curl 实测）。

**2.2 小结（≥3 个确切 URL）**：`https://ice2.somafm.com/groovesalad-128-mp3`（实测 ICY，禁止 CI/产品）、`https://somafm.com/groovesalad.pls`（官方 PLS，`Length=-1`）、`http://stream.radioparadise.com/aac-128`（官方供其他播放器）、`https://dir.xiph.org/yp.xml`（官方目录，体积大）、`https://de1.api.radio-browser.info/json/stations/search`（官方 API，数据 PD）、`https://hls.somafm.com/hls/groovesalad/128k/program.m3u8`（官方 HLS 演示）。

---

### 2.3 HLS (.m3u8)

#### 2.3.1 VOD vs LIVE 的规范判据（RFC 8216）

> RFC 8216 是 **Informational** RFC，「This document is not an Internet Standards Track specification」 [来源: https://www.rfc-editor.org/rfc/rfc8216]（Status of This Memo）。

- `#EXT-X-ENDLIST`：「indicates that **no more Media Segments will be added** to the Media Playlist file」；若 Playlist 含 presentation 的最后一个分片，则 **MUST contain** 该标签 [来源: https://www.rfc-editor.org/rfc/rfc8216]（4.3.3.4、6.2.1）。
- VOD 由 `#EXT-X-PLAYLIST-TYPE:VOD` 定义：「the Playlist file **MUST NOT change**」；`EVENT` 只能追加 [来源: https://www.rfc-editor.org/rfc/rfc8216]（4.3.3.5、6.2.1）。
- 没有 ENDLIST 时，服务端 **MUST** 持续发布含新分片的新版本 Playlist [来源: https://www.rfc-editor.org/rfc/rfc8216]（6.2.1）；Live Playlist **MUST NOT** 带 `EXT-X-PLAYLIST-TYPE` [来源: https://www.rfc-editor.org/rfc/rfc8216]（6.2.2）。
- 滑动窗口：服务端 `MAY` 移除分片，且 **MUST** 有 `EXT-X-MEDIA-SEQUENCE`，每移除一个分片其值 **MUST** 加 1、**MUST NOT** 减少或回绕；移除后 Playlist 时长 **MUST NOT** 小于 3× target duration [来源: https://www.rfc-editor.org/rfc/rfc8216]（6.2.2）。
- 客户端：「If the Media Playlist contains the `EXT-X-MEDIA-SEQUENCE` tag, the client **SHOULD assume that each Media Segment in it will become unavailable** at the time that the Playlist file was loaded plus the duration of the Playlist file」 [来源: https://www.rfc-editor.org/rfc/rfc8216]（6.3.2）；无 ENDLIST 正常播放时「**SHOULD NOT** choose a segment that starts less than three target durations from the end」 [来源: https://www.rfc-editor.org/rfc/rfc8216]（6.3.3）。
  - 说明：RFC 可见正文**没有**「clients MUST NOT seek/rewind beyond the window」或「MUST NOT assume segments are finite/seekable」的逐字条款，只有 Abstract 的「unbounded streams of multimedia data」与上述 SHOULD/MUST NOT（针对 Media Sequence Number 假设）[来源: https://www.rfc-editor.org/rfc/rfc8216]。
- 分片获取：Data「**SHOULD** be carried over HTTP」，但所有客户端 **MUST support HTTP schemes**；「Any Media Segment that is specified in a Playlist loaded by a client **MUST be available for immediate download**」 [来源: https://www.rfc-editor.org/rfc/rfc8216]（2、6.2.1、6.3.1）。`#EXT-X-BYTERANGE` 可在服务端支持 partial loading（HTTP Range）时把分片指定为大资源的子区间，需 compatibility version ≥ 4 [来源: https://www.rfc-editor.org/rfc/rfc8216]（4.3.2.2、6.2.1）。

#### 2.3.2 Apple 官方 bipbop / advanced stream（VOD）

- 官方 Streaming Examples 页：「**Note: This example is for testing only.**」；列出 Bip Bop stream / Advanced stream 等 [来源: https://developer.apple.com/streaming/examples/]。
- 从官方详情页源码取到的确切 master URL：`https://devstreaming-cdn.apple.com/videos/streaming/examples/bipbop_adv_example_hevc/master.m3u8` [来源: https://developer.apple.com/streaming/examples/advanced-stream-hevc.html]（本笔记 curl 抓源码所得）。
- 另一官方常用 URL：`https://devstreaming-cdn.apple.com/videos/streaming/examples/img_bipbop_adv_example_ts/master.m3u8`（实测 `200`，master playlist，`#EXT-X-VERSION:6`，多条 `#EXT-X-STREAM-INF`） [来源: https://devstreaming-cdn.apple.com/videos/streaming/examples/img_bipbop_adv_example_ts/master.m3u8]（本笔记 curl 实测）。
- 老官方文档《Deploying HTTP Live Streaming》列出的示例：`http://devimages.apple.com/iphone/samples/bipbop/bipbopall.m3u8` 与 `http://devimages.apple.com/iphone/samples/bipbop/gear3/prog_index.m3u8`，并说明 `.M3U8` 的 MIME 为 `application/x-mpegURL` / `vnd.apple.mpegURL`、`.ts` 为 `video/MP2T` [来源: https://developer.apple.com/library/ios/documentation/networkinginternet/conceptual/streamingmediaguide/deployinghttplivestreaming/deployinghttplivestreaming.html]。
- **VOD 实测**：`bipbop_adv_example_hevc/master.m3u8` 的第一条 variant `v5/prog_index.m3u8` 含 `#EXT-X-PLAYLIST-TYPE:VOD` + `#EXT-X-ENDLIST`，`#EXT-X-TARGETDURATION:8`，76 个 `#EXTINF` 分片 [来源: https://devstreaming-cdn.apple.com/videos/streaming/examples/bipbop_adv_example_hevc/master.m3u8]（本笔记 curl 实测）。

#### 2.3.3 Mux / Akamai / Unified Streaming / Bitmovin 公开测试流

- Mux：`https://test-streams.mux.dev/x36xhzz/x36xhzz.m3u8`（Big Buck Bunny）。实测 master `200`，第一条 variant `url_0/193039199_mp4_h264_aac_hd_7.m3u8` 含 `#EXT-X-PLAYLIST-TYPE:VOD` + `#EXT-X-ENDLIST`，64 个分片 ⇒ **VOD，可 seek** [来源: https://test-streams.mux.dev/x36xhzz/x36xhzz.m3u8]（本笔记 curl 实测）。
- Mux 低延迟 LIVE：`https://stream.mux.com/v69RSHhFelSm4701snP22dYz2jICy4E4FUyk02rW4gxRM.m3u8` —— hls.js 官方清单标 `live: true`，描述「Low-Latency HLS sample of Big Buck Bunny loop and a timer. **Restarts every 12 hours**」；实测 `200` [来源: https://github.com/video-dev/hls.js/blob/master/tests/test-streams.js]（本笔记 curl 实测）。
- Akamai LIVE：`https://cph-p2p-msl.akamaized.net/hls/live/2000341/test/master.m3u8`（路径含 `/hls/live/`）。实测 `200`，`#EXT-X-VERSION:3`，master playlist；解析出的 variant **无 `#EXT-X-ENDLIST`、无 `EXT-X-PLAYLIST-TYPE`** ⇒ **LIVE 滑动窗口** [来源: https://cph-p2p-msl.akamaized.net/hls/live/2000341/test/master.m3u8]（本笔记 curl 实测）。
- Unified Streaming：`https://demo.unified-streaming.com/k8s/features/stable/video/tears-of-steel/tears-of-steel.ism/.m3u8`。实测 `200`，master 含**纯音频 variant** `#EXT-X-STREAM-INF:BANDWIDTH=68000,CODECS="mp4a.40.2"` 与 `BANDWIDTH=136000`；第一 variant 含 `#EXT-X-ENDLIST`（184 分片）⇒ **VOD** [来源: https://demo.unified-streaming.com/k8s/features/stable/video/tears-of-steel/tears-of-steel.ism/.m3u8]（本笔记 curl 实测）。这条对纯音频播放器最有价值。
- Bitmovin / Bitdash：`https://bitdash-a.akamaihd.net/content/sintel/hls/playlist.m3u8` 实测 **`403`** ⇒ 已失效/受限 [来源: https://bitdash-a.akamaihd.net/content/sintel/hls/playlist.m3u8]（本笔记 curl 实测）。**稳定性反例**。
- SomaFM HLS 演示（纯音频 LIVE）：`https://hls.somafm.com/hls/groovesalad/128k/program.m3u8` 实测 `200`，`#EXT-X-MEDIA-SEQUENCE:159253`、`#EXT-X-TARGETDURATION:10`、多条 `#EXTINF:9.984`，**无 `#EXT-X-ENDLIST`** ⇒ LIVE [来源: http://hls.somafm.com/] [来源: https://hls.somafm.com/hls/groovesalad/128k/program.m3u8]（本笔记 curl 实测）。
- **第二轮复核（同日）**：该 URL 现返回 **404**（首轮 200）。⇒ 公网 LIVE 清单连「一次性人工观测」都不保证可复现，进一步支持「CI 必须本地 mock」的结论。

#### 2.3.4 hls.js / Shaka Player 官方 test 目录里列出的 demo 流（一手清单）

- **hls.js 官方 `tests/test-streams.js`**（`video-dev/hls.js`）是权威一手清单，含多条确切 URL，且明确标注 `live` / `abr` / `description` [来源: https://github.com/video-dev/hls.js/blob/master/tests/test-streams.js]：
  - `bbb`: `https://test-streams.mux.dev/x36xhzz/x36xhzz.m3u8`（Big Buck Bunny - adaptive qualities）
  - `fdr`: `https://cdn.jwplayer.com/manifests/pZxWPRg4.m3u8`
  - `mp3Audio`: `https://playertest.longtailvideo.com/adaptive/vod-with-mp3/manifest.m3u8`（**"MP3 VOD demo"**）
  - `mpegAudioOnly`: `https://pl.streamingvideoprovider.com/mp3-playlist/playlist.m3u8`（**"MPEG Audio Only demo"**）
  - `altAudioAudioOnly`: `https://playertest.longtailvideo.com/adaptive/alt-audio-no-video/sintel/playlist.m3u8`（**"Audio only with alternate audio track (Sintel)"**）
  - `altAudioMultiAudioOnly`: `https://playertest.longtailvideo.com/adaptive/alt-audio-no-video/angel-one.m3u8`（**"Audio only with multiple alternate audio tracks"**）
  - `audioOnlyMultipleLevels`: `https://s3.amazonaws.com/qa.jwplayer.com/~alex/121628/new_master.m3u8`（"Multiple non-alternate audio levels"）
  - `fmp4`: `https://storage.googleapis.com/shaka-demo-assets/angel-one-hls/hls.m3u8`（Shaka demo assets 的 HLS fMP4）
  - `AppleAdvancedHevcAvcHls`: `https://devstreaming-cdn.apple.com/videos/streaming/examples/bipbop_adv_example_hevc/master.m3u8`（Apple 官方）
  - `MuxLowLatencyHls`: `https://stream.mux.com/v69RSHhFelSm4701snP22dYz2jICy4E4FUyk02rW4gxRM.m3u8`（`live: true`）
  - `oceansAES`: `https://playertest.longtailvideo.com/adaptive/oceans_aes/oceans_aes.m3u8`（AES-128 加密，用于测试解密路径）
  - `fmp4Bitmovin`: `https://bitdash-a.akamaihd.net/content/MI201109210084_1/m3u8s-fmp4/f08e80da-bf1d-4e3d-8899-f0f6155f6efa.m3u8`（Bitmovin，本笔记**未实测**）
  - [来源: https://github.com/video-dev/hls.js/blob/master/tests/test-streams.js]
- 实测可达（均 `200`）：`mp3Audio`、`mpegAudioOnly`、`altAudioAudioOnly`（Sintel）、`altAudioMultiAudioOnly`（Angel One）、`audioOnlyMultipleLevels`、`fmp4`、`MuxLowLatencyHls` [来源: https://playertest.longtailvideo.com/adaptive/vod-with-mp3/manifest.m3u8] 等（本笔记 curl 逐条实测，见第 6 节来源索引）。
- Shaka Player 官方 demo：`https://shaka-player-demo.appspot.com/` 列出 Angel One（MPEG-DASH）、**"Dig the Uke — An audio-only presentation"**、"A self-indulgent DASH live stream" [来源: https://shaka-player-demo.appspot.com/]；Google 开发者页确认官方仓库/演示地址 [来源: https://developers.google.com/widevine/open-source/shaka-player]。**Shaka demo assets 的正式授权条款未核实**。

#### 2.3.5 FFmpeg 4.4.4 对 HLS + 自定义 AVIO 的限制（关键工程约束）

- FFmpeg 4.4 `hls.c` 源码注释：「An apple http stream consists of a playlist with media segment files … There may be **several playlists** …」「**Each playlist has its own demuxer**. If it currently is active, it has an **open AVIOContext** too」 [来源: https://ffmpeg.org/doxygen/4.4/hls_8c_source.html]（行 50–60、88–92）。
- `struct playlist` 自带 `char url[MAX_URL_SIZE]`、`AVIOContext pb`、`AVIOContext *input`、`AVIOContext *input_next`、`AVFormatContext *ctx`；清理时对每个 playlist 调 `ff_format_io_close(c->ctx, &pls->input)` 与 `avformat_close_input(&pls->ctx)` [来源: https://ffmpeg.org/doxygen/4.4/hls_8c_source.html]（行 93–105、246–272）。
- `new_playlist()` 用 `ff_make_absolute_url(pls->url, sizeof(pls->url), base, url)` 解析子 URL，「**if (!pls->url[0]) { … return NULL; }**」；`new_init_section()` 同样用 `ff_make_absolute_url(tmp_str, ..., url_base, info->uri)` 且空结果失败（`data:` URI 是特例） [来源: https://ffmpeg.org/doxygen/4.4/hls_8c_source.html]（行 296–319、400–423）。
- **推论**：HLS demuxer 需要①非空的 base URL 来解析相对 variant / segment / `EXT-X-MAP` URI，②为每个 playlist / segment 打开**多个独立资源**。单一自定义 `AVIOContext` 最多只能喂进初始 playlist 的字节流，且 `avformat_open_input(&fmt, nullptr, ...)` 的 `nullptr` 让 base URL 为空 ⇒ **该架构接不了 HLS**。
  - 说明：`hls.c` 页面在 520 行后 `[content truncated]`，实际的 `avio_open2` / `ffio_open_whitelist` / `avformat_open_input` 打开调用**未在可见片段中逐字核对**；以上结论基于可见的多 playlist/多 AVIOContext 设计与 `ff_make_absolute_url` 的空 base 失败逻辑 [来源: https://ffmpeg.org/doxygen/4.4/hls_8c_source.html]。
- 另注：FFmpeg `init_input()` 对 `AVFMT_NOFILE` 格式（如 rtsp）会打印「Custom AVIOContext makes no sense and will be ignored with AVFMT_NOFILE format.」——这是社区对源码的引用，说明「带 `AVFMT_NOFILE` 的 demuxer 与自定义 AVIO 天然冲突」[来源: https://cfanz.cn/resource/detail/ZgojDynyORzgl]（**二手，未逐字核对 FFmpeg 官方 doxygen**）。

**2.3 小结（≥3 个确切 URL，含 VOD/LIVE 区分）**：Apple `bipbop_adv_example_hevc/master.m3u8`（VOD）、Mux `x36xhzz.m3u8`（VOD）、Unified Streaming `tears-of-steel.ism/.m3u8`（VOD，含纯音频 variant）、Akamai `/hls/live/2000341/test/master.m3u8`（LIVE）、SomaFM `program.m3u8`（LIVE，纯音频）、Mux LL-HLS（LIVE）、hls.js 官方多条纯音频测试流。

---

### 2.4 DASH (.mpd)

#### 2.4.1 官方 test vectors 与确切 URL

- DASH-IF 官方一致性工具 FAQ：「**DASH-IF provides example MPDs also called as test vectors covering various test cases and features. They are hosted and accessible at `https://testassets.dashif.org/`**」 [来源: https://dashif.org/DASH-IF-Conformance/md_Conformance_Frontend_static_faq_page.html]。
- DASH-IF 官方 Tools 页：「**Find the test vectors corresponding to the latest DASH-IF guidelines: SD, HD, multichannel audio, HEVC, timed text, multiple periods, encryption … at `https://testassets.dashif.org/`**」；同页列出 Shaka Packager、GPAC/MP4Box 为参考打包工具 [来源: https://dash-industry-forum.github.io/tools/testassets] [来源: https://dash-industry-forum.github.io/tools]。
- 官方 IOP 指南给出的确切 MPD：`https://livesim2.dashif.org/livesim2/patch_60/segtimeline_1/testpic_2s/Manifest.mpd`（livesim2 测试向量），并说明可用 `https://livesim2.dashif.org/urlgen` 生成其他向量 [来源: https://dashif.org/DASH-IF-IOP/mpd-patch.pdf]。
- 实测可达的确切 MPD（均本笔记 curl 实测）：
  - `https://dash.akamaized.net/dash264/TestCases/1b/qualcomm/1/MultiRatePatched.mpd` → `200 application/dash+xml` [来源: https://dash.akamaized.net/dash264/TestCases/1b/qualcomm/1/MultiRatePatched.mpd]
  - `https://livesim.dashif.org/livesim/chunkdur_1/ato_7/testpic4_8s/Manifest.mpd` → `200 application/dash+xml`（LIVE 模拟） [来源: https://livesim.dashif.org/livesim/chunkdur_1/ato_7/testpic4_8s/Manifest.mpd]
  - `https://livesim2.dashif.org/livesim2/patch_60/segtimeline_1/testpic_2s/Manifest.mpd` → `200 application/dash+xml` [来源: https://livesim2.dashif.org/livesim2/patch_60/segtimeline_1/testpic_2s/Manifest.mpd]
  - `https://storage.googleapis.com/shaka-demo-assets/angel-one/dash.mpd` → `200 application/dash+xml`（Shaka 官方 demo asset） [来源: https://storage.googleapis.com/shaka-demo-assets/angel-one/dash.mpd]
  - `https://storage.googleapis.com/shaka-demo-assets/heliocentrism/heliocentrism.mpd` → `200` [来源: https://storage.googleapis.com/shaka-demo-assets/heliocentrism/heliocentrism.mpd]
  - `http://ftp.itec.aau.at/datasets/DASHDataset2014/BigBuckBunny/2sec/BigBuckBunny_2s_onDemand_2014_05_09.mpd` → `200 application/dash+xml`（ITEC DASH Dataset） [来源: http://ftp.itec.aau.at/datasets/DASHDataset2014/BigBuckBunny/2sec/BigBuckBunny_2s_onDemand_2014_05_09.mpd]
  - `https://storage.googleapis.com/shaka-demo-assets/angel-one-hls/hls.m3u8` → `200`（同一资产的 HLS 版本，见 2.3.4） [来源: https://storage.googleapis.com/shaka-demo-assets/angel-one-hls/hls.m3u8]
- **失效反例**：`https://bitmovin-a.akamaihd.net/content/MI201109210084_1/mpds/f08e80da-bf1d-4e3d-8899-f0f6155f6efa.mpd` → **`403`** [来源: https://bitmovin-a.akamaihd.net/content/MI201109210084_1/mpds/f08e80da-bf1d-4e3d-8899-f0f6155f6efa.mpd]（本笔记 curl 实测）。
- 官方 demo 页：`https://shaka-player-demo.appspot.com/`（Angel One / Dig the Uke audio-only / DASH live） [来源: https://shaka-player-demo.appspot.com/]。
- **各 test vector 的正式授权 / 是否允许自动化访问未核实**。

#### 2.4.2 为什么本项目大概率不该优先做 DASH

1. **解复用对象完全不同**：DASH 的入口是 XML manifest（MPD），需解析 `Period` / `AdaptationSet` / `Representation` / `BaseURL` / `SegmentTemplate` / `SegmentTimeline`，再逐个下载 ISO-BMFF / fMP4 分片；而 SoundBridge 的 12 种提取器全都是「本地容器 + 扩展名/magic sniff」模型（wav/mp3/aac/flac/ogg/aiff/ape/m4a/mkv/wma/asf），没有 MPD 解析、没有 fMP4/init segment 概念 [来源: https://dash-industry-forum.github.io/tools/testassets]。
2. **多资源打开，和 HLS 一样绕开自定义 AVIO**：MPD 会引用大量独立分片 URL，`avformat_open_input(&fmt, nullptr, ...)` 的 `nullptr` base 无法解析相对 `BaseURL`/分段 URL；单一 `AVIOContext` 只对应一个字节流 [来源: https://ffmpeg.org/doxygen/4.4/hls_8c_source.html]（HLS 同构推论，DASH demuxer 未逐行核对）。
3. **与「已知长度可随机 seek」冲突更严重**：LIVE DASH 用 `SegmentTemplate`/`SegmentTimeline` 的滑动窗口，无总长；VOD DASH 的时长来自 MPD 而非 `Content-Length` [来源: https://www.rfc-editor.org/rfc/rfc8216]（HLS 同属 ABR 清单类协议，作为类比依据；DASH 规范本身为 ISO/IEC 23009-1，**未核实**）。
4. **纯音频 DASH 供给少**：DASH-IF / Shaka 测试向量以音视频 ABR、DRM、多音轨、多 Period 为主；Shaka demo 里仅 "Dig the Uke" 是 audio-only [来源: https://shaka-player-demo.appspot.com/]；相比之下 HLS 侧 hls.js 官方清单有多条 audio-only 测试流 [来源: https://github.com/video-dev/hls.js/blob/master/tests/test-streams.js]。
5. **构建未确认**：背景里 FFmpeg 协议清单只列了 HTTP/HTTPS/TLS/HLS/RTMP/TCP/UDP，**是否启用 `dash` demuxer 未核实**；且没编 ffprobe/ffmpeg CLI，无法用命令行诊断 MPD。**投入产出比最低**。

---

### 2.5 协议与工程约束的一手依据（FFmpeg 4.4 doxygen / RFC）

#### 2.5.1 FFmpeg 4.4 官方文档（版本化页面）

- `AVFMT_FLAG_CUSTOM_IO`：`#define AVFMT_FLAG_CUSTOM_IO 0x0080` —「**The caller has supplied a custom AVIOContext, don't avio_close() it.**」 [来源: https://ffmpeg.org/doxygen/4.4/avformat_8h.html]
- `avformat_alloc_context` + 自定义 I/O 的官方说明：「… when you want to use custom functions for reading input data instead of lavf internal I/O layer. To do that, **create your own AVIOContext with avio_alloc_context()**, passing your reading callbacks to it. Then set the **pb** field of your AVFormatContext …」 [来源: https://ffmpeg.org/doxygen/4.4/avformat_8h_source.html]（行 119–125）。
- `avio_alloc_context()` 参数语义（逐字）：
  - `read_packet`:「A function for refilling the buffer, may be NULL. **For stream protocols, must never return 0 but rather a proper AVERROR code.**」 [来源: https://ffmpeg.org/doxygen/4.4/avio_8h.html]
  - `seek`:「A function for seeking to specified byte position, **may be NULL**」 [来源: https://ffmpeg.org/doxygen/4.4/avio_8h.html]
  - `buffer`:「must be allocated with `av_malloc()` … **may be freed and replaced with a new buffer by libavformat** … `AVIOContext.buffer` … **must be later freed with `av_free()`**」；AVIOContext 本身「**must be later freed with `avio_context_free()`**」 [来源: https://ffmpeg.org/doxygen/4.4/avio_8h.html]
- `AVSEEK_SIZE`：`#define AVSEEK_SIZE 0x10000` —「**ORing this as the "whence" parameter to a seek function causes it to return the filesize without seeking anywhere. Supporting this is optional. If it is not supported then the seek function will return <0.**」 [来源: https://ffmpeg.org/doxygen/4.4/avio_8h.html] [来源: https://ffmpeg.org/doxygen/4.4/avio_8h_source.html]（行 526–531）
- `AVSEEK_FORCE`：`#define AVSEEK_FORCE 0x20000` —「seek by any means (like **reopening and linear reading**) or other normally unreasonable means that can be **extremely slow**. **This may be ignored by the seek code.**」 [来源: https://ffmpeg.org/doxygen/4.4/avio_8h_source.html]（行 533–539）
- `AVIOContext.seekable` 定义：
  - `#define AVIO_SEEKABLE_NORMAL (1 << 0)` —「**Seeking works like for a local file.**」
  - `#define AVIO_SEEKABLE_TIME (1 << 1)` —「Seeking by timestamp with `avio_seek_time()` is possible.」
  - 字段：`/** A combination of AVIO_SEEKABLE_ flags or **0 when the stream is not seekable**. */ int seekable;` [来源: https://ffmpeg.org/doxygen/4.4/avio_8h_source.html]（行 37–45、257–260）
- `avio_size()`：「Get the filesize.」；`avio_seek()`：「fseek() equivalent for AVIOContext」 [来源: https://ffmpeg.org/doxygen/4.4/avio_8h.html]

#### 2.5.2 HTTP Range（RFC 7233）

- `Range` 是 **OPTIONAL**：「Range requests are an **OPTIONAL** feature of HTTP, designed so that recipients not implementing this feature … can respond as if it is a normal GET request」；「A server **MAY ignore** the Range header field」 [来源: https://www.rfc-editor.org/rfc/rfc7233]（1、3.1、4.4）。
- `206 Partial Content`：单段响应 **MUST** 带 `Content-Range`；多段用 `multipart/byteranges` 且顶层 **MUST NOT** 带 `Content-Range` [来源: https://www.rfc-editor.org/rfc/rfc7233]（4.1）。
- `Accept-Ranges`：支持时 `MAY send Accept-Ranges: bytes`，不支持时 `MAY send Accept-Ranges: none`；客户端可在未收到该头时仍发 Range 请求 [来源: https://www.rfc-editor.org/rfc/rfc7233]（2.3）。
- `Content-Range`：仅在 `206` / `416` 有语义；`complete-length` 未知时用 `*`，416 用 `bytes */1234` 表示当前长度 [来源: https://www.rfc-editor.org/rfc/rfc7233]（4.2）。
- **注**：RFC 7233 页面标注「**This RFC is now obsolete, see RFC 9110**」；现行合规判断应核对 RFC 9110 [来源: https://www.rfc-editor.org/rfc/rfc7233]。

#### 2.5.3 Icecast `Icy-MetaData` / 无限流

- 见 2.2.1：`Icy-MetaData: 1` → `icy-metaint`，元数据按字节间隔内嵌；官方无单一规范页，权威描述为社区协议抄本 + Icecast 官方 YP 文档 + 本笔记对 SomaFM 的实测响应头 [来源: https://gist.github.com/ericchianti/cd2af56379c4d581e7e666ced556b90f/95d6388049779e52aaf96e3176a8b5830b9f618b] [来源: https://www.icecast.org/docs/icecast-trunk/yp/] [来源: https://ice2.somafm.com/groovesalad-128-mp3]。
- 官方 PLS 用 `Length=-1` 表达无限长度 [来源: https://somafm.com/groovesalad.pls]。

#### 2.5.4 HLS RFC 8216 关键约束

见 2.3.1（`ENDLIST` / `PLAYLIST-TYPE` / 滑动窗口 / `EXT-X-MEDIA-SEQUENCE` / `EXT-X-BYTERANGE` / Informational 性质）[来源: https://www.rfc-editor.org/rfc/rfc8216]。

#### 2.5.5 用一手依据论证「自定义 AVIO + 已知长度可随机 seek」接直播流为什么不行

| # | 架构假设 | 一手依据 | 直播流现实 | 冲突 |
|---|---|---|---|---|
| 1 | `DataSource::readAt(offset, buf, size)` 任意 offset 可读 | [来源: https://ffmpeg.org/doxygen/4.4/avio_8h_source.html] `AVIO_SEEKABLE_NORMAL`「Seeking works like for a local file」 | ICY 无 `Accept-Ranges`、只 `200 OK` 顺序推流；HLS LIVE 分片随窗口过期 [来源: https://ice2.somafm.com/groovesalad-128-mp3] [来源: https://www.rfc-editor.org/rfc/rfc8216] | 任意 offset 读不到 |
| 2 | `getSize` 全量扫描得到确定长度 | [来源: https://ffmpeg.org/doxygen/4.4/avio_8h.html] `AVSEEK_SIZE`「return the filesize」/ `avio_size()` | 无 `Content-Length`、连接无限；`curl --max-time 5` 仍在收数据 [来源: https://ice2.somafm.com/groovesalad-128-mp3] | 全量扫描永不终止；`AVSEEK_SIZE` 只能返回 `<0`（「If it is not supported then the seek function will return <0」）[来源: https://ffmpeg.org/doxygen/4.4/avio_8h.html] |
| 3 | `read_packet` 返回 0 表示 EOF | [来源: https://ffmpeg.org/doxygen/4.4/avio_8h.html]「For stream protocols, **must never return 0** but rather a proper AVERROR code」 | 直播流没有 EOF，只有断线/超时 | 若按本地文件语义返回 0，FFmpeg 会提前结束 |
| 4 | `AVIOContext.seekable` 可置 `AVIO_SEEKABLE_NORMAL` | [来源: https://ffmpeg.org/doxygen/4.4/avio_8h_source.html]「0 when the stream is not seekable」 | 不可 seek | 置位即撒谎；不置位则依赖 seek 的提取器（MP3 Xing/VBRI、FLAC、MP4/M4A、Ogg granule）退化或失败 |
| 5 | `AVFMT_FLAG_CUSTOM_IO` 只接管一个 `pb` | [来源: https://ffmpeg.org/doxygen/4.4/avformat_8h.html]「The caller has supplied a custom AVIOContext」 | HLS 每个 playlist 有自己的 demuxer / `AVIOContext`，并 `ff_make_absolute_url` 解析子 URL [来源: https://ffmpeg.org/doxygen/4.4/hls_8c_source.html] | `avformat_open_input(&fmt, nullptr, ...)` 的 `nullptr` base 为空，`if (!pls->url[0]) return NULL` 直接失败 [来源: https://ffmpeg.org/doxygen/4.4/hls_8c_source.html] |
| 6 | `m_abortDecode` 是唯一取消信号 | 项目背景 | 网络读阻塞在 `read_packet` 内；若不在回调里检查该标志，取消无法生效；`AVSEEK_FORCE` 的「reopening and linear reading」还会放大阻塞 [来源: https://ffmpeg.org/doxygen/4.4/avio_8h_source.html] | 需要引入 `interrupt_callback` / 超时 / 断线重连状态机，超出「唯一取消信号」 |
| 7 | `durationMs==0` 已允许、seek 只留 `seekToMs` | 项目背景 | LIVE 既无时长也不可 seek | 元数据层能表达未知时长，但 UI/seek 路径需要能禁用 seek；`seekToMs` 单一入口无「不可 seek」状态 |
| 8 | 扩展名 + magic sniff 选提取器 | 项目背景 | URL 常无扩展名/带 query；`.m3u8` / `.mpd` 是文本清单不是音频容器；ICY 响应只有 `audio/mpeg` | `ExtractorFactory` 无法识别清单类数据源 |
| 9 | 没编 ffmpeg/ffprobe CLI | 项目背景 | 出问题无法命令行 `ffprobe -i URL` 诊断 | 只能靠 libavformat 日志与自造探针 |

---

## 3. 合规与风险（单列）

### 3.1 版权：四类要分清，哪些**不能**进 CI / 产品默认列表

| 类别 | 代表来源 | 能否进自动化 CI | 能否进产品默认列表 | 依据 |
|---|---|---|---|---|
| 公共领域（PD） | LibriVox / Internet Archive PD 条目、Wikimedia PD 文件 | **慎用**（archive.org ToS 限 research + 不得干扰服务器；LibriVox PD 仅限美国） | 慎用（需按来源 ToS、逐文件核实） | [来源: https://archive.org/about/terms] [来源: https://librivox.org/?p=31376/] [来源: https://commons.wikimedia.org/wiki/Commons:First_steps/Reuse] |
| CC 授权 | Wikimedia Commons（多为 CC BY / CC BY-SA，需署名 + 相同方式共享） | 不建议（署名义务 + hotlinking 官方不推荐） | 不建议 | [来源: https://commons.wikimedia.org/wiki/Commons:First_steps/Reuse] [来源: https://commons.wikimedia.org/wiki/Commons:Reusing_content_outside_Wikimedia] |
| 明确供测试的 demo 流/文件 | Apple「for testing only」、Mux / hls.js / Shaka / DASH-IF test vectors、samplelib「no license restrictions」、file-examples「test your app」 | **可以**（限开发测试用途） | **一般不可**（"for testing only" ≠ 产品分发授权；正式 ToS 多未核实） | [来源: https://developer.apple.com/streaming/examples/] [来源: https://github.com/video-dev/hls.js/blob/master/tests/test-streams.js] [来源: https://download.samplelib.com/sample-mp3.html] [来源: https://file-examples-download.com/?page_id=9] [来源: https://dashif.org/DASH-IF-Conformance/md_Conformance_Frontend_static_faq_page.html] |
| 仅供个人收听的广播流 | **SomaFM**、Radio Paradise 等音乐电台 | **禁止**（ToS 禁止第三方客户端/自动化/录制） | **禁止**（ToS 明确拒绝新第三方客户端） | [来源: https://somafm.com/legal/tos.html] |

- **SomaFM 是最明确的红线**：2026-09-04 ToS「We can't grant permission for third-party SomaFM clients or applications, even noncommercial ones」「not approving new third-party clients」「intended for individual/personal use by end users only」，禁止 embedding / restreaming / ripping / scrobbling，并提示运营方可能承担 BMI/SESAC/SoundExchange 年费 [来源: https://somafm.com/legal/tos.html]。
- **Radio Browser 的 PD 只覆盖元数据**（站名/tag/链接），不覆盖流内容版权；「I give all the rights I have at the accumulated data to the public domain」 [来源: https://www.radio-browser.info/]。把 `url_resolved` 直接内置进产品并不因此获得音乐授权。
- **「明确供测试」≠ 可商用**：Apple、hls.js/DASH-IF 测试向量都面向开发验证；Akamai / Mux / Unified Streaming / Shaka demo assets 的正式 ToS **未核实**。

### 3.2 稳定性：第三方公网服务不可作为 CI 门禁依赖

本轮实测到的失效/受限（均可作为「为什么必须本地 mock」的证据）：

- `https://ice.somafm.com/groovesalad` / `.../dronezone` → `404`（旧挂载点失效）[来源: https://www.soma.fm/listen/sonoscustom.html]（官方页仍列旧 URL，实测已 404）
- `https://bitdash-a.akamaihd.net/content/sintel/hls/playlist.m3u8` → `403` [来源: https://bitdash-a.akamaihd.net/content/sintel/hls/playlist.m3u8]
- `https://bitmovin-a.akamaihd.net/content/MI201109210084_1/mpds/f08e80da-...mpd` → `403` [来源: https://bitmovin-a.akamaihd.net/content/MI201109210084_1/mpds/f08e80da-bf1d-4e3d-8899-f0f6155f6efa.mpd]
- `https://file-examples.com/index.php/sample-audio-files/` → `403`（UA 相关）[来源: http://file-examples.com/index.php/sample-audio-files/]
- MDN 引用的 `http://jPlayer.org/audio/mp3/Miaow-01-Tempered-song.mp3` → `200 text/html`（内容已非音频）[来源: https://developer.mozilla.org/en-US/docs/Web/Media/Audio_and_video_delivery]
- archive.org 直链 → `302` 到会变化的存储节点 [来源: https://archive.org/download/island_moreau_librivox/island_dr_moreau_00_wells.mp3]
- `https://dir.xiph.org/yp.xml` 单次数 MB，且大量 `listen_url` 为空 [来源: https://dir.xiph.org/yp.xml]
- Radio Browser 官方自述「used freely **but without guarantee to work** … downtimes are possible」 [来源: http://docs.radio-browser.info/]
- SomaFM 官方称 2026-10-20 停掉 HLS-lossless 是因为「abused by bots and streamrippers」 [来源: http://hls.somafm.com/]
- `https://hls.somafm.com/hls/groovesalad/128k/program.m3u8` 首轮 200 → **同日第二轮复核 404**（本笔记 curl 实测）

**分层测试策略建议**（结合项目已有约定）：

1. **L0 单元/门禁（必须完全离线）**：本地 mock HTTP server + 固定素材。素材建议来自 samplelib（`sample-15s.mp3`、CBR/VBR/ID3 变体、8/16/48/96 kHz WAV）[来源: https://download.samplelib.com/sample-mp3.html] 与 file-examples-download（MP3/WAV/FLAC/OGG/AAC 小文件）[来源: https://file-examples-download.com/?page_id=9]；本地 server 固定实现 `200 + Content-Length + Accept-Ranges: bytes` 与 `206 + Content-Range`，并按 `Range` 返回 `readAt(offset, size)` 对应字节。
2. **L1 协议负例（离线）**：mock server 可切到「无 Content-Length / 无 Accept-Ranges / 忽略 Range 返回 200 / 无限 chunked」，验证 `AVSEEK_SIZE < 0`、`seekable=0`、`durationMs==0`、`read_packet` 不返回 0、取消能中断阻塞读。
3. **L2 手工冒烟（可用公网，不进门禁）**：公网点播直链、Apple/Mux VOD、hls.js 纯音频测试流。
4. **L3 只做人工观测**：ICY / HLS LIVE 滑动窗口，仅用于理解协议特征，绝不写进 `-j4` 回归。

> 引用项目约定：`ctest` 可并行（`-j4`）的前提是「**每个测试进程只写自己的文件**」；「新增用例若往固定名字的共享路径写东西，会把这个稳定性还回去」 [来源: d:/workspace/SoundBridge/AGENTS.md:111-117]。因此 L0 的 mock server 端口、临时目录、日志文件都必须带进程号（`sb_test::processId()` 的既有做法），绝不写共享固定路径或固定端口。

### 3.3 地域限制 / TLS / 重定向 / User-Agent / robots 与 ToS

- **重定向**：archive.org `/download/...` → `dn711304.ca.archive.org`（具体节点，随迁移变化）[来源: https://archive.org/download/island_moreau_librivox/island_dr_moreau_00_wells.mp3]；Radio Browser 明确要求「Never use a direct link to a single new server. It is much better to get a list of the servers …」，通过 DNS `all.api.radio-browser.info` 发现并随机重试 [来源: https://api.radio-browser.info/]。
- **User-Agent**：Radio Browser 要求「Send a speaking http agent string (e.g. mycoolapp/1.4)」[来源: https://api.radio-browser.info/]；`docs.radio-browser.info` 要求描述性 UA 如 `Cool Radio App/1.2` [来源: http://docs.radio-browser.info/]；Wikimedia Commons API 亦需 UA（本笔记以 `SoundBridge-Research/1.0` 调用成功）[来源: https://commons.wikimedia.org/w/api.php]；`file-examples.com` 对自定义 UA 返回 `403` [来源: http://file-examples.com/index.php/sample-audio-files/]。
- **TLS**：项目用 Schannel；实测 SomaFM 走 `https://ice2.somafm.com/...`，Icecast 内部响应仍是 `HTTP/1.0 200 OK` [来源: https://ice2.somafm.com/groovesalad-128-mp3]；MDN 老 jPlayer 素材是明文 `http://` [来源: https://developer.mozilla.org/en-US/docs/Web/Media/Audio_and_video_delivery]。混合内容 / 证书链 / SNI 需实测。
- **地域限制**：多个第三方资料提示公共测试流可能有地域限制或过期，本轮 SomaFM 旧直链 `404` 的确切原因（地域 / UA / 挂载迁移）**未核实** [来源: https://playm3u8.org/en/help/live-source-test]（二手提示，不作定论）。
- **robots / ToS**：archive.org ToS 限「scholarship and research purposes only」且不得干扰服务器 [来源: https://archive.org/about/terms]；Wikimedia 官方「hotlinking … is not recommended」 [来源: https://commons.wikimedia.org/wiki/Commons:Reusing_content_outside_Wikimedia]；SomaFM 禁止第三方应用/自动化 [来源: https://somafm.com/legal/tos.html]。各站 `robots.txt` 内容**未核实**。

### 3.4 是否需要在产品里内置公开电台列表（法律风险）

**建议：不内置。** 理由：

1. SomaFM ToS 逐字拒绝第三方客户端：「We can't grant permission for third-party SomaFM clients or applications, even noncommercial ones」「we're not approving new third-party clients」，并说明音乐授权（BMI / SESAC / SoundExchange）可能给运营方带来每年数千美元最低费用 [来源: https://somafm.com/legal/tos.html]。
2. Radio Browser 的 PD 只含元数据，不含流内容版权 [来源: https://www.radio-browser.info/]；`dir.xiph.org` 目录里的电台版权状态各异且官方声明「I'm not in any way responsible for the entries … Nor am I responsible for the radio stations」[来源: https://www.radio-browser.info/]（同类社区目录免责口径；Xiph 目录的对应条款未核实）。
3. 即便只存 URL 不存音频，「预置电台列表」也可能被解释为促成第三方客户端分发/嵌入，与上述 ToS 冲突。
4. 更稳妥的产品形态：**只提供「用户手动输入 URL / 打开本地 `.pls`/`.m3u8`」**，由用户对所输 URL 的授权负责；测试用的 demo 流只放在开发者文档与本地 mock 里，不随产品分发。

---

## 4. 推荐分层清单表格

> 「已知长度」「可 seek」列针对该 URL 的**媒体数据源本身**（HLS VOD 的 Playlist 固定、分片有限 ⇒ 记为「是（有限窗口）」；LIVE 滑动窗口 ⇒ 「否」）。

| 层级 | URL | 协议 | 格式 | 已知长度 | 可 seek | 授权 | 用途 | 稳定性备注 |
|---|---|---|---|---|---|---|---|---|
| 本地 mock | `http://127.0.0.1:<pid-port>/sample-15s.mp3`（素材源 [来源: https://samplelib.com/mp3/sample-15s.mp3]） | HTTP/1.1 + Range/206 | MP3 | 是（307453 B） | 是 | 「no license restrictions」[来源: https://download.samplelib.com/sample-mp3.html] | **CI 门禁 / 单元回归** | 固定素材、进程私有端口；遵循 [来源: d:/workspace/SoundBridge/AGENTS.md:111-117] |
| 本地 mock | `http://127.0.0.1:<pid-port>/sample.flac`（素材源 [来源: https://file-examples-download.com/wp-content/uploads/filr/63/sample.flac]） | HTTP + Range | FLAC | 是（68443 B） | 是 | 站点声明「test your app」[来源: https://file-examples-download.com/?page_id=9]，正式条款未核实 | 格式覆盖 / Range 负例 | 实测 206；主站 `file-examples.com` 403 [来源: http://file-examples.com/index.php/sample-audio-files/] |
| 公网点播 | `https://samplelib.com/mp3/sample-15s.mp3` | HTTPS + `Accept-Ranges: bytes` | MP3 | 是（307453 B） | 是 | 无许可限制（页面措辞）[来源: https://download.samplelib.com/sample-mp3.html] | 手工冒烟（不进门禁） | 实测 200/206；未见独立 ToS [来源: https://samplelib.com/] |
| 公网点播 | `https://file-examples-download.com/wp-content/uploads/filr/67/sample.ogg` | HTTPS + Range | OGG Vorbis | 是（98185 B） | 是 | 供测试，条款未核实 [来源: https://file-examples-download.com/?page_id=9] | 格式覆盖 | 实测 200 `audio/ogg` |
| 公网点播 | `https://archive.org/download/island_moreau_librivox/island_dr_moreau_00_wells.mp3` | HTTPS 302 → 206 | MP3 | 是（2828494 B） | 是 | PD（USA）[来源: https://librivox.org/?p=31376/] | 长文件 / 重定向链路 | ToS 限 research [来源: https://archive.org/about/terms]；302 节点会变 |
| 公网点播 | `https://upload.wikimedia.org/wikipedia/commons/5/53/Rossini_-_Il_Barbiere_di_Siviglia_-_La_calunnia_-_Francesco_Navarini_%281903%29_version_30secondes.ogg` | HTTPS + Range | OGG Vorbis | 是（1196609 B） | 是 | 逐文件 CC/PD，需署名 [来源: https://commons.wikimedia.org/wiki/Commons:First_steps/Reuse] | 格式覆盖 | 官方不推荐 hotlinking [来源: https://commons.wikimedia.org/wiki/Commons:Reusing_content_outside_Wikimedia] |
| 公网点播 | `https://github.com/mdn/learning-area/raw/refs/heads/main/html/multimedia-and-embedding/tasks/audio-video/audio.mp3` | HTTPS | MP3 | 是 | 是 | 仓库素材许可证**未核实** | 手工冒烟 | 实测 200 `audio/mpeg` [来源: https://developer.mozilla.org/en-US/docs/Learn_web_development/Core/Structuring_content/Test_your_skills/Audio_and_video] |
| 公网直播（ICY） | `https://ice2.somafm.com/groovesalad-128-mp3` | ICY over HTTPS（`icy-metaint:45000`） | MP3 | **否**（无 Content-Length，无限流） | **否** | **仅个人收听；禁第三方应用/CI** [来源: https://somafm.com/legal/tos.html] | **仅一次性人工协议观测** | 实测 Icecast 200 无限流；旧直链 404 [来源: https://ice2.somafm.com/groovesalad-128-mp3] [来源: https://www.soma.fm/listen/sonoscustom.html] |
| 公网直播（ICY） | `http://stream.radioparadise.com/aac-128` | ICY/HTTP | AAC | **否** | **否** | 官方供「other players」，但为受版权保护的音乐广播 [来源: https://radioparadise.com/listen/stream-links] | 仅手工 | 第三方应用/自动化条款**未核实** |
| 目录 / API | `https://de1.api.radio-browser.info/json/stations/search` | HTTPS JSON | 元数据 | 不适用 | 不适用 | 数据 PD（仅元数据）[来源: https://www.radio-browser.info/] | 查询候选（不内置） | 官方「without guarantee to work」；需描述性 UA、勿硬编码单服务器 [来源: https://api.radio-browser.info/] |
| 目录 / API | `https://dir.xiph.org/yp.xml` | HTTPS XML | 元数据 | 不适用 | 不适用 | **未核实** | 人工浏览 | 数 MB、大量空 `listen_url` [来源: https://dir.xiph.org/yp.xml] |
| HLS VOD | `https://devstreaming-cdn.apple.com/videos/streaming/examples/bipbop_adv_example_hevc/master.m3u8` | HLS（fMP4/AAC/AC-3） | 音视频 | 是（`PLAYLIST-TYPE:VOD` + `ENDLIST`，76 分片） | 是（有限窗口） | Apple「for testing only」[来源: https://developer.apple.com/streaming/examples/] | 手工 HLS VOD 验证 | 实测 200；variant 含 VOD/ENDLIST [来源: https://devstreaming-cdn.apple.com/videos/streaming/examples/bipbop_adv_example_hevc/master.m3u8] |
| HLS VOD | `https://test-streams.mux.dev/x36xhzz/x36xhzz.m3u8` | HLS（TS/AAC） | 音视频 | 是（64 分片） | 是 | Mux 公开测试流，正式 ToS **未核实** | 手工 HLS VOD 验证 | 实测 `PLAYLIST-TYPE:VOD`+`ENDLIST` [来源: https://test-streams.mux.dev/x36xhzz/x36xhzz.m3u8] |
| HLS VOD（纯音频优先） | `https://demo.unified-streaming.com/k8s/features/stable/video/tears-of-steel/tears-of-steel.ism/.m3u8` | HLS | 含纯音频 variant `mp4a.40.2` | 是（`ENDLIST`，184 分片） | 是 | 公开 demo，正式 ToS **未核实** | 纯音频 HLS 验证 | 实测 200 [来源: https://demo.unified-streaming.com/k8s/features/stable/video/tears-of-steel/tears-of-steel.ism/.m3u8] |
| HLS VOD（纯音频） | `https://playertest.longtailvideo.com/adaptive/vod-with-mp3/manifest.m3u8` | HLS | MP3 | 是 | 是 | hls.js 官方测试流 [来源: https://github.com/video-dev/hls.js/blob/master/tests/test-streams.js] | 纯音频 HLS 验证 | 实测 200 [来源: https://playertest.longtailvideo.com/adaptive/vod-with-mp3/manifest.m3u8] |
| HLS VOD（纯音频） | `https://pl.streamingvideoprovider.com/mp3-playlist/playlist.m3u8` | HLS | MPEG Audio | 是 | 是 | hls.js 官方测试流（"MPEG Audio Only demo"）[来源: https://github.com/video-dev/hls.js/blob/master/tests/test-streams.js] | 纯音频 HLS 验证 | 实测 200 |
| HLS LIVE | `https://hls.somafm.com/hls/groovesalad/128k/program.m3u8` | HLS（纯音频 AAC） | 音频 | **否**（滑动窗口） | **否** | **仅个人收听；禁第三方应用/CI** [来源: https://somafm.com/legal/tos.html] | 仅人工协议观测 | 首轮 `MEDIA-SEQUENCE:159253` 无 ENDLIST、**同日二轮复核 404** [来源: http://hls.somafm.com/] |
| HLS LIVE | `https://stream.mux.com/v69RSHhFelSm4701snP22dYz2jICy4E4FUyk02rW4gxRM.m3u8` | LL-HLS（fMP4） | 音视频 | **否** | **否** | Mux 测试流，正式 ToS **未核实** | 仅手工 | hls.js 标 `live:true`，「Restarts every 12 hours」[来源: https://github.com/video-dev/hls.js/blob/master/tests/test-streams.js] |
| HLS LIVE | `https://cph-p2p-msl.akamaized.net/hls/live/2000341/test/master.m3u8` | HLS | 音视频 | **否** | **否** | Akamai 公开测试，ToS **未核实** | 仅手工 | 实测无 ENDLIST/PLAYLIST-TYPE [来源: https://cph-p2p-msl.akamaized.net/hls/live/2000341/test/master.m3u8] |
| DASH（不建议） | `https://dash.akamaized.net/dash264/TestCases/1b/qualcomm/1/MultiRatePatched.mpd` | DASH/MPD | ABR 音视频 | 依 MPD（非 Content-Length） | 依 MPD | DASH-IF test vector，授权 **未核实** [来源: https://dashif.org/DASH-IF-Conformance/md_Conformance_Frontend_static_faq_page.html] | 仅记录，P3 | 实测 200 `application/dash+xml` |
| DASH（不建议） | `https://livesim2.dashif.org/livesim2/patch_60/segtimeline_1/testpic_2s/Manifest.mpd` | DASH LIVE 模拟 | ABR | **否** | **否** | 官方 test vector [来源: https://dashif.org/DASH-IF-IOP/mpd-patch.pdf] | 仅记录 | 实测 200 |
| DASH（不建议） | `https://storage.googleapis.com/shaka-demo-assets/angel-one/dash.mpd` | DASH VOD | ABR | 依 MPD | 依 MPD | Shaka demo asset，授权 **未核实** [来源: https://shaka-player-demo.appspot.com/] | 仅记录 | 实测 200 |

---

## 5. 对 SoundBridge 的接入优先级建议

1. **P0 — 先只做「有 `Content-Length` + 支持 `Range/206` 的静态 HTTP 点播」，且用本地 mock 落地**。把 `Range: bytes=<offset>-<offset+size-1>` 映射成 `DataSource::readAt(offset, buf, size)`；`getSize` 改从 `HEAD` / 首个 `206` 的 `Content-Range: bytes …/<total>` 取完整长度，**不要沿用全量扫描**（否则每个 URL 都要先下载一遍）[来源: https://www.rfc-editor.org/rfc/rfc7233] [来源: https://ffmpeg.org/doxygen/4.4/avio_8h.html]。固定素材用 samplelib（含 CBR/VBR/ID3 变体、多采样率 WAV）与 file-examples-download 的 MP3/WAV/FLAC/OGG/AAC 小文件 [来源: https://download.samplelib.com/sample-mp3.html] [来源: https://file-examples-download.com/?page_id=9]。这一步与现有「已知长度 + 可随机 seek」契约完全兼容，风险最低。

2. **P1 — 在 `DataSource` 契约里把「已知长度 / 可 seek」变成显式可选能力，并补齐取消与超时**。对应 `AVSEEK_SIZE` 返回 `<0`（官方：「If it is not supported then the seek function will return <0」）与 `AVIOContext.seekable = 0`（官方：「0 when the stream is not seekable」）[来源: https://ffmpeg.org/doxygen/4.4/avio_8h.html] [来源: https://ffmpeg.org/doxygen/4.4/avio_8h_source.html]。`m_abortDecode` 必须贯穿到 `read_packet` 的阻塞点（配合 `AVFormatContext.interrupt_callback`），否则网络挂住时唯一取消信号失效；`read_packet` 对流式数据「must never return 0 but rather a proper AVERROR code」 [来源: https://ffmpeg.org/doxygen/4.4/avio_8h.html]。`durationMs==0` 已允许可继续复用，但 `seekToMs` 需要能表达「不可 seek」，UI 要禁用进度条拖动 [来源: https://www.rfc-editor.org/rfc/rfc8216]。

3. **P1 — 明确暂不接 ICY / HLS LIVE，把「断线重连」留到有独立状态机之后**。理由：ICY 无 Content-Length、无 Range、无限流（实测 `icy-metaint:45000` + 5 秒仍在收数据）[来源: https://ice2.somafm.com/groovesalad-128-mp3]；HLS LIVE 分片随滑动窗口过期（RFC 8216 6.3.2「SHOULD assume … will become unavailable」）[来源: https://www.rfc-editor.org/rfc/rfc8216]。现有提取器按扩展名 + magic sniff 选择，URL 无扩展名 / `.m3u8` 与 `.mpd` 是文本清单，都会落到 sniff 失败 [来源: https://ffmpeg.org/doxygen/4.4/avformat_8h.html]。`AVSEEK_FORCE` 的「reopening and linear reading … extremely slow」更说明用 seek 模拟流式读取不可取 [来源: https://ffmpeg.org/doxygen/4.4/avio_8h_source.html]。

4. **P2 — 若将来做 HLS，必须绕开自定义 AVIO，让 FFmpeg 原生 URL/协议层接管**。FFmpeg 4.4 `hls.c` 里每个 playlist 有自己的 demuxer / `AVIOContext`，并用 `ff_make_absolute_url(pls->url, ..., base, url)` 解析子 URL，空 `base` 直接 `return NULL`；`avformat_open_input(&fmt, nullptr, ...)` 的 `nullptr` 无法满足 [来源: https://ffmpeg.org/doxygen/4.4/hls_8c_source.html]。可行方向是：对清单类 URL 走 `avformat_open_input(&fmt, url, ...)` + protocol 白名单（`protocol_whitelist`）/ `AVFormatContext.interrupt_callback`，而不是继续塞 `pb`；并且先只做 VOD（有 `#EXT-X-ENDLIST` / `PLAYLIST-TYPE:VOD`，如 Apple bipbop、Mux x36xhzz、hls.js 的 `mp3Audio`）[来源: https://www.rfc-editor.org/rfc/rfc8216] [来源: https://github.com/video-dev/hls.js/blob/master/tests/test-streams.js]。注意项目**没编 ffprobe/ffmpeg CLI**，诊断只能靠 libavformat 日志与自造探针。

5. **P3 — DASH 暂不做；产品不内置公开电台列表**。DASH 的 MPD + fMP4/ABR 与 12 种本地格式提取器架构无重叠，纯音频测试流也少，且项目构建是否启用 `dash` demuxer **未核实** [来源: https://dash-industry-forum.github.io/tools/testassets] [来源: https://shaka-player-demo.appspot.com/]。产品层面只提供「用户手动输入 URL / 打开本地播放列表」，不预置 SomaFM / Radio Paradise 等电台 —— SomaFM ToS 逐字拒绝新第三方客户端并限定个人收听，Radio Browser 的 PD 只覆盖元数据 [来源: https://somafm.com/legal/tos.html] [来源: https://www.radio-browser.info/]。

---

## 6. 一手来源索引（按类别）

**FFmpeg 4.4 官方（版本化 doxygen / 源码 / 文档）**
- https://ffmpeg.org/doxygen/4.4/avio_8h.html
- https://ffmpeg.org/doxygen/4.4/avio_8h_source.html
- https://ffmpeg.org/doxygen/4.4/avformat_8h.html
- https://ffmpeg.org/doxygen/4.4/avformat_8h_source.html
- https://ffmpeg.org/doxygen/4.4/hls_8c_source.html
- https://ffmpeg.org/fate.html
- https://git.ffmpeg.org/gitweb/ffmpeg.git/blob/26148e923613e718787c6fc4bf3f64e8909f597c:/doc/fate.texi
- https://ffmpeg.org/pipermail/ffmpeg-devel/2025-April/342876.html（`rsync://fate-suite.ffmpeg.org/fate-suite/`）

**规范**
- https://www.rfc-editor.org/rfc/rfc7233（HTTP Range；已 obsolete by RFC 9110）
- https://www.rfc-editor.org/rfc/rfc8216（HLS，Informational）

**Icecast / 目录 / API**
- https://www.icecast.org/docs/icecast-trunk/yp/
- https://icecast.org/docs/icecast-2.0.2/yp.html
- https://lists.xiph.org/pipermail/icecast-dev/2013-February/002123.html
- https://dir.xiph.org/ 、https://dir.xiph.org/yp.xml
- https://www.radio-browser.info/ 、https://api.radio-browser.info/ 、http://docs.radio-browser.info/ 、https://de1.api.radio-browser.info/json/stations/search
- https://www.soma.fm/listen/sonoscustom.html 、https://somafm.com/groovesalad.pls 、https://ice2.somafm.com/groovesalad-128-mp3 、http://hls.somafm.com/ 、https://somafm.com/legal/tos.html
- https://radioparadise.com/listen/stream-links 、https://www.wikidata.org/wiki/Q3416792
- https://gist.github.com/ericchianti/cd2af56379c4d581e7e666ced556b90f/95d6388049779e52aaf96e3176a8b5830b9f618b（ICY 协议抄本，非官方规范）

**点播素材 / 授权**
- https://samplelib.com/ 、https://download.samplelib.com/sample-mp3.html 、https://download.samplelib.com/sample-wav.html 、https://samplelib.com/mp3/sample-15s.mp3
- https://file-examples-download.com/?page_id=9 、http://file-examples.com/index.php/sample-audio-files/
- https://archive.org/about/terms 、https://archive.org/download/island_moreau_librivox/island_dr_moreau_00_wells.mp3 、https://archive.org/services/collection-rss.php?collection=librivoxaudio
- https://librivox.org/?p=31376/ 、https://pressbooks.library.torontomu.ca/doctormoreau/?p=90
- https://commons.wikimedia.org/wiki/Commons:First_steps/Reuse 、https://commons.wikimedia.org/wiki/Commons:Reusing_content_outside_Wikimedia 、https://upload.wikimedia.org/wikipedia/commons/5/53/Rossini_-_Il_Barbiere_di_Siviglia_-_La_calunnia_-_Francesco_Navarini_%281903%29_version_30secondes.ogg
- https://developer.mozilla.org/en-US/docs/Web/Media/Audio_and_video_delivery 、https://developer.mozilla.org/en-US/docs/Learn_web_development/Core/Structuring_content/Test_your_skills/Audio_and_video

**HLS / DASH 测试流**
- https://developer.apple.com/streaming/examples/ 、https://developer.apple.com/streaming/examples/advanced-stream-hevc.html 、https://developer.apple.com/library/ios/documentation/networkinginternet/conceptual/streamingmediaguide/deployinghttplivestreaming/deployinghttplivestreaming.html
- https://devstreaming-cdn.apple.com/videos/streaming/examples/bipbop_adv_example_hevc/master.m3u8 、https://devstreaming-cdn.apple.com/videos/streaming/examples/img_bipbop_adv_example_ts/master.m3u8
- https://test-streams.mux.dev/x36xhzz/x36xhzz.m3u8 、https://stream.mux.com/v69RSHhFelSm4701snP22dYz2jICy4E4FUyk02rW4gxRM.m3u8
- https://cph-p2p-msl.akamaized.net/hls/live/2000341/test/master.m3u8 、https://demo.unified-streaming.com/k8s/features/stable/video/tears-of-steel/tears-of-steel.ism/.m3u8
- https://github.com/video-dev/hls.js/blob/master/tests/test-streams.js
- https://shaka-player-demo.appspot.com/ 、https://developers.google.com/widevine/open-source/shaka-player
- https://dashif.org/DASH-IF-Conformance/md_Conformance_Frontend_static_faq_page.html 、https://dash-industry-forum.github.io/tools/testassets 、https://dash-industry-forum.github.io/tools 、https://dashif.org/DASH-IF-IOP/mpd-patch.pdf
- https://dash.akamaized.net/dash264/TestCases/1b/qualcomm/1/MultiRatePatched.mpd 、https://livesim.dashif.org/livesim/chunkdur_1/ato_7/testpic4_8s/Manifest.mpd 、https://livesim2.dashif.org/livesim2/patch_60/segtimeline_1/testpic_2s/Manifest.mpd 、https://storage.googleapis.com/shaka-demo-assets/angel-one/dash.mpd 、http://ftp.itec.aau.at/datasets/DASHDataset2014/BigBuckBunny/2sec/BigBuckBunny_2s_onDemand_2014_05_09.mpd

**项目内一手**
- d:/workspace/SoundBridge/AGENTS.md:111-117（测试并发与文件写入约定）

---

## 7. 未核实 / 不确定项（明确留白，勿当作结论使用）

1. samplelib、file-examples(-download) 的**完整 ToS / 是否允许热链与自动化高频访问**——仅见页面营销措辞，未见条款页。
2. MDN `learning-area` 仓库音频素材的**具体许可证**；jPlayer 素材的授权（且实测已失效）。
3. Google Chrome / Chromium、W3C 官方测试音频文件的**确切直链与授权**——本轮未找到可确认的一手页面。
4. Radio Browser 数据许可：官网原文是 **"public domain"**；二手资料常写作「CC0」，**该措辞未在官网核实**。
5. `yp.xml`（dir.xiph.org）的**数据许可与自动化访问条款**。
6. Icecast `Icy-MetaData` **没有单一官方规范页**；权威描述来自社区协议抄本 + 本笔记对 SomaFM 的实测响应头。
7. FFmpeg 项目构建**是否启用 `dash` demuxer**、以及 `protocol_whitelist` 具体取值——未核实。
8. FFmpeg 4.4 `hls.c` 页面 520 行后内容被截断，实际 `avio_open2` / `ffio_open_whitelist` / `avformat_open_input` 调用**未逐字核对**；`AVFMT_NOFILE` 日志文本来自二手引用。
9. SomaFM `ice.somafm.com/groovesalad` 等旧直链 `404` 的**确切原因**（地域限制 / UA / 挂载迁移）——未核实。
10. Akamai / Mux / Unified Streaming / Bitmovin / Shaka demo assets 各自的**正式 ToS、是否允许自动化测试、是否允许产品内置**——未核实。
11. HLS `#EXT-X-BYTERANGE` 与 FFmpeg 4.4 `hls.c` 的**具体交互细节**——未逐行核对。
12. DASH 规范本身为 ISO/IEC 23009-1（付费标准），本笔记未引用其原文；2.4.2 第 3 点借 HLS RFC 作类比，**非 DASH 一手依据**。
13. 各站 `robots.txt` 内容——未核实。
14. Radio Paradise 返回伪 `Content-Range` 总长是服务端特定行为还是 Icecast 通用做法——**未核实**；本轮只在 `aac-128` 一条上观测到。
