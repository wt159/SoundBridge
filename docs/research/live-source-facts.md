# 直播源事实基线：可复用的连接接缝与 FFmpeg 4.4 LIVE/ICY 行为

> 取证日期：**2026-11-13**（时点快照）
> 对应：GitHub issue #148（父票 #147）
> 性质：**纯取证、只读**。未改生产代码 / 用例 / CI / CMake / 规格；未跑 ctest；本文件是本次唯一新增文件。
> 方法：仓库源码逐行 `Read`/`Grep`；FFmpeg 4.4.4 本地源码（`sdk/3rdparty/ffmpeg-4.4.4/`）逐字引用；**构建产物实测**——用仓库自带 `dist/windows_x86_64_gcc_debug` 的头文件 + `libav*.dll.a` 写一次性探针（放在仓库外 `D:/tmp/sbprobe/`，未入库），对本地回环 HTTP / HLS 服务跑真实 `avformat_open_input` / `av_read_frame` / `av_seek_frame`。
> 标注纪律：**【事实】** = 源码行 / 逐字引用 / 命令输出；**【推断】** = 由事实推出但未经运行验证；**【未核实】** = 本次不可知（附原因）。
> 行号会漂移：本文件所有行号对应 2026-11-13 工作树（master `69e5114dc`）。

---

## 0. 结论速览

| 组 | 一句话结论 | 强度 |
|---|---|---|
| 1 重连接缝 | `reconnectAtOffset` 生产调用点**确实为零**，且它不在持有者实际持有的 `DataSourceBase` 上；三个天然候选位置缺的都是同一批东西（字节偏移、错误通道、可用的 `readAt`） | 事实（grep） |
| 2 读失败 vs 结束 | `av_read_frame` 的**任何**负返回在 `HlsExtractor::packetAt` 都译成 `nullptr`，`PacketCursor` 在未知总数下把 `nullptr` 定义成「正常结束」；EOF / EIO / EXIT 三条路径在提取器层完全不可分辨 | 事实（源码）+ 实测（EOF） |
| 3 HLS LIVE | 无 `#EXT-X-ENDLIST` 的 playlist **会**自行重载；窗口耗尽、segment 404、`m3u8_hold_counters` 耗尽一律以 `AVERROR_EOF` 收场（不报错）；LIVE 的 `duration = AV_NOPTS_VALUE`；`av_seek_frame` 在 LIVE 上失败并被洗成裸 `-1` | 事实（源码 + 实测） |
| 4 ICY | `http` 的 `icy` **默认 1**（会发 `Icy-MetaData: 1`）；`HttpSource` 用 7 条判据拒 ICY，但**自己不发** `Icy-MetaData`，检测依赖服务端是否主动回 `icy-*` 头 | 事实（源码 + 实测默认值） |
| 5 重连选项 | `reconnect`/`reconnect_streamed` 默认 0、`reconnect_delay_max` 默认 120s；`timeout` **不是 http 协议选项**（实测 `Option not found`）只落到 tcp；hls 仅把 7 个 avio 选项下传 segment，**reconnect 家族不下传** | 事实（源码 + 实测） |
| 6 构建 | Windows：`CONFIG_HLS_DEMUXER 1`、`CONFIG_HTTP_PROTOCOL 1`、`CONFIG_DASH_DEMUXER 0`，DLL 实测 hls 在、dash 不在；Linux 无产物，**推断**同默认（dash 依赖 libxml2） | 事实（Win）/ 推断（Linux） |

---

## 1. 重连接缝现状

### 1.1 生产调用点为零（【事实】）

`sdk/extractor/DataSource.hpp:173`：

```cpp
    virtual sdk_utils::status_t reconnectAtOffset(off64_t /*offset*/)
    {
        return sdk_utils::ERROR_UNSUPPORTED;
    }
```

全仓（排除 `sdk/3rdparty/`）grep 命中 **5 处，全部是声明或测试替身**：

```
$ grep -rn "reconnectAtOffset" --include=*.hpp --include=*.cpp --include=*.h --include=*.cc . | grep -v "^./sdk/3rdparty"
./sdk/extractor/DataSource.hpp:173:    virtual sdk_utils::status_t reconnectAtOffset(off64_t /*offset*/)
./sdk/test/unit/test_extractor.cpp:77:        sdk_utils::status_t reconnectAtOffset(off64_t offset) override
./sdk/test/unit/test_extractor.cpp:348:        sdk_utils::status_t reconnectAtOffset(off64_t) override { return sdk_utils::OK; }
./sdk/test/unit/test_extractor.cpp:433:        ... (MinimalDataSource)
./sdk/test/unit/test_extractor.cpp:457:        ... (MinimalDataSource)
```

三条附证：

- 【事实】**没有任何生产子类 override 它**：`FileSource` / `HttpSource` / `UrlSource` 三个 header 里都没有该方法（`grep "reconnectAtOffset" sdk/extractor/*.h` 零命中）。即：即使今天有人调用，`HttpSource`、`UrlSource` 也会落到基类 `ERROR_UNSUPPORTED`。
- 【事实】接缝不在持有者手里的类型上：`DataSourceBase`（`DataSource.hpp:7-60`）只有 `initCheck` / `readAt` / `getSize` / `flags` / `close` / `getAvailableSize`，**没有** `reconnectAtOffset`；而所有持有者持有的都是基类指针——`ExtractorHelper::getDataSource()` 返回 `DataSourceBase *`（`ExtractorHelper.hpp:148`），`HlsExtractor::m_source` 是 `DataSourceBase *`（`HlsExtractor.h`），`AudioStreamDecoder::m_extractor` 是 `ExtractorHelper *`（`AudioStreamDecoder.h:134`）。
- 【事实】直通路径根本不走 `readAt`：`UrlSource::readAt()` 恒返回 `-1`（`UrlSource.cpp:151-159`，注释「直通路径不经 readAt 供数（FFmpeg 原生 URL 层自取）」）；`HlsExtractor::open()` 也不传自定义 `pb`（`HlsExtractor.cpp:100-101`）。`reconnectAtOffset` 的语义（按字节偏移重连并让后续 `readAt` 从该偏移继续）在这条路径上没有承载点。

