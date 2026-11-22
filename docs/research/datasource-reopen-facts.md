# 事实核对基线：FileSource 关闭再重开对既有 extractor 的可行性（逐格式）

> 时点快照：**2026-11-22**，commit **2eef4e8f7**（`master`）。
> 取证票：GitHub issue #274（父地图 #273「千曲库的本地数据源生命周期与句柄预算」）。
> 取证方式：**只读源码核查**。事实与推断分开标注；每条事实给「路径:行号 + 符号」，行号会漂移，引用以路径 + 符号 + 摘录为准。查不到的写「未核实 + 已查范围」。
> 本次**未修改任何生产代码 / 用例 / 规格**，**未构建、未跑测试**。
> Grep 均限定到 `sdk/` 下具体目录或文件；3rdparty 只做定点读取（FFmpeg 4.4.4 的 `aviobuf.c`），不做全目录扫描。

---

## 0. 一句话结论与三分类

**结论：可以复用。** 10 个已注册格式的 extractor 在「关闭 → 重开同一路径」之后都还能正确工作，**没有任何一个必须重建**，也**没有任何一个 extractor 常驻 fd**（全仓只有 `FileSource` 自己开文件）。

成立的前提只有两条，且都不依赖格式：**(a) 关闭窗口内不出现 `readAt`；(b) 目标文件的内容与长度不变**。跨关闭唯一会失效的 extractor 自有状态是「构造期从 `getSize()` 抄下来的长度」，而它在 10 个格式里都只影响末端截断判定，不影响从原偏移继续读。

| 格式 | 类别 | 关键判据（`path:line`） |
| --- | --- | --- |
| `.wav` | 透明可重开 | `sdk/extractor/wav/WAVExtractor.cpp:257`（init 取 size 后只留偏移）；`sdk/extractor/PcmPacketReader.hpp:54`（逐包 `readAt` 绝对偏移） |
| `.mp3` | 透明可重开 | `sdk/extractor/mp3/MP3Extractor.cpp:387`（init 取 size 后只留偏移）；`sdk/extractor/mp3/MP3FrameReader.cpp:273`（读失败不推进偏移） |
| `.flac` | 透明可重开 | `sdk/extractor/flac/FLACExtractor.cpp:231`（init 取 size 后只留偏移）；`sdk/extractor/flac/FLACFrameReader.cpp:258`（`readAt` 绝对偏移） |
| `.m4a` | 该格式需特例 | `sdk/extractor/m4a/M4APacketReader.cpp:71`（`readAt<=0` → `AVERROR_EOF`）；`sdk/3rdparty/ffmpeg-4.4.4/libavformat/aviobuf.c:570`（AVIO 内部 EOF 置位） |
| `.ogg` | 该格式需特例 | `sdk/extractor/ogg/OGGExtractor.h:24`（`getMetaData()` 恒 `nullptr`、无包工厂）；`sdk/audio/decode/AudioStreamDecoder.cpp:653`（载荷由解码线程内的 `VorbisDecode` 直读数据源） |
| `.aac` | 透明可重开 | `sdk/extractor/aac/AACExtractor.cpp:152`（init 取 size 后只留偏移）；`sdk/extractor/aac/AACFrameReader.cpp:191`（`readAt` 绝对偏移） |
| `.aiff` | 透明可重开 | `sdk/extractor/aiff/AIFFExtractor.cpp:94`（init 取 size 后只留偏移）；`sdk/extractor/aiff/AIFFExtractor.cpp:53`（`PcmPacketReader`） |
| `.asf` | 该格式需特例 | `sdk/extractor/asf/ASFPacketReader.cpp:67`（`readAt<=0` → `AVERROR_EOF`，与 m4a 同形）；`sdk/extractor/asf/ASFPacketReader.cpp:264`（`seekTo` = 关掉重开 demux，既有先例） |
| `.ape` | 该格式需特例 | `sdk/extractor/ape/APEExtractor.cpp:248`（init 的 demux 扫描结束即 `scanner.close()`）；`sdk/extractor/ape/APEPacketReader.cpp:67`（`readAt<=0` → `AVERROR_EOF`） |
| `.mkv` | 该格式需特例 | `sdk/extractor/mkv/MKVExtractor.cpp:162`（init 的 demux 扫描结束即 `scanner.close()`）；`sdk/extractor/mkv/MKVPacketReader.cpp:67`（`readAt<=0` → `AVERROR_EOF`） |

> 「该格式需特例」**不等于**「必须重建」。它表示：复用成立，但复用判据多一条不变量（关闭窗口内零读 ⇒ FFmpeg AVIO 的 EOF 标志）或复用主体不是 extractor（ogg 的载荷读位置在解码器里）。详见 §3。
> 「必须重建 extractor」这一档**为空**，见 §1.1 与 §5 第 1 条。
> 注册表实际有 12 个扩展名键（`ExtractorFactory.cpp:334-348`）：10 个票面格式 + `.wma`（与 `.asf` 共用 `ASFExtractor`，结论同 §3.8）+ `.m3u8`（走 `HlsExtractor`，网络源，按票面边界在图外）。

---

## 1. 四条共通结构事实（分类的依据）

### 1.1 全仓只有一处 `fopen`：`FileSource` 是唯一句柄持有者

**事实**
- `sdk/extractor` 全目录 Grep `fopen|ifstream|std::fstream|::open(|\.open(`，文件句柄的唯一打开点是 `FileSource.cpp:42`（`_wfopen`）与 `FileSource.cpp:56`（`m_file.open`）；其余命中全是 `*PacketReader::open()` / `HlsExtractor::open()` 这类**内存 demux 装配函数**，不碰文件系统。10 个 extractor 与 4 个 `*PacketReader` 都不开文件。
- `DataSourceBase::close()` 是**空实现**，`FileSource` 未 override：`sdk/extractor/DataSource.hpp:148` `virtual void close() {};`。`FileSource.h:11-52` 的成员只有 `m_fp`/`m_file`、`m_offset`、`m_length`、`m_lock`、`m_name`、`m_extensionName`。
- 生产代码**从未调用** `DataSource::close()`：对 `sdk/` + `app/` 全量 Grep `source->close()` / `m_source->close()` / `DataSource.*close()`（排除 3rdparty）零命中；`FileSource`/`HttpSource` 的 `close()` 只在声明与定义处出现。

```cpp
// sdk/extractor/DataSource.hpp:148
    virtual void close() {};
```

**推断**：因此「关闭」在数据源层只可能是 `FileSource` 自己把 `m_fp`/`m_file` 收掉；extractor 侧没有任何 fd 需要跟着收，也就不存在「某格式必须常驻 fd」的候选。这一点直接回答了票面子问题 4。

### 1.2 所有 extractor 只持「裸指针 + 绝对偏移」，运行期不做 `seek` 语义

**事实**
- 10 个 extractor 都保存 `DataSourceBase *m_dataSource` 并在构造期把它交给内部 reader（`WAVExtractor.h:39`、`MP3Extractor.h:24`、`FLACExtractor.h:253`、`OGGExtractor.h:36`、`AACExtractor.h:81`、`AIFFExtractor.h:66`、`APEExtractor.h:53`、`ASFExtractor.h:266`、`MKVExtractor.h:55`、`M4AExtractor.h:53`）。
- `DataSource` 接口没有独立 `seek()`：定位语义由 `readAt(绝对 offset)` 承载（`DataSource.hpp:22-25`，注释逐字「the given offset is equal to, or beyond, the end of the source」）。
- 4 个 FFmpeg `*PacketReader` 的 `AvioData` 只有三个字段 `{DataSourceBase *source; int64_t pos; int64_t size;}`（`M4APacketReader.h:67-71`，另三个同名结构同形），`avioRead` 只做 `source->readAt(ctx->pos, …)` 再 `ctx->pos += n`。

### 1.3 `getSize()` 的调用**全部**落在构造期 / sniff 期，播放期零调用

**事实**：对 `sdk/extractor`（排除 `FileSource.cpp`/`HttpSource.cpp`/`UrlSource*`/`DataSource.hpp`）Grep `getSize`，命中 15 处，逐条核对后**全部**位于构造函数、`init()`、`sniff()` 或 `Resync()`（后者只在构造期被调用）：

| 文件:行 | 所在函数 | 期别 |
| --- | --- | --- |
| `AACExtractor.cpp:36` | `sniff()` | 构造前探测 |
| `AACExtractor.cpp:88` | `init()` | 构造期 |
| `AIFFExtractor.cpp:94` | `init()` | 构造期 |
| `APEExtractor.cpp:164` | `initWithFFmpegDemux()` | 构造期 |
| `ASFExtractor.cpp:136` / `:215` | `init()` 兜底 / `initWithFFmpegDemux()` | 构造期 |
| `FLACExtractor.cpp:232` | `init()` | 构造期 |
| `M4AExtractor.cpp:104` | `initWithFFmpegDemux()` | 构造期 |
| `MKVExtractor.cpp:105` | `initWithFFmpegDemux()` | 构造期 |
| `MP3Extractor.cpp:94` / `:328` / `:388` | `Resync()` / 构造函数 | 构造期 |
| `OGGExtractor.cpp:131` | `init()` | 构造期 |
| `WAVExtractor.cpp:258` | `init()` | 构造期 |
| `ID3.cpp:988` | `parseV1()`（`MP3Extractor` 以 `ignoreV1=true` 构造，不走） | 构造期 |