### 1.2 三个候选位置各缺什么（【推断】，缺口的行号是事实）

| 候选 | 现有形状 | 缺的东西 |
|---|---|---|
| `HlsExtractor::packetAt`（`sdk/extractor/hls/HlsExtractor.cpp:161-209`） | 只有包序号 `index` / `m_consumed`，没有字节偏移概念；`av_read_frame` 的负返回在 191 行直接 `return nullptr`；`m_fmt` 由 FFmpeg 全权持有，重开只能 `avformat_close_input` + `avformat_open_input`（即从头开始，不是「at offset」） | ① 包序号→字节偏移的映射（HLS 的「offset」在 FFmpeg 内部是 segment 序号 + 段内偏移，不暴露）；② 一个能表达「失败 / 该重连 / 结束」三种结果的返回通道（现在只有 `nullptr`）；③ 一个在直通路径上真的可用的 `reconnectAtOffset` 实现 |
| `AudioStreamDecoder` 读循环 | 持有 `ExtractorHelper *`（`AudioStreamDecoder.h:134`）；`PacketCursor::nextPacket` 只返回 `bool`（`ExtractorHelper.hpp:236-261`）；HLS 走 `runFfmpegPacketLoop`（`AudioStreamDecoder.cpp:1024`） | ① 错误码通道（`bool` 丢掉了 `AVERROR_*`）；② 偏移量；③ 数据源指针（当前只能经 `m_extractor->getDataSource()` 拿到 `DataSourceBase *`，无 `reconnectAtOffset`） |
| `MusicPlayer` 控制队列 | `m_workQueue` 串行回调（`MusicPlayer.h` / `MusicPlayer.cpp:107-108`）；EOS 处理是 `advanceToNextTrack()`（`MusicPlayer.cpp:581-592`），错误处理是 `handleDecodeError`（`MusicPlayer.cpp:595-640`） | ① 位置语义：播放器只会「重开本曲」（回到 0），拿不到「断点续传」的字节偏移；② 重连会与 `m_positionEpoch` / 曲目代际交织（`MusicPlayer.cpp:558-570`）；③ 它只能消费解码器已上报的 `ErrorCode`，而解码器在 HLS 路径上只会报 `DataCorrupted` / 正常 EOS（`AudioStreamDecoder.cpp:1035`、`1038`），没有「可重连的读失败」这一类 |

**共同缺口一句话**：三条候选都缺「可区分的读失败」这一前置事实（见第 2 组），否则重连触发条件无从判定。

---

## 2. 读失败与流结束现状不可分辨

### 2.1 三处逐字

**① `sdk/extractor/hls/HlsExtractor.cpp:184-192`**（跳包循环，`av_read_frame` 的**所有**负返回同一出口）：

```cpp
    int ret = 0;
    bool gotAudio = false;
    for (int skipped = 0; skipped < kNonAudioPacketSkipLimit; ++skipped) {
        ret = av_read_frame(m_fmt, m_pkt);
        if (ret < 0) {
            // 包耗尽与坏数据都走这里：可解性不在提取器判定（ADR 0002），
            // 交给播放期的「包耗尽 + EOS 汇总」兜底。
            return nullptr;
        }
```

**② `sdk/extractor/ExtractorHelper.hpp:233-235`**（把 `nullptr` 定义成「结束」）：

```cpp
    // 拉下一包；返回 false 表示序列正常结束。空包（nullptr 或 0 字节）不产出、直接跳过，
    // 与整块视图拼合时「跳过空包」的语义一致。
    // 未知总数（kUnknownPacketCount）的序列没有「跳过空包继续」的余地：nullptr 就是结束。
```

配合 `ExtractorHelper.hpp:245-253`：

```cpp
        while (!countKnown || m_nextIndex < count) {
            AudioBuffer::AudioBufferPtr current = m_extractor->packetAt(m_nextIndex);
            ++m_nextIndex;
            if (current == nullptr) {
                if (!countKnown) {
                    return false;
                }
                continue;
            }
```

HLS 的 `packetCount()` 恒为 `kUnknownPacketCount`（`HlsExtractor.h` 覆写），所以 HLS 一定走 `return false` 这条。

**③ `sdk/audio/decode/AudioStreamDecoder.cpp:704-709`**（唯一区分「取消」与「EOS」的分支）：

```cpp
            if (!cursor.nextPacket(packet)) {
                if (m_stopRequest.load() || m_abortDecode.load()) {
                    break;   // P1：取消导致的读中断不是 EOS，不触发曲目结束序列
                }
                finishAtEndOfStream();
                break;
            }
```

### 2.2 `AVERROR_EOF` / `AVERROR(EIO)` / `AVERROR_EXIT` 各落到哪（【事实】+【推断】）