**推断**：播放期不会再问数据源「你多长」。所以「重开后 `getSize()` 变了」不会被任何一包读到；它只以「构造期抄下的那份长度」参与偏移 clamp。

### 1.4 demux 面一律在**播放期**、且在**解码线程内**创建

**事实**
- `AudioStreamDecoder.cpp:566-569` 注释逐字：「11 种格式全部有包工厂（#66 contract），逐包 pull（增量）；……demux 句柄（FLACDecode + 包游标）在这一行之前是不存在的，创建发生在解码线程里、随本任务退出释放（规格 4.3 所有权清单）」。
- 各 extractor 的 reader 都是懒建：`WAVExtractor.cpp:70-79`、`AIFFExtractor.cpp:48-57`、`MP3Extractor.cpp:410-418`、`AACExtractor.cpp:177-185`、`FLACExtractor.cpp:41-50`、`APEExtractor.cpp:106-115`、`ASFExtractor.cpp:301-310`、`MKVExtractor.cpp:47-56`、`M4AExtractor.cpp:46-55`。
- FFmpeg 四家在**构造期只开一次扫描用 demux，并在 init 末尾关掉**：`APEExtractor.cpp:248`、`ASFExtractor.cpp:270`、`MKVExtractor.cpp:162`、`M4AExtractor.cpp:158`，四处注释一致：「demux 到此为止：播放期会按需重开一个（规格 4.3 所有权清单）」。

```cpp
// sdk/extractor/ape/APEExtractor.cpp:246-248
    // demux 到此为止：播放期会按需重开一个（规格 4.3 所有权清单）。
    scanner.close();
```

**推断**：主干方案「入列探测 → 关句柄 → 播放前重开」的关闭窗口正好落在这段「init 已结束、reader 尚未创建」的空档里。此时**没有任何 demux / AVIO / libFLAC / libvorbisfile 对象存在**，所以子问题 3 里点名要防的「demux 位置」「已解析的索引」不可能跨关闭泄漏——它们还没被创建。

### 1.5 复用主体的生命周期已由现网代码保证

**事实**
- 条目持有 `std::shared_ptr<DataSource> source`（`sdk/MusicPlayList.h:83`），同一份 `shared_ptr` 也进 `PlaybackTask::owner`（`MusicPlayList.h:109-131`，注释写明「extractor 的保活引用（#268 实证的 UAF）」）。
- 创建点唯一：`MusicPlayList.cpp:593` `source.reset(new FileSource(musicPath.c_str()));`；extractor 用同一裸指针构造并存入条目：`MusicPlayList.cpp:617-618`、`:636`。
- 解码器只拿裸指针：`AudioStreamDecoder.cpp:145-148` `DataSourceBase *ds = m_extractor->getDataSource(); ds->setAbortFlag(&m_abortDecode);`。

**推断**：`FileSource` 对象与 extractor 一样活到条目退役 join 之后，因此「同一个对象、同一路径」的重开在所有权上是现成成立的，不需要新引用计数。

---

## 2. 跨关闭会失效的三处状态（逐条给证据）

### 2.1 `m_length` 是构造期一次性的，重开不会重算

**事实**
- 构造期取长度并立即把文件位置复位：`FileSource.cpp:45-50`（Windows `_fseeki64(SEEK_END)` + `_ftelli64` + `_fseeki64(SEEK_SET)`）与 `:58-62`（POSIX `seekg`/`tellg`/`seekg`）。`m_length` 初值 -1（`:26`）。
- `readAt` 用 `m_length` 做两件事：`offset >= m_length` 直接返回 0；否则把 `size` 裁到 `m_length - offset`（`FileSource.cpp:109-117`）。
- `getSize` 直接回填缓存值：`FileSource.cpp:157-168`。

```cpp
// sdk/extractor/FileSource.cpp:109-117
    if (m_length >= 0) {
        if (offset >= m_length) {
            return 0; // read beyond EOF
        }
        uint64_t numAvailable = m_length - offset;
        if ((uint64_t)size > numAvailable) {
            size = numAvailable;
        }
    }
```

**重开后文件变长的后果**：`offset >= m_length` 仍按旧长度判 0，`getSize()` 仍回旧值 ⇒ 追加部分**永远读不到**，且不报错（FFmpeg 侧 `AVSEEK_SIZE`/`SEEK_END` 也按旧 `ctx->size` clamp）。extractor 构造期抄下的长度（§1.3 表）同样不变，二者一致地错，表现为「文件尾部当不存在」。

**重开后文件变短的后果**（平台不同，证据不同）：
- Windows：`std::fread` 短读且 `ferror==0` ⇒ 返回**实际读到的字节数**（`FileSource.cpp:132-138`），上层按短读继续；`PcmPacketReader.hpp:60-66` 有专门的短读分支「init 之后文件变短：只交出真正读到的那些字节」。
- POSIX：`m_file.read()` 短读会置失败位，`!m_file.good()` ⇒ 返回 `FAILED_TRANSACTION`（负值，`FileSource.cpp:146-151`），上层看到的是「读失败」而不是短读。

**推断**：两条路径都没崩、没越界，但会分别表现为「少读」与「读失败」，且不会自动重新取长度。

### 2.2 关闭期间读一次的代价：原生 reader 可重试，FFmpeg 侧置 EOF 标志

**事实（`FileSource` 关闭态返回值）**：`FileSource::readAt` 第一行 `if (!isOpen()) return NO_INIT;`（`FileSource.cpp:104-106`），`NO_INIT = -ENODEV`（`sdk/utils/ErrorUtils.h:28`），即**负值**；`getSize` 关闭态返回 `NO_INIT`（`:161-163`）。注意 `isOpen()` 判断在 `std::lock_guard` **之前**（`:104` 早于 `:107`），`m_fp`/`m_file` 也不是原子量——关闭与读并发时该检查本身不可靠。

**事实（原生 reader：失败不推进偏移，可重试）**
- `PcmPacketReader.hpp:54-59`：`got <= 0` 直接 `return false`，`m_nextOffset` 只在 `:67` 前进。
- `MP3FrameReader.cpp:273-278`：`got <= 0` 时 `m_buf.resize(loaded)` 回滚并 `return false`，`m_nextOffset` 不动（只在 `emit()` 里 `:243` 前进）。
- `AACFrameReader.cpp:191`、`FLACFrameReader.cpp:258` 同形：失败即回滚缓冲，偏移不动。

**事实（FFmpeg 四家：负返回被译成 `AVERROR_EOF`，AVIO 层会置位 `eof_reached`）**
- `M4APacketReader.cpp:71-76`（APE/ASF/MKV 同名函数同形）：

```cpp
    ssize_t n = ctx->source->readAt(ctx->pos, buf, static_cast<size_t>(buf_size));
    if (n <= 0) {
        return AVERROR_EOF;
    }
    ctx->pos += n;
```

- 库侧一手：`aviobuf.c:570-576`，`len == AVERROR_EOF` ⇒ `s->eof_reached = 1`，且注释明确「do not modify buffer if EOF reached so that a seek back can be done without rereading data」。
- 清除位只有两条路：`avio_seek` 末尾 `s->eof_reached = 0;`（`aviobuf.c:333`）；`avio_feof` 会「清位 + 重新 `fill_buffer`」（`aviobuf.c:365-373`）。

**推断**：同样是「关闭窗口内被读一次」，原生 reader 只是这一次 pull 失败、下一次从同一偏移重试；FFmpeg 四家会在 AVIO 里留下 EOF 状态，能否自愈取决于 demuxer 之后是否调 `avio_feof`/`avio_seek`。这是把 m4a/asf/ape/mkv 单列为「需特例」的唯一理由——不是「不可重开」，而是「重开判据多一条零读不变量」。

### 2.3 关闭必须晚于解码线程 join：`readAt` 就在解码线程里

**事实**
- 取消信号注入点在 `start()` 里：`AudioStreamDecoder.cpp:144-149`，`ds->setAbortFlag(&m_abortDecode); m_abortSource = ds;`（`ds` 就是 `m_extractor->getDataSource()`）。
- 注入后立刻起解码线程：`AudioStreamDecoder.cpp:214-217` `m_thread = std::thread(&AudioStreamDecoder::threadFunc, this);`。
- 解码线程逐包 pull：`AudioStreamDecoder.cpp:765` / `:1120` `cursor.nextPacket(packet)` ⇒ `ExtractorHelper::packetAt()` ⇒ 各 reader 的 `readAt`。FLAC / Vorbis 还有自己的 native 读回调：`FLACDecode.cpp:195`（`m_dataSource->readAt(...)`）、`VorbisDecode.cpp:58`（`self->m_dataSourcePtr->readAt(...)`）。
- 停止顺序是先 join 再解绑：`AudioStreamDecoder.cpp:236-247`：

```cpp
void AudioStreamDecoder::stop()
{
    requestStop();
    if (m_thread.joinable()) {
        m_thread.join();
    }
    m_threadFinished.store(true, std::memory_order_release);
    // P1：解码线程已退出，解绑裸指针，避免 decoder 析构后数据源持有悬垂 flag。
    if (m_abortSource != nullptr) {
        m_abortSource->setAbortFlag(nullptr);
        m_abortSource = nullptr;
    }
```

- `requestStop()` 同时置 `m_stopRequest` 与 `m_abortDecode`（`:230-234`），而 `m_abortDecode` 正是 `AbortInterrupt.hpp:8-11` 的 `sbAbortRequested` 读的那个位——`avioRead` 与 `interrupt_callback` 都查它（`AbortInterrupt.hpp:13-18`、`M4APacketReader.cpp:68-70`、`:154-155`）。