| 错误码 | FFmpeg 侧产生点 | 到 `packetAt` | 到解码侧 |
|---|---|---|---|
| `AVERROR_EOF`（`-541478725`） | `hls.c:2291`（`hls_read_packet` 无包可出）、`hls.c:1456/1466/1492/1499`（不再需要 / 窗口耗尽 / hold 计数耗尽 / `finished`） | `191` 行 `return nullptr` | `nextPacket()==false` → 非取消 → `finishAtEndOfStream()`（EOS） |
| `AVERROR(EIO)` | ① `http.c:1493-1500`「Stream ends prematurely」：HLS **VOD**（有 `Content-Length`）中途断流；② `hls.c:1531-1532` 段 404/打不开 → **不是 EIO**，是跳过；③ `hls.c:2176` 播放列表重载失败时**会**把 `parse_playlist` 的错误（如 `AVERROR_HTTP_NOT_FOUND`）原样上抛 | 同上，`191` 行吞掉 | 同上，EOS |
| `AVERROR_EXIT`（`-1414092869`） | `avio.c:367`（`ff_check_interrupt`）、`hls.c:1501/1526/1551`（重载/开段时 `ff_check_interrupt`），由 `m_fmt->interrupt_callback`（`HlsExtractor.cpp:93-94`）触发 | 同上，`191` 行吞掉 | **区分得开**：`stop()` 同时置 `m_stopRequest` 与 `m_abortDecode`（`AudioStreamDecoder.cpp:216-217`），`runFfmpegPacketLoop` 在读前（`1006-1016`）与读后（`1026-1028`）都能 break，不进 EOS |

三条附注（【事实】）：

- 唯一能让非 EOF 错误逃出 hls demuxer 的路径是**播放列表重载失败**：`hls.c:2174-2180`
  ```cpp
                ret = av_read_frame(pls->ctx, pls->pkt);
                if (ret < 0) {
                    if (!avio_feof(&pls->pb) && ret != AVERROR_EOF)
                        return ret;
  ```
  但 `HlsExtractor.cpp:188-191` 不看 `ret` 的值，这条信息在提取器边界丢失。
- **segment 404 不会报错**：`hls.c:1524-1532` 打一条 warning，`v->cur_seq_no += 1`，`goto reload`——继续试下一段。
- 解码侧对 HLS 的 EOS/错误汇总还有一层「不可分辨」：`runFfmpegPacketLoop` 里 `nextPacket` 返回 false 后先 `flushDecode()`，`flushRet < 0` 才报 `DataCorrupted`，否则 `finishAtEndOfStream()`（`AudioStreamDecoder.cpp:1024-1039`）。

### 2.3 实测（【事实】）

用本地 HLS 服务（1 个有效段 + 1 个 404 段，无 `#EXT-X-ENDLIST`，`m3u8_hold_counters=2`）跑真实 `av_read_frame`：

```
open ret=0 (0.039s) dict_left=-1
after open: duration=-9223372036854775808  nb_streams=1  ctx_flags=0x2
...
read#99 ret=0 st=0 size=71 pts=-9223372036854775808 blocked=0.000s
read#100 ret=-541478725 (End of file) blocked=1.560s
```

服务端日志（同一时刻）：

```
[07:39:15] "GET /seg2.aac HTTP/1.1" 404 -
[07:39:16] "GET /live_static.m3u8 HTTP/1.1" 200 -
[07:39:17] "GET /live_static.m3u8 HTTP/1.1" 200 -
```

即：**段 404 + 窗口耗尽 + hold 计数耗尽，最终都是 `AVERROR_EOF`**，调用方拿不到任何「这是网络故障」的信号。

---

## 3. FFmpeg 4.4 HLS LIVE 行为

### 3.1 无 `#EXT-X-ENDLIST` 是否自行重载：**会**

- 【事实】`#EXT-X-ENDLIST` 只置 `pls->finished = 1`：`hls.c:878-880`
  ```cpp
        } else if (av_strstart(line, "#EXT-X-ENDLIST", &ptr)) {
            if (pls)
                pls->finished = 1;
  ```
- 【事实】重载间隔由**最后一段的 `#EXTINF` 时长**决定，没有段时退回 `#EXT-X-TARGETDURATION`：`hls.c:1380-1385`
  ```cpp
  static int64_t default_reload_interval(struct playlist *pls)
  {
      return pls->n_segments > 0 ?
                            pls->segments[pls->n_segments - 1]->duration :
                            pls->target_duration;
  }
  ```
  （`target_duration` 来自 `#EXT-X-TARGETDURATION`：`hls.c:823` `pls->target_duration = t * AV_TIME_BASE;`，即秒 → 微秒。）
- 【事实】`read_data()` 的重载 → 取段循环：`hls.c:1459-1479`
  ```cpp
        /* If this is a live stream and the reload interval has elapsed since
         * the last playlist reload, reload the playlists now. */
        reload_interval = default_reload_interval(v);
  reload:
        reload_count++;
        if (reload_count > c->max_reload)
            return AVERROR_EOF;
        if (!v->finished &&
            av_gettime_relative() - v->last_load_time >= reload_interval) {
            if ((ret = parse_playlist(c, v->url, v, NULL)) < 0) {
  ...
            reload_interval = v->target_duration / 2;
  ```
  注意：**首轮之后间隔改成 `TARGETDURATION / 2`**（1478 行），不是 `TARGETDURATION`。
- 【事实】实测：本地服务每秒推进一次窗口，客户端观测到 `GET /live.m3u8` 与 `GET /segN.aac` 按约 1 s 节奏成对出现，`av_read_frame` 每次阻塞到下一段到货才返回 0（`blocked=0.9xx s`）。见 §8 复现。

### 3.2 窗口耗尽 / segment 404 时 `av_read_frame` 返回什么

| 情形 | 源码行为 | 对调用方的返回 |
|---|---|---|
| 窗口耗尽但 `finished`（VOD 播完） | `hls.c:1497-1499` `if (v->finished) return AVERROR_EOF;` | `AVERROR_EOF` |
| 窗口耗尽、LIVE、还有机会 | `hls.c:1500-1506`：`while (now - last_load_time < reload_interval) { if (ff_check_interrupt) return AVERROR_EXIT; av_usleep(100*1000); } goto reload;` | **阻塞**（100 ms 粒度轮询），到点重载；取消则 `AVERROR_EXIT` |
| 重载后仍无新段 | `hls.c:1486-1493`：`m3u8_hold_counters++`，`>= c->m3u8_hold_counters`（默认 **1000**）→ `return AVERROR_EOF` | `AVERROR_EOF` |
| 单次 `read_data` 内重试过多 | `hls.c:1464-1466` `reload_count > c->max_reload`（默认 **1000**）→ `return AVERROR_EOF` | `AVERROR_EOF` |
| segment 404 / 打不开 | `hls.c:1524-1532`：warning + `v->cur_seq_no += 1` + `goto reload` | **不返回错误**，跳过继续 |
| segment 读到一半断流（VOD，有 `Content-Length`） | `http.c:1493-1500` `return AVERROR(EIO)` → `hls.c:1580-1590` 关掉、`v->cur_seq_no++`、`goto restart` | **不返回错误**，跳过继续 |

实测印证：`read#100` 在 1.56 s 阻塞后返回 `AVERROR_EOF`，其间服务端只发生 1 次 404 + 2 次 playlist 重载（见 §2.3 日志）。

### 3.3 LIVE 下 `avformat_find_stream_info` 是否阻塞

- 【事实】上限是**累计流时间**而不是墙钟：`utils.c:3626-3630`（默认 `max_analyze_duration = 5 * AV_TIME_BASE`，`mpegts` 为 7 s），判定处 `utils.c:3896-3912`：`codec_info_duration` 累加自 `pkt->duration`，达到 `limit` 才 `break`。
- 【事实】另有字节上限 `probesize`（默认 5 000 000，`options_table.h:39`），`utils.c:3801` `if (read_size >= probesize) { ... break; }`。
- 【事实】HLS 内层子上下文的这两个值被改写：`hls.c:1999-2000`
  ```cpp
        pls->ctx->probesize = s->probesize > 0 ? s->probesize : 1024 * 4;
        pls->ctx->max_analyze_duration = s->max_analyze_duration > 0 ? s->max_analyze_duration : 4 * AV_TIME_BASE;
  ```
  但外层（`HlsExtractor::open()` 调用的那个 `m_fmt`）仍是 5 s / 5 MB，`HlsExtractor.cpp:110` 未改这两项。
- 【事实】读错误在这里被吞：`utils.c:3811-3816`
  ```cpp
        ret = read_frame_internal(ic, pkt1);
        if (ret == AVERROR(EAGAIN))
            continue;

        if (ret < 0) {
            /* EOF or error*/
            eof_reached = 1;
            break;
  ```
  → 之后只要 `has_codec_parameters` 成立，函数仍返回 `>= 0`。
- 【推断】因此 LIVE 上 `find_stream_info` 的阻塞时长 ≈「凑够 5 s 流时间 / 5 MB」所需时间，取决于段下载速度与包的 `duration`；对真实 AAC/MP3 直播约为数个 segment 的下载时间，**不是固定 5 s 墙钟**。
- 【事实】实测反例（说明「时间上限」依赖 `pkt->duration`）：构造的 ADTS 段 `pkt->duration == 0` 时，`codec_info_duration` 永远累加不到 5 s，`find_stream_info` 在 40 s 超时内都没有返回（`timeout 40` 退出码 124）。

### 3.4 LIVE 下 `AVFormatContext::duration` 取值

- 【事实】默认值是 `AV_NOPTS_VALUE`：`utils.c:584` `s->duration = s->start_time = AV_NOPTS_VALUE;`
- 【事实】hls 只在 playlist 已 `finished`（见过 `ENDLIST`）时才写 `s->duration`：`hls.c:1918-1923`
  ```cpp
      /* If this isn't a live stream, calculate the total duration of the
       * stream. */
      if (c->variants[0]->playlists[0]->finished) {
          int64_t duration = 0;
          for (...) duration += ...segments[i]->duration;
          s->duration = duration;
      }
  ```
- 【事实】实测：LIVE `duration=-9223372036854775808`；同一份内容加 `#EXT-X-ENDLIST` 后 `duration=2000000`（2 s）。
- 【事实】`HlsExtractor.cpp:135` 把负值归一成 0：`m_declaredDurationUs = m_fmt->duration > 0 ? m_fmt->duration : 0;` → LIVE 的 `durationMs` 落 0，与规格 §3「`durationMs == 0` 语义是标称时长未知」一致。

### 3.5 `av_seek_frame` 在 LIVE 上的行为

- 【事实】hls 的 `read_seek`：`hls.c:2294-2360`。找段失败 → `AVERROR(EIO)`：`hls.c:2334-2335`
  ```cpp
      if (!seek_pls || !find_timestamp_in_playlist(c, seek_pls, seek_timestamp, &seq_no))
          return AVERROR(EIO);
  ```
  时间戳早于 `first_timestamp` 时 `find_timestamp_in_playlist` 返回 0（失败）：`hls.c:1649-1660`。