**推断**：`readAt` 的调用者有四类，全部在解码线程里（包游标、libFLAC 读回调、libvorbisfile 读回调、FFmpeg AVIO 回调）。因此关闭窗口必须晚于 `stop()` 的 `join()`；且因为 §2.2 的「关闭态读 = `NO_INIT`」，早关不会崩，只会把一次读变成 EOF/失败——这正是最容易漏测的时序坑。

---

## 3. 逐格式

> 每节固定四问：构造期缓存什么 / 运行期是否只走 `readAt` / 跨关闭有没有自有状态失效 / 是否常驻 fd。
> 「透明可重开」= 上表 A 档；「需特例」= C 档，复用成立但多一条不变量或复用主体不在 extractor。

### 3.1 `.wav` — **透明可重开**

- **构造期缓存**：`DataSourceBase*`（`WAVExtractor.h:39`）、`m_waveFormat`、`m_dataOffset`、`m_dataSize`、`m_audioSpec`、`m_dataTruncated`。其中 `m_dataSize` 是 **`getSize()` 现算的**：声明长度与「文件剩余」取小（`WAVExtractor.cpp:257-267`）。
- **运行期访问**：只经 `PcmPacketReader::nextPacket` → `readAt(m_dataOffset + m_nextOffset, …)`（`PcmPacketReader.hpp:54-55`），切片长度常量对齐到帧（`:74-83`）。没有第二路 fd、没有 demux 对象、没有 `seek` 语义。
- **跨关闭**：自有状态只有一个 `m_nextOffset`（`PcmPacketReader.hpp:88`，绝对偏移的下一个包起点），重开后从原偏移继续等价。失效项只有 §2.1 的 `m_dataSize` 快照（文件变长/变短时末端判定不更新）与 `m_dataTruncated`。
- **常驻 fd**：无。

```cpp
// sdk/extractor/PcmPacketReader.hpp:53-56
        // readAt 是 master 既有 off64_t 边界：偏移从新增的 int64_t 成员显式转回既有契约。
        const ssize_t got = m_source->readAt(
            static_cast<off64_t>(m_dataOffset + m_nextOffset), block->data(), want);
```

### 3.2 `.mp3` — **透明可重开**

- **构造期缓存**：`DataSourceBase*`、`m_firstFramePos`、`m_fixedHeader`、`m_dataSize`、`m_audioEnd`（去掉 ID3v1 尾标后的音频末端）。`m_dataSize` 来自 `getSize()`（`MP3Extractor.cpp:387-390`）；`m_audioEnd` 由「尾部 128 字节是否 `TAG`」决定（`:391-404`）。`Resync()` 本身也是纯 `readAt` 扫描（`:59-230`）。
- **运行期访问**：`MP3FrameReader` 只持 `m_source` + `m_audioStart` + `m_audioEnd` + `m_nextOffset` + 一块 `m_buf`（`MP3FrameReader.cpp:115-131`）；`appendChunk` 用 `readAt(m_nextOffset + loaded, …)`（`:273-274`）。
- **跨关闭**：唯一跨关闭的状态是 `m_buf` 里尚未消费的原始文件字节（最多 `kMaxScanBytes`）与 `m_nextOffset`。二者都是「文件某个绝对区间的内容副本」，文件不变即仍有效。读失败不推进偏移（`:273-278`），重开后同一偏移可重试。
- **常驻 fd**：无。`ID3` 在构造期是局部对象并已把 tag 拷进自己的 `mData`（`ID3.h:60-66`），不跨构造存活。
- **注意（非阻断）**：`m_dataSize` / `m_audioEnd` 是快照；文件变长时尾部新增帧读不到，变短时由短读自然收敛。

### 3.3 `.flac` — **透明可重开**

- **构造期缓存**：`DataSourceBase*`、`m_audioDataOffset`（最后一个 metadata block 之后的偏移）、`m_dataSize`（= 整份 `fileSize`，`FLACExtractor.cpp:231-233`）、`m_maxFrameBytes`（只用来定扫描缓冲上限）。
- **运行期访问**：`FLACFrameReader` 按 `readAt(m_nextOffset + loaded, …)` 拉字节（`FLACFrameReader.cpp:258-259`），逐帧现场重算边界；`seekPacketByte()` 只改 `m_nextOffset` + 清缓冲（`FLACExtractor.cpp:77-89`）。解码侧走包游标，不直接碰数据源（`AudioStreamDecoder.cpp:571-575`）。
- **跨关闭**：无 demux 对象、无索引、无 EOF 标志；`m_buf` 与 `m_nextOffset` 同 mp3 口径。`m_dataSize` 快照同 §2.1。
- **常驻 fd**：无。libFLAC 的 native RA 分支（`FLACDecode::read_callback` 的 `m_dataSource` 分支，`FLACDecode.cpp:183-205`）在生产链只在 `packetCount()==0` 时启用（`AudioStreamDecoder.cpp:576`），10 个格式都非 0 包，故不走。