- 【事实】`duration` 只用于「越过末尾」判定：`hls.c:2315-2318` `duration = s->duration == AV_NOPTS_VALUE ? 0 : s->duration;` → LIVE 下该值为 0，此判定不生效。
- 【事实】**错误码会被洗掉**：hls 声明了 `AVFMT_NOGENSEARCH`（`hls.c:2424`），所以 `seek_frame_internal` 在 `read_seek` 失败后既不尝试 `read_timestamp` 也不退到 generic seek，直接 `return -1`（`utils.c:2470-2483`）。调用方看到的是裸 `-1`（= `EPERM`），不是 `EIO`。
- 【事实】实测（LIVE，同一份内容）：`av_seek_frame(fmt, 0, 0, AVSEEK_FLAG_BACKWARD) ret=-1 (Operation not permitted)`；VOD（带 `ENDLIST`）同参数 `ret=0 (ok)`。
- 【事实】LIVE 起始段位由 `live_start_index` 决定，默认 **-3**（`hls.c:1702-1709` + 实测探针打印 `live_start_index default=-3`），即从窗口末尾往前 3 段开始。

---

## 4. ICY 侧行为

### 4.1 `icy` 选项默认值：**1**

- 【事实】`http.c:152`
  ```c
      { "icy", "request ICY metadata", OFFSET(icy), AV_OPT_TYPE_BOOL, { .i64 = 1 }, 0, 1, D },
  ```
- 【事实】实测（探针 `av_opt_get(pb, "icy", AV_OPT_SEARCH_CHILDREN)` 打真实 http 连接）：
  ```
  get icy              ret=0 value=true
  ```
  即**默认就会请求** ICY 元数据，除非显式 `icy=0`。

### 4.2 `Icy-MetaData` / `StreamTitle` 读取路径

- 【事实】请求头：`http.c:1372-1373`
  ```c
      if (!has_header(s->headers, "\r\nIcy-MetaData: ") && s->icy)
          av_bprintf(&request, "Icy-MetaData: 1\r\n");
  ```
- 【事实】响应头：`Icy-MetaInt` 单独解析（`http.c:1101-1102` `s->icy_metaint = strtoull(p, NULL, 10);`），其余 `Icy-*` 走 `parse_icy` → 拼进 `icy_metadata_headers`（`http.c:802-816`）。
- 【事实】流内元数据块：`store_icy()`（`http.c:1642-1685`）在 `icy_metaint` 字节音频后读 1 字节长度 + `len*16` 字节数据，`av_opt_set(s, "icy_metadata_packet", data, 0)` 并调 `update_metadata()`；`update_metadata`（`http.c:1614-1639`）把 `key='value';` 拆进 `s->metadata`。`StreamTitle` 就是这条路径上的一个 key，**不是独立 API**。
- 【事实】剥离后交付：`http_read()`（`http.c:1684-1692`）先 `store_icy()` 再 `http_read_stream()`，所以上层 demuxer 看到的是不含元数据块的纯音频字节。

### 4.3 无限字节流下 `av_read_frame` 的正常返回形态

- 【事实】无 `Content-Length` 时 http 协议置 `filesize = UINT64_MAX`、`is_streamed = 1`（`http.c:1406`、`http.c:1068-1078`、`http.c:621-625`）。
- 【事实】`av_read_frame` 成功即返回 0 并给出一个包（`avformat.h:2331` 的 `@return 0 if OK, < 0 on error or end of file`）。对无限流，**0 会无限次返回**，只有当连接被对端关闭（`http.c:1466-1472`）或发生不可重连的错误时才返回负值；`AVERROR_EOF` 与「网络断开」在 demuxer 层仍不可区分（见第 2 组）。
- 【推断】对 ICY，正常形态就是「0 + 一个 MP3/AAC 包」，包间隔由服务端码率决定；`av_read_frame` 本身不做任何节奏控制（无 sleep、无墙钟对齐），消费侧必须自己限速（SoundBridge 靠 ring 背压）。

### 4.4 `HttpSource::probe()` 拒绝 ICY 的逐条判据（行号 = `sdk/extractor/HttpSource.cpp`）

| # | 判据 | 行号 | 备注 |
|---|---|---|---|
| 1 | URL 以 `https://` 开头 → 拒绝 | 599-602 | 与 ICY 无关，但会先命中 |
| 2 | 扩展名 `.m3u8` / `.mpd` → 拒绝 | 290-292（`isPlaylistExtension`）、608-611 | 同上 |
| 3 | **响应头出现任意 `icy-` 前缀** → `"Icy-* header present, reject infinite stream"` | 685-688（判据）、345-353（`hasIcyHeader`，`lowerCopy` 后比 `"icy-"` 前缀） | **唯一针对 ICY 的判据** |
| 4 | 状态码不是 206 | 710-712 | 无限流通常回 200 → 命中这条 |
| 5 | 缺 `Accept-Ranges: bytes` | 714-717（判定函数 356-360） | Icecast 一般不回 |
| 6 | `Content-Range` 总长缺失/非法/`<=0` | 719-722 | |
| 7 | 内容像清单（Content-Type / `#EXTM3U` / `<?xml`） | 725-728（`looksLikePlaylist` 525-556） | |
| 附 | 伪长度二次/三次校验（尾段、首字节复查）也各自再判 `icy-*` 与 206 | 729-736；`verifyTail` 559-570、`verifyHeadAgain` 574-584、`readAt` 655 | |

### 4.5 判据的实际覆盖面（【事实】+【推断】）

- 【事实】`HttpSource` 的请求头只有 4 行，**不含 `Icy-MetaData`**：`HttpSource.cpp:419-423`
  ```cpp
    const std::string req = "GET " + path + " HTTP/1.1\r\n"
                          + "Host: " + hostHeader + "\r\n"
                          + "Range: bytes=" + std::to_string((long long)start) + "-"
                          + std::to_string((long long)end) + "\r\n"
                          + "Connection: close\r\n"
                          + "\r\n";
  ```
- 【推断】Icecast 一类服务端的 `icy-metaint` 只在收到 `Icy-MetaData: 1` 时才出现；不发该头时，判据 3 能命中的只剩服务端无条件回写的 `icy-name` / `icy-genre` / `icy-br` 等。**若某服务端只回 `icy-metaint` 且不无条件回其它 `icy-*`，判据 3 会漏**，实际拦截退回判据 4/5/6（200 + 无 `Accept-Ranges`）。这三条对「伪 `Content-Range` + 206」的服务端不成立——与规格 §4.3 已记录的 Radio Paradise 风险同源。

---

## 5. 重连相关选项在 4.4 的语义与默认值

### 5.1 语义与默认值（【事实】）

`http.c:160-171`（逐字）：

```c
    { "reconnect", "auto reconnect after disconnect before EOF", OFFSET(reconnect), AV_OPT_TYPE_BOOL, { .i64 = 0 }, 0, 1, D },
    { "reconnect_at_eof", "auto reconnect at EOF", OFFSET(reconnect_at_eof), AV_OPT_TYPE_BOOL, { .i64 = 0 }, 0, 1, D },
    { "reconnect_on_network_error", "auto reconnect in case of tcp/tls error during connect", OFFSET(reconnect_on_network_error), AV_OPT_TYPE_BOOL, { .i64 = 0 }, 0, 1, D },
    { "reconnect_on_http_error", "list of http status codes to reconnect on", OFFSET(reconnect_on_http_error), AV_OPT_TYPE_STRING, { .str = NULL }, 0, 0, D },
    { "reconnect_streamed", "auto reconnect streamed / non seekable streams", OFFSET(reconnect_streamed), AV_OPT_TYPE_BOOL, { .i64 = 0 }, 0, 1, D },
    { "reconnect_delay_max", "max reconnect delay in seconds after which to give up", OFFSET(reconnect_delay_max), AV_OPT_TYPE_INT, { .i64 = 120 }, 0, UINT_MAX/1000/1000, D },
```

| 选项 | 默认 | 归属 | 备注 |
|---|---|---|---|
| `reconnect` | 0 | http | 「EOF 之前断开」才重连 |
| `reconnect_streamed` | 0 | http | 流式（`is_streamed`）源的**前置开关** |
| `reconnect_delay_max` | **120**（秒） | http | 退避 1→3→7→…，超过即 `AVERROR(EIO)`（`http.c:1580-1581`） |
| `reconnect_at_eof` | 0 | http | |
| `rw_timeout` | **0**（无超时），微秒 | `URLContext`（`avio.c:60`） | 所有协议共享 |
| `timeout` | **http 无此选项**；tcp 有（`tcp.c:55`，-1 = 无限制） | tcp | 见 5.2 |

### 5.2 实测：`timeout` 不是 http 协议选项（【事实】）

```
get rw_timeout       ret=0 value=0
get timeout          ret=-1414549496 value=-
set timeout         =1234567  ret=-1414549496 (Option not found)
set rw_timeout      =1234567  ret=0 (ok)
leftover dict:                        # 空：说明 timeout 被下层 tcp 消费掉了
```

同一个探针连 `tcp://` 时：

```
get rw_timeout       ret=0 value=3000000
get timeout          ret=0 value=3000000
set timeout         =1234567  ret=0 (ok)
```

-1414549496 = `AVERROR_OPTION_NOT_FOUND`。即：`UrlSource.cpp:194` 的 `av_dict_set(&opts, "timeout", "3000000", 0)` **不会**被 http 协议识别，只会经 `http_open` 的 `s->chained_options`（`http.c:630` 拷贝 → `http.c:306` 取回 → `http.c:253` 传给 tcp）落到 tcp 的 `rw_timeout`。**真正对 http 有意义的名字是 `rw_timeout`**，但它只被 `URLContext` 消费（`avio.c:322-325` 的 `av_opt_set_dict`），且 `avio.c:383-387` 只在 `EAGAIN` 重试路径上用它——阻塞 socket 不会走到那里。

### 5.3 hls demuxer 重开的 segment 请求是否吃到这些选项

- 【事实】hls 只把**固定 7 个** avio 选项从 `s->pb` 抄给 segment 请求：`hls.c:1714-1734`
  ```cpp
  static int save_avio_options(AVFormatContext *s)
  {
      ...
      static const char * const opts[] = {
          "headers", "http_proxy", "user_agent", "cookies", "referer", "rw_timeout", "icy", NULL };
  ```
  调用点 `hls.c:1878`（`hls_read_header` 开头），随后 `hls.c:1884` 额外塞入 `"seekable"`。
- 【事实】segment 打开走 `open_url()` → `s->io_open(s, pb, url, AVIO_FLAG_READ, &tmp)`（`hls.c:673-691`），`tmp` = `c->avio_opts` + 段级 `offset` / `end_offset`。
- 由此得出：

| 选项 | 对 hls 的 segment 请求是否生效 | 依据 |
|---|---|---|
| `headers` / `user_agent` / `cookies` / `referer` / `http_proxy` | 生效 | `hls.c:1716` |
| **`rw_timeout`** | 生效（若在顶层 open 时设置） | `hls.c:1716` |
| `icy` | 生效 | `hls.c:1716` |
| **`timeout`** | **不生效**：既不在 7 项里，也不是 http 选项（§5.2） | `hls.c:1716` + 实测 |
| **`reconnect` / `reconnect_streamed` / `reconnect_delay_max` / `reconnect_at_eof`** | **不生效**：不在 7 项里，segment 是全新 URLContext，回到默认 0/0/120 | `hls.c:1716` |
| `protocol_whitelist` | 生效（不走 dict，走 `AVFormatContext::protocol_whitelist`） | `avformat.h:1806-1808`、`utils.c:552-554`、`options.c:191` |