### 3.4 `.m4a` — **该格式需特例**

- **构造期缓存**：`m_fileSize`（`M4AExtractor.cpp:104`）与 `m_audioSpec` / extradata；init 用局部 `M4APacketReader scanner` 做只读扫描，结尾 `scanner.close()`（`:158`）。**构造结束时没有任何 demux 存活**。
- **运行期访问**：`M4APacketReader` 持 `AvioData{source, pos, size}`（`M4APacketReader.h:67-71`，构造期初值见 `.cpp:47-49`）、一块 64 KB AVIO 缓冲（`:18`、`:128`）与 `AVFormatContext`；`avioRead` 只 `readAt(ctx->pos, …)`（`:71`）；`avioSeek` 的 `AVSEEK_SIZE` 返回构造期 `ctx->size`（`:88-90`），`SEEK_END` 也按它算（`:100-102`）。
- **跨关闭**：`ctx->pos` 是绝对文件偏移，本身不失效；但 `readAt<=0 → AVERROR_EOF`（`:72-74`）会在 AVIO 里置 `eof_reached`（`aviobuf.c:570-576`）。**只要关闭窗口内没有读**，重开后从 `ctx->pos` 继续与不关闭逐字节等价。另一处失效项是 `ctx->size` 快照（文件变长后 `SEEK_END`/`AVSEEK_SIZE` 仍报旧长度）。
- **常驻 fd**：无。但**常驻一个 demux 句柄（含 64 KB 缓冲）**，这是它不同于原生五家的地方。
- **既有先例**：`seekTo()` 就是「关掉整个 demux 再重开」（`M4APacketReader.cpp:277-294`），且序列起点 `packetAt(0)` 必走它（`M4AExtractor.cpp:66-70`），所以「序列起点 = 重新 open」这条路径现网已经在跑。

### 3.5 `.ogg` — **该格式需特例**

- **构造期缓存**：`DataSourceBase*`、`m_audioCodecID`、`m_spec`、`m_dataSize`（= `fileSize`，`OGGExtractor.cpp:131-134`）。init 是**整份文件逐页只读扫描**（`:148-193`），只为拿首包 codec 签名与末页 granule；页表与页负载都不留（注释 `:136-140`）。
- **运行期访问**：**extractor 没有包工厂**——`OGGExtractor` 只 override `getMetaData()` 返回 `nullptr`（`OGGExtractor.h:23-24`），`packetCount()` 走基类默认（`ExtractorHelper.hpp:88-95` 把 `nullptr` 读成 0 包）。载荷由解码线程内的 `VorbisDecode` 用 libvorbisfile 直读数据源：`AudioStreamDecoder.cpp:653-668` 取 `m_extractor->getDataSource()` 后 `initVFFromSource(ds, m_extractor->getDataSize())`。
- **跨关闭**：extractor 自身无失效项（只有 `m_dataSize` 快照）。真正的读位置 `m_pos` / `m_size` 在 `VorbisDecode` 里（`VorbisDecode.cpp:50-62` 的 `readCallback`、`:81-110` 的 `seekCallback`、`:120-127` 的 `tellCallback`），而 `VorbisDecode` 是**每次起播时在解码线程里新建的局部对象**（`AudioStreamDecoder.cpp:654`）。因此复用判据必须写成「重开先于 `VorbisDecode` 构造」，**不能只看 extractor 是否存活**——这是与其余九种最本质的区别。
- **常驻 fd**：无。

```cpp
// sdk/audio/decode/AudioStreamDecoder.cpp:653-658
    if (m_codecID == AUDIO_CODEC_ID_VORBIS) {
        VorbisDecode vorbis(this);
        vorbis.setAbortFlag(&m_abortDecode);
        DataSourceBase *ds = m_extractor->getDataSource();
        if (ds != nullptr) {
            if (!vorbis.initVFFromSource(ds, m_extractor->getDataSize())) {
```

### 3.6 `.aac` — **透明可重开**

- **构造期缓存**：`DataSourceBase*`、`m_header`（首个 ADTS 头）、`m_spec`、`m_dataSize`（= `fileSize`）、`m_audioStart`（ID3v2 前缀之后，`AACExtractor.cpp:149-154`）。`scanFrameCount()` 只在 init 里数帧拿时长（`:160`），不留帧表。
- **运行期访问**：`AACFrameReader` 持 `m_source` / `m_audioStart` / `m_audioEnd` / `m_nextOffset` / `m_buf`（`AACFrameReader.h:52-57`），读点 `AACFrameReader.cpp:191-192`。
- **跨关闭**：同 mp3 口径（`m_buf` + 绝对偏移，失败不推进，`AACFrameReader.cpp:168-196`）。`m_dataSize`/`m_audioStart` 是快照。
- **常驻 fd**：无。