- 【事实】实测佐证「协议级选项只对被设置的那个 URLContext 生效」：给 http 加 `reconnect=1` 后 `get reconnect → true`，加 `reconnect_streamed` 后才 `true`；这属于 http 实例，不会传递给后续新建的 segment 连接。

### 5.4 对「无限流」重连的真实语义（【事实】）

`http.c:1566-1582`：

```c
    read_ret = http_buf_read(h, buf, size);
    while (read_ret < 0) {
        uint64_t target = h->is_streamed ? 0 : s->off;

        if (read_ret == AVERROR_EXIT)
            break;

        if (h->is_streamed && !s->reconnect_streamed)
            break;

        if (!(s->reconnect && s->filesize > 0 && s->off < s->filesize) &&
            !(s->reconnect_at_eof && read_ret == AVERROR_EOF))
            break;
```

- 必须**同时**开 `reconnect_streamed` 与 `reconnect` 才会进入重连（流式源单开 `reconnect` 无效）。
- `target = h->is_streamed ? 0 : s->off` → 流式源重连时**从头（offset 0）重开**，不是「断点续传」。
- 退避 `1, 3, 7, …` 秒，超过 `reconnect_delay_max` → `AVERROR(EIO)`（`http.c:1580-1581`）。
- 与 `HlsExtractor.cpp:93-94` 的 `interrupt_callback` 不冲突：`ff_network_sleep_interruptible` 期间可被取消（`http.c:1583-1585`）。

---

## 6. 构建可用性

### 6.1 Windows（【事实】，`sdk/3rdparty/ffmpeg-4.4.4/config.h`）

```
2217:#define CONFIG_DASH_DEMUXER 0
2266:#define CONFIG_HLS_DEMUXER 1
2670:#define CONFIG_HTTP_PROTOCOL 1
2672:#define CONFIG_HTTPS_PROTOCOL 1
2691:#define CONFIG_TLS_PROTOCOL 1
432:#define CONFIG_OPENSSL 0
445:#define CONFIG_GNUTLS 0
```

- 【事实】`configure:3318` `dash_demuxer_deps="libxml2"` → DASH 关闭是因为 libxml2 未启用。
- 【事实】HTTPS/TLS 协议编进去了，但**没有任何 TLS 后端**（OPENSSL 0、GNUTLS 0）。

### 6.2 构建产物实测（【事实】，仓库自带 DLL）

探针（仓库外编译，链接 `dist/windows_x86_64_gcc_debug/lib/libavformat.dll.a`）：

```
demuxer hls      : Apple HTTP Live Streaming
demuxer dash     : (absent)
demuxer mpegts   : MPEG-TS (MPEG-2 Transport Stream)
demuxer mp3      : MP2/3 (MPEG audio layer 2/3)
demuxer aac      : raw ADTS AAC (Advanced Audio Coding)
protocols: async cache concat crypto data ffrtmphttp file ftp gopher gophers hls http httpproxy https mmsh mmst pipe rtmp rtmps rtmpt rtmpts rtp srtp subfile tcp tls udp udplite
hls options:
  live_start_index       default=-3
  max_reload             default=1000
  m3u8_hold_counters     default=1000
  http_persistent        default=1
  http_multiple          default=-1
  http_seekable          default=-1
avformat version=3820644
```

- 【事实】`https://` 实际不可用：显式放开白名单（`"https,tls,tcp"`）后 `avio_open2` 仍失败：`https open ret=-1313558101 (av_strerror: Unknown error occurred)`。与 §6.1 无 TLS 后端一致。
- 【事实】hls 的 `http_persistent` 默认 **1**（`hls.c:2403`），即同域名 segment 复用 keep-alive 连接（`hls.c:1242-1243`、`hls.c:607-622`）——这会让「每段新建连接」的假设不成立。

### 6.3 Linux（【推断】）

- 【事实】`sdk/3rdparty/ffmpeg-4.4.4/build_linux_arm.sh` 是裸 `./configure --cross-prefix=arm-linux-gnueabihf- --target-os=linux --arch=arm …`，**没有** `--disable-demuxers` / `--disable-protocols`，也没有 `--enable-libxml2`。
- 【事实】仓库内没有 Linux 侧的 `config.h` 或预编译产物（`dist/` 只有 `windows_x86_64_gcc_debug`）。
- 【推断】按 4.4 默认值：`CONFIG_HLS_DEMUXER 1`、`CONFIG_HTTP_PROTOCOL 1`；`CONFIG_DASH_DEMUXER` 取决于交叉编译环境是否装了 libxml2，**默认应为 0**。
- 【推断】HTTPS 在 Linux 侧同样不可用（未 `--enable-openssl` / `--enable-gnutls`）。
- 【未核实】Linux 实际取值：无产物可验，需在目标环境跑 `./configure` 后读生成的 `config.h`，或对 libavformat 执行 `av_find_input_format("hls")`（原因：本机无 Linux 工具链）。

---

## 7. 与既有规格描述不一致之处

对照 `doc/network-audio-source-spec.md`（§3 / §4.3 / §5.1 / §6.2）与 `docs/adr/0003`。