### 3.7 `.aiff` — **透明可重开**

- **构造期缓存**：`m_dataOffset`（SSND 负载起点）、`m_dataSize`（声明长度与文件剩余取小，`AIFFExtractor.cpp:169-178`）、`m_audioSpec`、`m_dataTruncated`；`fileSize` 来自 `getSize()`（`:92-94`）。
- **运行期访问**：与 wav 完全同构，复用 `PcmPacketReader`（`AIFFExtractor.cpp:53-54`），逐包 `readAt` 后做大端转换（`:79-82`、`:225-245`）。
- **跨关闭**：同 wav。`m_dataSize` 快照同 §2.1。
- **常驻 fd**：无。

### 3.8 `.asf` — **该格式需特例**

- **构造期缓存**：先试 `initWithFFmpegDemux()`（`ASFExtractor.cpp:129`），成功后缓存 `m_fileSize`、spec、extradata、`m_audioPacketCount`，并在结尾 `scanner.close()`（`:270`）；失败才走手写 ASF 对象扫描兜底（`:134-203`，纯 `readAt`）。
- **运行期访问**：`ASFPacketReader` 与 m4a 同构（`AvioData` + `AVIOContext` + `AVFormatContext`），读点同名 `avioRead`；`seekTo` = `close()` + `open()`（`ASFPacketReader.cpp:264-281`）。
- **跨关闭**：同 m4a：`ctx->pos` 绝对偏移可续，`AVSEEK_SIZE` 用构造期 `size`，关闭窗口内读一次会置 `eof_reached`。ASF 的尾部 Simple Index Object 由 ffmpeg demux 内部按需读，自仓不留（`ASFPacketReader.h:16-20` 注释）。
- **常驻 fd**：无；常驻一个 demux 句柄。

### 3.9 `.ape` — **该格式需特例**

- **构造期缓存**：`m_fileSize`（`APEExtractor.cpp:164-169`）、spec / `m_bitRate` / `m_blockAlign` / extradata / `m_audioPacketCount`；init 用局部 `scanner` 逐包走完整份文件，`scanner.close()`（`:248`）后 demux 归零。spec 不全时再走手写 `parseApeHeader()`（`:196`，纯 `readAt`）。
- **运行期访问**：`APEPacketReader` 同构，`avioRead` / `avioSeek` / `AVSEEK_SIZE` 与 m4a 逐字一致；`seekTo` = `close()` + `open()`（`APEPacketReader.cpp:264-281`）。
- **跨关闭**：同 m4a。
- **常驻 fd**：无；常驻一个 demux 句柄。

### 3.10 `.mkv` — **该格式需特例**

- **构造期缓存**：`m_fileSize`（`MKVExtractor.cpp:105`）、spec / `m_bitRate` / `m_blockAlign` / extradata / `m_audioPacketCount`；init 的 `scanner.close()` 在 `:162`。
- **运行期访问**：`MKVPacketReader` 同构；`seekTo` = `close()` + `open()`（`MKVPacketReader.cpp:264-281`）。Matroska 的 cluster/block 边界靠 demux 现场解析，不建 seek 表（`MKVPacketReader.h:11-18` 注释）。
- **跨关闭**：同 m4a。
- **常驻 fd**：无；常驻一个 demux 句柄。

---

## 4. 附节：票面点名的四项补充核查

### 4.1 `FileSource.h` 在公开头目录之外；ABI 版本仍为 2

**事实**
- 公开头目录是 `sdk/include/soundbridge/`，只有 5 个头：`error.h`、`logging.h`、`player.h`、`sdk.h`、`version.h`。
- `FileSource.h` 唯一实体在 `sdk/extractor/FileSource.h`；全仓 `find -name FileSource.h` 只此一处（`build/_agent/*` 与测试代码引用它，属仓库内私有面）。`FileSource` 没出现在任何 `sdk/include/` 头里。
- ABI 宏定义在 `sdk/include/soundbridge/version.h:9`，当前值 **`SOUNDBRIDGE_ABI_VERSION 2`**；同文件 `:7-8` 注释「increment only when binary compatibility is broken」，并注明 2 的来源是地图 #235（`TrackInfo` 加 `id`、`Player` 加音量四方法）。

**推断**：给 `FileSource` 增删 `open()/close()` 不触及任何已安装头，不构成公开 ABI 变更；`SOUNDBRIDGE_ABI_VERSION` 不必因此 +1。此结论与地图 #273「`SOUNDBRIDGE_ABI_VERSION` 保持 2」的硬约束一致。

### 4.2 `HttpSource::close()` **不能**作为「close 后可继续用」的先例

**事实**
- `close()` 只做一件事：`m_initCheck = NO_INIT;`（`HttpSource.cpp:682-686`），没有重开路径（`HttpSource.cpp` 内 `reopen` 零命中；`probe()` 只在构造期调用一次，`:631-635`）。
- close 后 `readAt` 立即返回 **`-1`**（`HttpSource.cpp:654` `if (m_initCheck != OK) return -1;`），而不是 0——与 `readAt` 契约里「0 = 到达末尾、负 = 失败」相比，这是「失败」语义。
- close 后 `getSize` 返回 `NO_INIT`（`:677`）。

**推断**：`HttpSource::close()` 是**单向终止**，只证明了「close 可以被实现成一个可用性标志、已持有该对象的调用方不会崩」，**不证明**「close → 重开 → 既有对象继续用」。把它当先例会得出错误结论：两个源在 close 后 `readAt` 的返回语义不同（`FileSource` 关态是 `NO_INIT` 负值；`HttpSource` 是 `-1`，二者都不是 0，因此都应被上层当作失败而非 EOF —— 但 FFmpeg 的 `avioRead` 把 `<=0` 一律译作 `AVERROR_EOF`，见 §2.2）。

### 4.3 `m_length` 一次性求值的后果

见 §2.1，此处只补一条量化事实：`m_length` 在**析构前没有任何写入点**——`FileSource.cpp` 全文对 `m_length` 的赋值只有构造期 `:47`（Windows）与 `:60`（POSIX）两处，其余全是读（`:109`、`:113`、`:165`）。因此重开文件不会刷新它，`getSize()` 在重开后仍返回旧值。

### 4.4 解码线程侧的读证据：关闭窗口必须晚于 join

见 §2.3。三条独立证据：
1. 取消信号在 `start()` 里注入（`AudioStreamDecoder.cpp:144-149`），解码线程紧接着创建（`:214-217`）；
2. 解码线程是 `readAt` 的唯一调用者（包游标 `:765` / `:1120`；libFLAC `FLACDecode.cpp:195`；libvorbisfile `VorbisDecode.cpp:58`；FFmpeg AVIO `M4APacketReader.cpp:71` 等）；
3. `stop()` 的 `join()` 在解绑之前（`:236-247`），构成现成的「读已停」栅栏。

---

## 5. 与预期不符之处 / 未核实

1. **没有任何格式「必须重建 extractor」**（预期可能有）。原因不是「重开很聪明」，而是 §1.4：demux 面一律在播放期懒建，关闭窗口里根本不存在 demux/AVIO/解码器对象。唯一跨关闭的 extractor 自有状态是构造期抄下的长度，而它在播放期零消费。
2. **「必须常驻 fd」的候选为空**（票面子问题 4）。全仓只有 `FileSource` 开文件（§1.1）；4 个 FFmpeg 格式常驻的是 demux 句柄与 64 KB AVIO 缓冲，不是 fd。
3. **`ogg` 的复用判据不在 extractor 上**。`OGGExtractor` 的 `packetCount()` 恒 0、`getMetaData()` 恒 `nullptr`，载荷由 `VorbisDecode` 直读数据源。任何「只检查 extractor 是否可用」的判据在 ogg 上会漏掉真正的读位置（`VorbisDecode::m_pos`）。
4. **FFmpeg 侧的 EOF 不是永久锁存**。`aviobuf.c:365-373` 的 `avio_feof` 会清 `eof_reached` 并重新 `fill_buffer`，`avio_seek` 也会清（`:333`）。所以准确表述是「置位后能否自愈取决于 demuxer 是否再探测」，而不是「一次误读永久废掉 demux」。
5. **`FileSource::readAt` 的关闭态检查在锁外**（`FileSource.cpp:104-107`），`m_fp`/`m_file` 非原子。这不能证明「close 与读并发是安全的」，只证明「关窗必须靠外部时序保证（join）」。
6. **无关重开的观察（不在本票范围）**：`FLACExtractor.cpp:232` 用 `static_cast<DataSource *>(m_dataSource)->getSize(...)` 把 `DataSourceBase*` 向下转型到 `DataSource*`，只对 `DataSource` 子类成立。本地链上该参数是 `FileSource`（`MusicPlayList.cpp:593`），故当前不构成实际缺陷；是否存在可达的非 `DataSource` 输入未核。
7. **未核实**：`getSize` 命中表（§1.3）是按「函数名 + 所在函数」人工归类，未逐条走调用图证明 `Resync()` 只在构造期被调用（已核 `MP3Extractor.cpp` 内两处调用点 `:252`、`:286` 均在构造函数体内）。另：`sdk/3rdparty` 内除 `aviobuf.c` 外未做其它库内行为核对。