| # | 规格原文 | 事实 | 性质 |
|---|---|---|---|
| 1 | §3「取消」行：「`m_abortDecode` 只经 `setAbortFlag` 贯通 `FLACDecode` / `VorbisDecode` 读回调，**未贯通 4 个 `avioRead` 阻塞点**；全自有 `sdk/` 源码 `interrupt_callback` **零命中**」 | `HlsExtractor.cpp:93-94` 与 `UrlSource.cpp:195-198` 都设置了 `AVIOInterruptCB`（`sbAbortInterruptCallback`）；`AudioStreamDecoder.cpp:133` 仍只对数据源 `setAbortFlag` | **已过期**：前半句（`avioRead` 阻塞点）仍成立，后半句（零命中）不再成立 |
| 2 | §3「构建」行：「`protocol_whitelist` 项目未设置（默认 NULL）」 | `UrlSource.cpp:193` 与 `HlsExtractor.cpp:99` 均设为 `"http,tcp"` | **已过期** |
| 3 | §3「构建」行：「`CONFIG_NETWORK / HTTP / HTTPS / TLS / HLS / RTMP / TCP / UDP = 1`」隐含 https 可用 | `CONFIG_OPENSSL 0` + `CONFIG_GNUTLS 0`；实测 `https` 打开失败（`-1313558101`） | **措辞不准**：协议注册了，后端没有 |
| 4 | §5.1「三者语义由协议事实票用 FFmpeg 4.4 官方 doxygen 逐字确认」，列出「`AVSEEK_SIZE` 返回 `<0` = 不支持取大小」「`AVIOContext.seekable = 0` = 流不可 seek」 | `avio.h` 中**没有**这两句逐字表述；能逐字对上的只有 `avio.h:453-455`：*"For stream protocols, must never return 0 but rather a proper AVERROR code."*（`read_packet` 回调） | **措辞不准**：`read_packet` 那句成立且可逐字引用，另两句应标为「工程约定」而非 doxygen 原文 |
| 5 | §4.2 S3/S4 理由：「长度未知不可 seek、无自然终点、**取消信号未贯通到阻塞读点**」 | 取消已贯通（`HlsExtractor.cpp:93-94`）；「无自然终点」只对「持续有新段」成立——默认 `m3u8_hold_counters=1000`，若窗口连续不前进，约 1000×`TARGETDURATION/2` 后会以 `AVERROR_EOF` 收场（`hls.c:1486-1493`） | **部分过期 / 需限定** |
| 6 | §6.3 建议白名单「`file,http,https,tcp,tls,crypto`」 | 实现取 `"http,tcp"`（`UrlSource.cpp:193`、`HlsExtractor.cpp:99`），注释明说「不含 file/https/crypto」 | **规格与实现相反**（实现更严；且 https 本就不能用，见 #3） |
| 7 | §6.2「空 base 下相对 URI 无 scheme ⇒ 打开失败」 | 与 #148 无关的新事实：hls 的 segment 重开走 `s->io_open`（`hls.c:691`）且 `protocol_whitelist` 经 `s->protocol_whitelist` 下传（`options.c:191`）；但**只对 7 个 avio 选项**（`hls.c:1716`）——`timeout` / `reconnect*` 不下传 | **补充**：不影响结论，但「同白名单重开」成立、「同选项重开」不成立 |
| 8 | ADR 0003「`nullptr` = 序列结束；可解性只在播放期由『包耗尽 + ring 0 字节 + EOS 汇总』兜底」 | 与实现完全一致（`ExtractorHelper.hpp:235`、`HlsExtractor.cpp:188-191`、`AudioStreamDecoder.cpp:1024-1039`） | **一致**（但正因如此，任何「断线重连」都必须先改这条契约，或绕开 `PacketCursor`） |

---

## 8. 复现（本次实证用的一次性探针，未入库）

环境：Windows 10 + `D:/Qt/Tools/mingw730_64/bin/gcc` + `sdk/3rdparty/dist/windows_x86_64_gcc_debug`（头文件、`libav*.dll.a`、`bin/*.dll`）。探针源码在 `D:/tmp/sbprobe/`，与仓库无关。

```bash
# 头/库路径
INC=/d/workspace/SoundBridge/sdk/3rdparty/dist/windows_x86_64_gcc_debug/include
LIB=/d/workspace/SoundBridge/sdk/3rdparty/dist/windows_x86_64_gcc_debug/lib
BIN=/d/workspace/SoundBridge/sdk/3rdparty/dist/windows_x86_64_gcc_debug/bin

# 1) 版本/协议/解复用器/默认值
gcc -o probe.exe probe.c -I$INC -L$LIB -lavformat -lavcodec -lavutil
PATH=$BIN:$PATH ./probe.exe

# 2) 选项归属（icy 默认值、timeout 是否 http 选项、rw_timeout）
PATH=$BIN:$PATH ./probe2.exe "http://127.0.0.1:18765/probe.c"
PATH=$BIN:$PATH ./probe2.exe "tcp://127.0.0.1:18765"

# 3) LIVE 行为（本地 python 起 HLS 服务，每秒推进滑动窗口、无 ENDLIST）
python hlsserver.py 18768 &
PATH=$BIN:$PATH ./probe3.exe "http://127.0.0.1:18768/live.m3u8" 5

# 4) 窗口耗尽 + segment 404（静态清单 + 一个不存在的段，hold_counters=2）
python -m http.server 18765 --bind 127.0.0.1 &
PATH=$BIN:$PATH ./probe3.exe "http://127.0.0.1:18765/live_static.m3u8" 108 skip 2
```

`probe3` 的打印项即本文引用的观测值：`open` 耗时、`duration`、`nb_streams`、`ctx_flags`、`find_stream_info` 耗时、`av_seek_frame` 返回码、每次 `av_read_frame` 的返回码/包大小/阻塞时长。

合成素材说明（避免误读）：段是「合法 ADTS 头 + 全零负载」，只保证 demux 层可解析，**解码必然失败**（`channel element 0.0 is not allocated`）；因此本文的 LIVE 结论只覆盖 **demux / 时间轴 / 错误码**，不覆盖真实 AAC 解码或音质。
