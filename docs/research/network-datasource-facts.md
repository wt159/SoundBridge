# 事实核对基线：DataSource、自定义 AVIO 与提取器链路的网络接入现状

> 取证票：GitHub issue #119（`wt159/SoundBridge`）。
> 取证方式：**只读源码/构建脚本核查**，遵循 `research` 技能的一手来源优先原则：事实与推断分开、每条事实附「路径 + 符号名」、查不到的写「未核实 + 已查范围」。
> 行号仅为辅助（`docs/research/network-audio-sources-survey.md` 已声明行号会漂移）；引用以路径 + 符号为准。
> 本次**未修改任何生产代码 / 用例 / CI / 规格**，**未执行构建或测试**，**未做任何 git 操作**（分支由编排层统一处理）。
> Grep 一律限定 `path` 到 `sdk/` 的子目录或具体文件，并对跨目录查询用 `glob: "!3rdparty/**"` 排除 `sdk/3rdparty`；第 6 项所需的一手构建证据为**定点读取** `sdk/3rdparty` 内的 `build.sh` / `configure` / `config.h` 等具体文件，不是全目录扫描。
> 背景快照 `docs/research/network-audio-sources-survey.md` §1 与 `AGENTS.md` NOTES 仅作线索；下文所有「现状」均回源码/构建脚本/配置产物核对，不以快照或 `doc/` 规格为据。

---

## 1. `DataSource` 接口签名与 `FileSource` 实现假设

### 1.1 接口确切签名（`sdk/extractor/DataSource.hpp`）

**事实**
- `class DataSourceBase`（`DataSource.hpp:7`）与 `class DataSource : public DataSourceBase`（`DataSource.hpp:150`）。工厂/提取器侧实际接收的是 `DataSourceBase *`（`ExtractorFactory.h`、`ExtractorHelper.hpp`）。
- `virtual ssize_t readAt(off64_t offset, void *data, size_t size) = 0`（`DataSource.hpp:24`）。头文件注释逐字：「Returns the number of bytes read, or -1 on failure. It's not an error if this returns zero; it just means the given offset is equal to, or beyond, the end of the source.」即**按绝对字节偏移随机读**，返回 0 表示 offset 到达/越过末尾，-1 表示失败。
- `virtual sdk_utils::status_t getSize(off64_t *size)`（`DataSource.hpp:125-129`）：基类默认实现是 `*size = 0; return sdk_utils::UNKNOWN_ERROR;`，即「长度未知」是基类默认语义，并非纯虚。
- **接口没有独立的 `seek()` 方法**。随机访问/定位语义由 `readAt(offset, ...)` 的绝对 `offset` 承载；另有 `virtual sdk_utils::status_t reconnectAtOffset(off64_t offset)`（`DataSource.hpp:158-161`，默认返回 `sdk_utils::ERROR_UNSUPPORTED`）、`getAvailableSize(off64_t, off64_t *)`（默认返回 -1）、`flags()`、`getUri()`、`close()`、`initCheck()`。
- `DataSourceBase::Flags` 枚举里预留了 `kIsHTTPBasedSource = 8`、`kIsLocalFileSource = 16`、`kStreamedFromLocalHost = 2`、`kIsCachingDataSource = 4`、`kWantsPrefetching = 1`（`DataSource.hpp:9-15`）；但生产子类只用到 `kIsLocalFileSource`。
- 便捷读取器 `getUInt16/24/32/64` 与 `getUInt*Var`（`DataSource.hpp:27-123`）全部基于 `readAt`，不做流式语义。

### 1.2 唯一生产子类 `FileSource`（`sdk/extractor/FileSource.h` / `.cpp`）

**事实**
- 唯一继承 `DataSource` 的生产类：`class FileSource : public DataSource, public NonCopyable`（`FileSource.h:11`）；构造签名 `FileSource(const char *filename)`（`FileSource.h:13`），**入参是本地文件路径字符串**。
- 生产链的构造点：`sdk/MusicPlayList.cpp` 的 `MusicPlayList::_addMusic()` — `std::shared_ptr<FileSource> source(new FileSource(musicPath.c_str()))`（`MusicPlayList.cpp:258`），随后 `if (source->initCheck() != OK)` 报 `ErrorCode::FileOpenFailed` 并 `return`（`MusicPlayList.cpp:264-268`）。
- 打开方式（`FileSource.cpp:11-59`）：Windows 用 `_wfopen(wide.c_str(), L"rb")` + `sdk_utils::toWideString`（`FileSource.cpp:32-34`，与 `AGENTS.md` NOTES 一致）；非 `_WIN32` 用 `std::ifstream::open(filename, std::ios::in | std::ios::binary)`（`FileSource.cpp:48`）。
- **「已知长度」假设**：构造时立即 `_fseeki64(fp, 0, SEEK_END)` + `_ftelli64`（`FileSource.cpp:38-40`）或 `seekg(0, end)` + `tellg`（`:51-53`）把长度缓存进 `int64_t m_length`；`m_length` 初值 -1（`:18`）。
- `readAt(off64_t offset, void *data, size_t size)`（`FileSource.cpp:89-135`）：`std::lock_guard` 加锁；`offset >= m_length` 返回 0（`:96-99`）；把 `size` 裁剪到 `m_length - offset`（`:100-103`）；每次读都 `_fseeki64(fp, offset + m_offset, SEEK_SET)` / `seekg(offset + m_offset)` 再 `fread`/`read`（`:107-118` / `:120-133`）。`m_offset` 构造为 0（`:17`）。
- `getSize(off64_t *size)`（`FileSource.cpp:137-148`）：**直接返回缓存的 `m_length` 并返回 `OK`**，是 O(1)，不是全量扫描。
- `flags()` 返回 `kIsLocalFileSource`（`FileSource.h:23`）；`toString()` 返回 `m_name`（`FileSource.h:25`）；私有 `getExtensionName()` 用 `filename.rfind('.')` 取最后一个点之后的子串（`FileSource.cpp:150-157`），但该成员 `m_extensionName` 不参与工厂选择（见 §3）。
- 未 override 的方法沿用基类默认：`reconnectAtOffset` → `ERROR_UNSUPPORTED`；`getAvailableSize` → -1；`getUri` → 空串。

**事实（子类全景，需与票面「唯一生产子类」措辞区分）**
- `DataSource` 的生产子类确实只有 `FileSource`。但 `DataSourceBase` 另有一个生产内的内存实现：`struct MemorySource : public DataSourceBase`（`sdk/extractor/mp3/ID3.cpp:20-42`），是 `ID3.cpp` 内 file-local 的定长内存包装（构造 `MemorySource(const uint8_t *, size_t)`、`readAt` 用 `memcpy`），仅用于 ID3 元数据解析，**不是 `DataSource` 子类、不是 `ExtractorFactory` 的输入、不在播放主链**。
- 测试替身（非生产）：`sdk/test/unit/test_extractor.cpp` 的 `MockDataSource`、`TestFactoryDataSource`、`MinimalDataSource` 继承 `DataSource`（`test_extractor.cpp:29, 311, 421, 445`）。

---

## 2. 自定义 `AVIOContext` 回调 / `AVFMT_FLAG_CUSTOM_IO` / `avformat_open_input(&fmt, nullptr, ...)`

**事实（调用点：4 个 PacketReader，实现几乎逐字相同）**
- 装配点在 `sdk/extractor/{ape, mkv, asf, m4a}/*PacketReader.cpp`，各文件同名符号 `avioRead` / `avioSeek` / `open()` / `close()` / `nextPacket()` / `seekTo()`：
  - `sdk/extractor/ape/APEPacketReader.cpp`、`sdk/extractor/mkv/MKVPacketReader.cpp`、`sdk/extractor/asf/ASFPacketReader.cpp`（三者的行号一致：`avioSeek` 内 `AVSEEK_SIZE` 在 `:78`、`avio_alloc_context` 在 `:124`、`AVFMT_FLAG_CUSTOM_IO` 在 `:140`、`avformat_open_input` 在 `:142`、`avformat_seek_file` 在 `:264`）。
  - `sdk/extractor/m4a/M4APacketReader.cpp`（`AVSEEK_SIZE` 在 `:82`、`avio_alloc_context` 在 `:128`、`AVFMT_FLAG_CUSTOM_IO` 在 `:144`、`avformat_open_input` 在 `:146`、`avformat_seek_file` 在 `:277`）；`kIoBufferSize = 64 * 1024`（`:16-18`），其余三者同量级。
- 以 `M4APacketReader` 为代表（已完整核对）：
  - `AvioData`（`M4APacketReader.h:67-71`）只有三个字段：`DataSourceBase *source; int64_t pos; int64_t size;`。`size` 来自构造参数 `fileSize`（`M4APacketReader.cpp:33-49`）。
  - `avioRead(void *opaque, uint8_t *buf, int buf_size)`（`M4APacketReader.cpp:56-71`）：`ssize_t n = ctx->source->readAt(ctx->pos, buf, buf_size)`；`if (n <= 0) return AVERROR_EOF;`（`:66-67`，即把 `readAt` 返回 0 直接当 EOF）；`ctx->pos += n`。
  - `avioSeek(void *opaque, int64_t offset, int whence)`（`M4APacketReader.cpp:73-109`）：`if (whence == AVSEEK_SIZE) return ctx->size;`（`:82-84`）；`switch (whence & ~AVSEEK_FORCE)`（`:87`），`SEEK_SET/SEEK_CUR/SEEK_END` 都基于 `ctx->pos`/`ctx->size`，并把 `newPos` clamp 到 `[0, ctx->size]`（`:101-108`）。
  - `open()`（`M4APacketReader.cpp:111-177`）：入口 `if (m_ioData.source == nullptr || m_ioData.size <= 0) return false;`（`:116-118`，即**必须有正的长度**才能装配）；`av_malloc(kIoBufferSize)` → `avio_alloc_context(ioBuffer, kIoBufferSize, 0, &m_ioData, &M4APacketReader::avioRead, nullptr, &M4APacketReader::avioSeek)`（`:122-130`，**write_flag=0、seek 回调非空**）；`m_fmt = avformat_alloc_context()`（`:137`）；`m_fmt->pb = m_avio`（`:143`）；`m_fmt->flags |= AVFMT_FLAG_CUSTOM_IO`（`:144`）；`int ret = avformat_open_input(&m_fmt, nullptr, nullptr, nullptr);`（`:146`）——**url、format、options 全为 `nullptr`，没有传任何 `AVDictionary`，也没有设置 `m_fmt->interrupt_callback`**；随后 `avformat_find_stream_info(m_fmt, nullptr)`（`:155`）、`av_find_best_stream`（`:162`）、`av_read_frame`（`:230`）。
  - `seekTo(int64_t offset)`（`M4APacketReader.cpp:266-283`）：`close()` + `open()` 重开 demux；`offset > 0` 时走 `avformat_seek_file(m_fmt, -1, INT64_MIN, offset, INT64_MAX, AVSEEK_FLAG_BYTE)` + `avformat_flush`（`:277-281`），是**字节级** seek，不是时间级。
  - `declaredDurationUs()`（`M4APacketReader.cpp:293-296`）返回 `m_fmt->duration`（AV_TIME_BASE 微秒）。

**事实（`AVSEEK_SIZE` 与 `seekable` 当前如何处理）**
- `AVSEEK_SIZE`：4 个 `avioSeek` 回调统一返回 `ctx->size`，即**构造时由调用方传入的 `fileSize`**（不是运行时向数据源查询）。`AVSEEK_FORCE` 只被 `whence & ~AVSEEK_FORCE` 剥离，未做特殊慢路径。
- `AVIOContext.seekable`：**自有源码中从未显式赋值该字段**（对 `sdk/` 全量 Grep `seekable|AVSEEK_SIZE|AVSEEK_FORCE|avio_alloc_context`，排除 `sdk/3rdparty`，命中的 `seekable` 全部属于 `VorbisDecode::open(bool seekable)` 与测试日志，没有任何 `m_avio->seekable = ...`）。
- 库侧确定行为（一手：`sdk/3rdparty/ffmpeg-4.4.4/libavformat/aviobuf.c`）：`ffio_init_context()` 里 `s->seekable = seek ? AVIO_SEEKABLE_NORMAL : 0;`（`aviobuf.c:115`）；`avio_alloc_context()`（`aviobuf.c:138`）在 `:150` 调用 `ffio_init_context`。4 个 PacketReader 都传了**非空 seek 回调** ⇒ `m_avio->seekable == AVIO_SEEKABLE_NORMAL`。即当前等效于「无论底层是否可 seek，都向 FFmpeg 声明为本地文件式可 seek」。
- 结论：`AVSEEK_SIZE` 返回的是「构造时已知长度」，配合 `AVIO_SEEKABLE_NORMAL` 与 `readAt` 绝对偏移，正是快照所称「已知长度 + 可随机 seek」契约；`avioRead` 又把 `readAt` 的 0 直接译作 `AVERROR_EOF`。

---

## 3. `ExtractorFactory` 选择路径与 URL 落点

**事实（注册表与归一化，`sdk/extractor/ExtractorFactory.cpp` / `.h` / `ExtractorHelper.hpp`）**
- `normalizeExtension()`（`ExtractorFactory.cpp:28-43`）：补前导 `.`、逐字符转小写；**不解析 URL、不去查询串、不去 fragment**。
- `sniffExtensionByMagic()`（`ExtractorFactory.cpp:64-116`）：`readHeader` 读前 16 字节（`readAt(0, buf, 16)`，`:45-52`），匹配 `RIFF/WAVE→.wav`、`FORM/AIFF|AIFC→.aiff`、`fLaC→.flac`、`OggS→.ogg`、`ID3→.mp3`、`MAC →.ape`、EBML→`.mkv`、`0xFF 0xFx→.aac`、ASF GUID→`.asf`、`ftyp@4→.m4a`、`#!AMR→.amr`（`:71-114`）。**没有 `.m3u8` / `#EXTM3U` / `.mpd` / `<?xml` / ICY 的任何识别**。
- 内置注册（`ExtractorFactory.cpp:333-343`）：`REGISTER_EXTRACTOR_WITH_SNIFF` 共 10 个（`.wav .aac .mp3 .flac .m4a .ogg .aiff .asf .ape .mkv`）+ `REGISTER_EXTRACTOR(".wma", ASFExtractor)` 1 个 = **11 个扩展名键**；`defaultExtractorMap`（`ExtractorHelper.hpp:32-44`）也是同样 11 个，且注释明确「`.amr` 与 `.opus` 曾在此处虚报支持」。即 `sniffExtensionByMagic` 能吐 `.amr` 但注册表里没有 `.amr`。
- 选择路径 `createExtractor(DataSourceBase *, const std::string &extensionName, bool enableSniff)`（`ExtractorFactory.cpp:233-304`），四级：
  1. `lookupEntry(normalized)` 命中注册表 → `createFromEntry()`（构造即 init，`finalizeExtractor()` 校验 `initCheck()==OK` 且 `AudioSpec.sampleRate/numChannel > 0`，`:158-195`）；成功即返回。
  2. 扩展名命中但构造/校验失败、且 `enableSniff=true` → `sniffExtensionByMagic()` 回退，且要求 `sniffed != normalized`（`:245-262`）；不递归。
  3. 扩展名**未**命中：若 `!enableSniff` → 记 `unsupported extension` 返回 `nullptr`（`:267-270`）；若 `enableSniff` → 先 `sniffExtensionByMagic()`，命中且在注册表则用（`:272-280`）。
  4. 仍未中 → 遍历注册表全部带 `sniffer` 的条目，逐个 `entry.second.sniffer(source)` 试探（`:282-300`），命中即用；全失败记 `createExtractor sniff failed` 返回 `nullptr`（`:302-303`）。`REGISTER_EXTRACTOR_WITH_SNIFF` 宏把 sniffer 绑到 `TYPE::sniff`（`ExtractorFactory.h:57-67`）。
- 生产调用点：`MusicPlayList::_addMusic()` — `ExtractorFactory::isSupportedExtension(fileProperties.extensionName)`（`MusicPlayList.cpp:273`）与 `ExtractorFactory::createExtractor(source.get(), fileProperties.extensionName, true)`（**`enableSniff=true`**，`:275`）；返回空时按「扩展名是否注册」分档 `ExtractorInitFailed` / `ExtractorUnsupported`（`:276-283`）。
- `extensionName` 来源：`FileProperties::parseFileName()`（`sdk/MusicPlayList.h:31-51`）— `fullPath.find_last_of("/")` 取文件名，`fileName.find_last_of(".")` 取最后一个点起（含点）的子串。**只认 `/`、只按最后 `.` 切，不处理 URL scheme / query / fragment**。

**事实（URL 会落到哪里）**
- **生产链实际落点（第一层）**：URL 字符串会作为 `musicPath` 传给 `new FileSource(musicPath.c_str())`（`MusicPlayList.cpp:258`）。`FileSource` 把整串当本地路径 `_wfopen` / `ifstream::open`，打开失败 → `isOpen() == false` → `initCheck()` 返回 `NO_INIT`（`FileSource.cpp:84-87`）→ 在 `MusicPlayList.cpp:264-268` 报 `ErrorCode::FileOpenFailed` 并 `return`。**URL 根本到不了 `ExtractorFactory`**，也不会进入任何 demuxer。当前没有任何把 `http://` / `https://` 识别为网络 URI 的生产代码。
- **假设数据源能读（例如未来 `HttpSource`，或测试替身）时的第二层落点**（以下为「按现有 factory 代码推导的行为」，非当前生产链实测）：
  - 无扩展名 / `.m3u8` / `.mpd`：`normalizeExtension` 得到未注册键（`.m3u8`、`.mpd` 或空）→ `enableSniff=true` → `sniffExtensionByMagic` 读前 16 字节，HLS 清单以 `#EXTM3U`、MPD 以 `<?xml` 开头，不匹配任何二进制 magic → 返回空串 → 遍历 10 个 `TYPE::sniff`（都是二进制 magic，已核对 `WAVExtractor::sniff` 为 `RIFF/WAVE`，`sdk/extractor/wav/WAVExtractor.cpp:18-25`）→ 全部不命中 → `createExtractor sniff failed` → `nullptr` → `ExtractorUnsupported`（因 `isSupportedExtension` 为 false）。
  - 带查询串 `x.mp3?token=...`：`parseFileName()` 得到 `.mp3?token=...`（`MusicPlayList.h:43-46`），`normalizeExtension` 未注册 → 若底层首部是真实 MP3 数据（ID3 或 `0xFF 0xFx`），`sniffExtensionByMagic` 可能回退命中 `.mp3` → 走 `MP3Extractor`；若清单文本则同样落到 `nullptr`。
  - 结论：清单类（`.m3u8` / `.mpd`）与 ICY、无扩展名 URL 在现有「扩展名 + magic sniff」模型下都落到**拒绝**（`nullptr` / `ExtractorUnsupported`），不存在 HLS/DASH demuxer 被选中的路径。

---

## 4. `m_abortDecode` 贯通情况与 `AVFormatContext.interrupt_callback` 现状

**事实**
- `m_abortDecode` 是 `AudioStreamDecoder` 的成员 `std::atomic<bool>`（`sdk/audio/decode/AudioStreamDecoder.h:159`）。
- `stop()` 路径置 `m_stopRequest.store(true)` + `m_abortDecode.store(true)`（`AudioStreamDecoder.cpp:209-210`）；`reset()`/start 时清为 false（`:172-174`、`:214-215`）。
- `cancelRequested()`（`AudioStreamDecoder.cpp:451-457`）返回 `m_stopRequest || m_abortDecode || m_seekRequestMs >= 0`；解码线程多处轮询该口（如 `:471`、`:756`、`:896`）。
- `seekToMs(uint64_t targetMs)`（`AudioStreamDecoder.cpp:219-233`）：`IDLE/ERROR` 状态直接 return；**只写 `m_seekRequestMs` 单槽**（`:232`），注释明确「刻意不置 `m_abortDecode`：它是 stop 的读回调 ABORT 信号（FLACDecode / VorbisDecode 的 `m_abortFlag` 指向它）」（`:226-231`，对应 #59/#60/#65 时序坑）。
- `m_abortDecode` 的下游只有两处 `setAbortFlag`：`flac.setAbortFlag(&m_abortDecode)`（`AudioStreamDecoder.cpp:500`）与 `vorbis.setAbortFlag(&m_abortDecode)`（`:590`）。`FLACDecode::setAbortFlag` / `VorbisDecode::setAbortFlag`（`FLACDecode.cpp:175-177` / `VorbisDecode.cpp:386-388`）把指针存进各自的 `std::atomic<bool> *m_abortFlag`（`FLACDecode.h:21` / `VorbisDecode.h:19`），在其**读/拉包回调**里检查（`FLACDecode.cpp:239, 365`、`VorbisDecode.cpp:256`）。
- **未贯通到自定义 AVIO 读阻塞点**：4 个 `PacketReader` 的 `AvioData` 只有 `{source, pos, size}`（§2），`avioRead` 只调 `source->readAt(...)`，不检查任何 abort/取消标志；没有任何生产代码把 `m_abortDecode` 传给 PacketReader 或 AVIO 回调。对 `sdk/` 全量 Grep `setAbortFlag|abortFlag|m_abortDecode|m_abortFlag|interrupt_callback`（排除 `sdk/3rdparty`），`m_abortDecode` 的引用全部落在 `AudioStreamDecoder.cpp` / `.h` 与 `FLACDecode` / `VorbisDecode`，无一处进入 extractor。
- **`AVFormatContext.interrupt_callback` 现状 = 未设置**：对 `sdk/` 全量 Grep `interrupt_callback`（`glob: "!3rdparty/**"`）**无命中**；4 个 `open()` 在 `avformat_alloc_context()` 之后只设 `m_fmt->pb` 与 `m_fmt->flags |= AVFMT_FLAG_CUSTOM_IO`（§2），没有 `m_fmt->interrupt_callback = ...`，`avformat_open_input` 的 options 也是 `nullptr`。
- 推论（明确标注）：当前「唯一取消信号」`m_abortDecode` 能中断 libFLAC / libvorbisfile 的 native 读回调，但**无法中断阻塞在 `avioRead → DataSource::readAt` 上的 FFmpeg demux 读**；网络读若挂住，`interrupt_callback` / 超时 / 断线重连状态机均不存在于源码。

---

## 5. `durationMs == 0` 与 `seekToMs` 在当前播放器契约里的实际行为

**事实（时长来源与「0 = 未知」）**
- `AudioSpec` 含 `uint64_t durationMs`，默认构造为 0（`sdk/audio/common/AudioCommon.hpp:72-90`，`:88`）。
- `durationMs` 由各提取器从容器头/帧扫描算出（`sdk/extractor/*`），且**0 是真实可达状态**：`ASFExtractor.cpp:104` `spec.durationMs = 0;`、`OGGExtractor.cpp:203` 注释「不取的话 durationMs 恒为 0」、`FLACExtractor.cpp:176-179`（坏头时 `m_spec.durationMs = 0`）。
- 传播链：`MusicPlayList.cpp:294` `signalProperties.durationMs = signalProperties.spec.durationMs` → `MusicPlayer.cpp:684` `m_listener->onMusicPlayerDurationChanged(...)` → `player_impl.cpp:48-55` `m_duration = duration` 并回调 `PlayerCallbacks::onDurationChanged(uint64_t durationMs)`（`sdk/include/soundbridge/player.h:37`）→ `Player::duration()` 返回 `m_duration`（`player_impl.cpp:103-106`，`m_duration` 默认 0，`:126`）。
- 语义：`AudioStreamDecoder.cpp:343` `const bool haveNominal = m_durationMs > 0 && m_decodedRate > 0;`；`:350-353` 注释「durationMs == 0 时 haveNominal=false，不报（规格第 6 节）」，EOS 汇总的 `shortOfNominal` / `seekPastEnd` 均以 `haveNominal` 为前提。即**0 允许，语义是「标称时长未知」**。
- UI 层：`app/mainwindow.cpp:699-700` `const qulonglong durationMs = viewState.durationMs; mDurationSlider->setRange(0, durationMs / 1000);` — `durationMs==0` 时进度条 range 为 `(0, 0)`，滑块不可拖动（等效禁用 seek）；`app/playercontroller.cpp:126` 的 `m_player->seek(seconds*1000)` 是唯一 seek 入口。

**事实（seek 契约与 `durationMs==0` 的空操作）**
- 接口：`Player::seek(uint64_t positionMs)`（`player.h:58`）→ `Player::seek`（`player_impl.cpp:260-263`）→ `Impl::seek` → `m_player->setPosition(positionMs)`（`:155`）→ `MusicPlayer::setPosition`（`MusicPlayer.cpp:889-891`）→ `Impl::setPosition`（`:227-233`，经 `m_workQueue.asyncRunTask` **异步**跑 `_setPosition`）→ `Impl::_setPosition`（`:388-418`）。
- `_setPosition` 的丢弃条件（`:396-409`）：无 `cur`/`task`/`decoder`（停止/切歌窗口）丢弃；`task->trackIndex != cur->index` 丢弃；**`if (pos > cur->signalProperties.durationMs) return;`（`:407-409`）**。⇒ 当 `durationMs == 0` 时，**任何 `pos > 0` 的 seek 都在这一行直接返回（空操作/丢弃）**；`pos == 0` 不满足 `0 > 0`，会继续执行 `cur->signalProperties.curPositionMs = pos;`（`:413`）与 `task->decoder->seekToMs(pos);`（`:414`）。`Player` / `Impl` / `MusicPlayer` 各层均**没有**单独的 `durationMs == 0` 判断分支（全量 Grep `durationMs == 0|duration() == 0` 在 `sdk/` 排除 3rdparty 只命中 `AudioStreamDecoder.cpp:351` 的注释与测试文件）。
- `_setPosition` 通过后：`AudioStreamDecoder::seekToMs()`（`:219-233`，见 §4）只写单槽；解码线程 FLAC/Vorbis 循环里 `m_durationMs > 0` 的判断只出现在 **seek 失败后的分支**（`:548-549`、`:616-617`），用于把「超出可解内容」汇总成 `DataTruncated`，不是入口处的空操作开关。
- `ExtractorHelper::seekToMs(uint64_t targetMs, uint64_t *actualMs = nullptr)`（`sdk/extractor/ExtractorHelper.hpp:142-147`）基类默认返回 `sdk_utils::INVALID_OPERATION`；**没有任何提取器 override 它**（对 `sdk/extractor` Grep `seekToMs` 只命中这一处声明）。生产链的时间定位实际由 `AudioStreamDecoder::seekToMs` + `PacketReader::seekTo(offset)` 的字节 seek（§2）实现，不经 `ExtractorHelper::seekToMs`。这与快照「seek 只保留 `seekToMs`」一致，但要注意接口层有 `Player::seek` / `MusicPlayer::setPosition` 两级包装。

---

## 6. FFmpeg 构建：`dash` demuxer、`protocol_whitelist` 与协议启用（核实 §7 第 7 项）

**事实（构建入口，一手构建脚本）**
- 版本与入口：`sdk/3rdparty/CMakeLists.txt` 设 `FFMPEG_ROOT = ${3RDPARTY_DIR}/ffmpeg-4.4.4`（`:69`）；非 ARM 用 `${FFMPEG_ROOT}/build.sh`（`:79-84`），ARM 用 `build_linux_arm.sh`（`:72-77`）。`build_option.cmake:6-8` 默认 `BUILD_OPTION_FFMPEG ON`。
- `sdk/3rdparty/ffmpeg-4.4.4/build.sh` 的 `./configure`（`:28-41`）：`--enable-shared --enable-gpl --enable-version3 --enable-nonfree --disable-postproc --disable-debug --disable-doc --disable-ffmpeg --disable-ffplay --disable-ffprobe --disable-asm`。`build_linux_arm.sh`（`:28-43`）同上，仅把 `--disable-asm` 换成 `--disable-x86asm` 并加交叉编译参数。
- **`build.sh` / `build_linux_arm.sh` 均未出现任何 `--enable-protocol` / `--disable-protocol` / `--enable-demuxer` / `--disable-demuxer` / `--enable-network` / `--disable-network` / `--enable-libxml2` 选项**；协议与 demuxer 的启用完全交给 FFmpeg `configure` 默认。
- `--disable-ffmpeg/ffplay/ffprobe` 对应「没编命令行工具」。

**事实（`config.h` = configure 产物，直接读 `CONFIG_*`）**
- `sdk/3rdparty/ffmpeg-4.4.4/config.h` 存在（85 KB），头部 `FFMPEG_CONFIGURATION` 逐字记录了与 `build.sh` 完全一致的命令行，且 `OS_NAME mingw32`、`CC_IDENT "gcc 7.3.0 ... MinGW-W64"`、`FFMPEG_DATADIR .../dist/windows_x86_64_gcc_debug` — **这是 Windows / MinGW 一次真实 configure 的产物**，`sdk/3rdparty/dist/` 下也只有 `windows_x86_64_gcc_debug/` 一个平台目录。
- **`CONFIG_DASH_DEMUXER 0`**（`config.h:2217`）⇒ **`dash` demuxer 未启用**。
- 因果链（一手）：`configure:3318` `dash_demuxer_deps="libxml2"`；`config.h:503` `CONFIG_LIBXML2 0`；`libxml2` 列在 `configure:1757` 的 `EXTERNAL_LIBRARY_LIST`（`:1822`，默认关闭、不在 `AUTODETECT_LIBS` `:1974` 内），`build.sh` 未传 `--enable-libxml2`（`configure:300` 帮助文本亦注明「needed for dash」）。⇒ 依赖不满足，dash demuxer 被自动禁用。
- 协议 / demuxer 启用值（`config.h`）：
  - `CONFIG_NETWORK 1`（`:590`）
  - `CONFIG_HLS_DEMUXER 1`（`:2266`）、`CONFIG_HLS_PROTOCOL 1`（`:2669`）
  - `CONFIG_HTTP_PROTOCOL 1`（`:2670`）、`CONFIG_HTTPS_PROTOCOL 1`（`:2672`）、`CONFIG_MMSH_PROTOCOL 1`（`:2674`）
  - `CONFIG_RTMP_PROTOCOL 1`（`:2679`）、`CONFIG_RTMPS_PROTOCOL 1`（`:2681`）
  - `CONFIG_TCP_PROTOCOL 1`（`:2690`）、`CONFIG_TLS_PROTOCOL 1`（`:2691`）、`CONFIG_UDP_PROTOCOL 1`（`:2692`）
  - TLS 后端：`CONFIG_SCHANNEL 1`（`:525`）、`CONFIG_OPENSSL 0`（`:432`）、`CONFIG_GNUTLS 0`（`:445`）⇒ Windows 侧 TLS 走 Schannel。
  - CLI：`CONFIG_FFMPEG 0` / `CONFIG_FFPROBE 0` / `CONFIG_FFPLAY 0`（`:577-579`）。
- 即快照 §1「带 HTTP/HTTPS/TLS/HLS/RTMP/TCP/UDP 协议」在**这份 Windows 构建里属实**，但属 `configure` 默认启用，**不是项目显式配置**；且「库里协议 = 1」**不等于播放器能用**——§2 的自定义 `AVIOContext` + `avformat_open_input(&fmt, nullptr, nullptr, nullptr)` 绕开了 FFmpeg 的 URL/协议层（url 为 `nullptr`、options 为 `nullptr`）。

**事实（`protocol_whitelist` 取值，核实 §7 第 7 项）**
- 全仓 Grep `protocol_whitelist`（`glob: "!sdk/3rdparty/**"`）**只命中** `docs/research/network-audio-sources-survey.md:445`（P2 建议里提及）与 `:508`（§7 第 7 项标未核实），**任何生产代码都没有该字样**。
- 4 个 `open()` 传给 `avformat_open_input` 的第 4 参数是 `nullptr`（§2），没有 `AVDictionary *options`，因此**本项目没有设置 `protocol_whitelist`，不存在「应用层的取值」**。
- 库内默认（一手）：`sdk/3rdparty/ffmpeg-4.4.4/libavformat/avio.c:58` 把 `protocol_whitelist` 定义为 `AV_OPT_TYPE_STRING`，默认 `{ .str = NULL }`；实际打开 URL 时若未设置会回落到 `uc->prot->default_whitelist`（`avio.c:194-203`），并由 `options.c:191` 的 `ffio_open_whitelist(..., s->protocol_whitelist, s->protocol_blacklist)` 传入。但当前自定义 AVIO 路径不触发协议打开，故该默认值在本链路上不构成生效取值。

**推断（明确标注，非仓库内实证）**
- Linux CI 侧使用同一个 `build.sh`（非 ARM 分支），`libxml2` 在 `EXTERNAL_LIBRARY_LIST` 默认关闭且未 `--enable-libxml2` ⇒ 推断 Linux 侧 `dash` demuxer 同样为 0、协议宏组合同理；但仓库内**没有 Linux 侧 configure 产物**可实证，见 §7。

---

## 7. 未核实项（含已查范围）

1. **Linux CI 侧的 FFmpeg configure 产物**（`config.h` / `ffbuild/config.mak`）未提交到仓库，Linux 侧 `CONFIG_DASH_DEMUXER` / 协议 / TLS 后端的实证缺失。已查范围：`Glob sdk/3rdparty/dist/**/config.{h,mak}` 无文件；`ls sdk/3rdparty/dist/` 只有 `windows_x86_64_gcc_debug/`；`Glob sdk/3rdparty/ffmpeg-4.4.4/config.{h,mak}` 的 `config.h` 是 Windows 产物（由 `FFMPEG_CONFIGURATION` 判定）。因此「Linux 侧 dash 也不启用」是基于 `configure:3318` + `EXTERNAL_LIBRARY_LIST` 默认关闭的**推断**，不是实证。Windows 侧结论（§6）不受影响。
2. **FFmpeg 库内 `protocol_whitelist` 的最终生效值**（各 `URLProtocol.default_whitelist` 的具体字符串，如 hls/http 各自允许哪些子协议）。已查范围：`libavformat/avio.c:58`（AVOption 默认 NULL）、`:179-215`、`:335-341`（`default_whitelist` 回落与 `av_match_list` 分支）、`libavformat/options.c:191`；未逐个协议核对 `default_whitelist` 定义。当前自定义 AVIO 路径不触发该逻辑，故不影响现状结论。
3. **其余 9 个 extractor `sniff()` 对 HLS/MPD/ICY 文本清单是否一律返回 false**。已核实 `sniffExtensionByMagic()` 全部 11 条 magic 规则（`ExtractorFactory.cpp:64-116`）与 `WAVExtractor::sniff`（`RIFF/WAVE`，`WAVExtractor.cpp:18-25`）；另 9 个 sniffer 未逐个读实现，仅由注册宏 `REGISTER_EXTRACTOR_WITH_SNIFF` 绑定 `TYPE::sniff`（`ExtractorFactory.h:57-67`）及同类二进制 magic 风格推断。
4. **快照 §1「`getSize` 走全量扫描」所指的精确代码位置**。`FileSource::getSize` 已核实为 O(1) 返回缓存 `m_length`；源码里「全量扫描」性质的描述出现在提取器 init 期只读扫描（`M4APacketReader.h:21-22`「读完整份文件只为累加标称时长」、`ExtractorHelper.hpp:152-155`），但快照作者原指哪一处未核实。
5. **Linux 侧 TLS 后端**（系统 openssl / gnutls 是否被 configure 检测到）与 Linux 侧 `CONFIG_HTTPS_PROTOCOL` 的实证值。Windows 产物里是 `SCHANNEL=1 / OPENSSL=0 / GNUTLS=0`；Linux 无产物，未核实。
6. **`AVFMT_NOFILE` 等 demuxer 与自定义 AVIO 的冲突日志文本**、以及 HLS `hls.c` 里 `ff_make_absolute_url` / `ffio_open_whitelist` 的逐行调用细节：属 FFmpeg 库内部，本票未逐行核对（调研快照 §2.3.5 / §7 第 8 项亦自述未逐字核对）。

---

## 8. 与调研快照（`docs/research/network-audio-sources-survey.md` §1 / §7）不一致之处

1. **`getSize` 不是全量扫描**（§1）。`FileSource::getSize`（`FileSource.cpp:137-148`）直接返回构造时 `_fseeki64/tellg` 缓存的 `m_length`，O(1)；长度在构造时就定死。「全量扫描」在源码里对应的是提取器 init 期的只读扫描（§7 第 4 项），不是 `getSize`。
2. **格式数量是 11 不是 12**（§1「支持 12 种本地格式」）。`ExtractorFactory.cpp:333-343` 与 `defaultExtractorMap`（`ExtractorHelper.hpp:32-44`）都是 11 个扩展名（10 个带 sniff + `.wma` 不带 sniff）。另外 `sniffExtensionByMagic` 会吐 `.amr`（`ExtractorFactory.cpp:112-114`），但 `.amr` **未注册**，`defaultExtractorMap` 注释明确它曾虚报支持。
3. **§7 第 7 项已核实**：`dash` demuxer **未启用**（`config.h:2217 CONFIG_DASH_DEMUXER 0`，因 `libxml2` 依赖未满足）；`protocol_whitelist` **本项目未设置取值**（`avformat_open_input` options = `nullptr`，生产代码无该字样），库内 AVOption 默认 NULL。两项由「未核实」变为「已核实」。
4. **协议「启用」的层次不同**（§1 / §2.5.5）。`CONFIG_NETWORK/HTTP/HTTPS/TLS/HLS/RTMP/TCP/UDP = 1` 是 FFmpeg `configure` 默认产物（Windows `config.h` 实证），不是项目显式开关；且 §2 的自定义 `AVIOContext` + `url = nullptr` 绕开 FFmpeg 协议层，**库里协议可用 ≠ 播放器能接入 URL**。TLS 后端在 Windows 是 Schannel（`CONFIG_SCHANNEL 1`，openssl/gnutls 为 0）。
5. **取消信号的贯通范围比快照更窄**（§2.5.5 表 #6「`m_abortDecode` 是唯一取消信号」属实，但需补充）：它只经 `setAbortFlag` 贯通到 `FLACDecode` / `VorbisDecode` 的读回调，**未贯通到 4 个 PacketReader 的 `avioRead` 阻塞点**；`AVFormatContext.interrupt_callback` 在全自有 `sdk/` 源码中零命中、未设置。
6. **工厂不是简单的「扩展名 + magic sniff」**（§1 / §2.5.5 表 #8）：实际是四级路径（扩展名 → 扩展名失败后 magic 回退 → 未注册时 magic sniff → 遍历全部 sniffer 兜底），见 §3。更关键的是**生产链第一层**：URL 字符串在 `MusicPlayList.cpp:258-268` 被 `FileSource` 当本地路径打开失败，以 `FileOpenFailed` 拒绝，**到不了 `ExtractorFactory`**；快照只讨论了 factory 层的 sniff 失败，未覆盖这一层。
7. **`DataSource::readAt` 的 EOF 语义**（§1）：接口注释明确 `readAt` 返回 0 是「offset 到达/越过末尾」而非错误（`DataSource.hpp:21-24`）；`avioRead` 把 `n <= 0` 一律译作 `AVERROR_EOF`（`M4APacketReader.cpp:66-67`）。`AVIOContext.seekable` 也并非「未处理」：自有源码未显式赋值，但 `avio_alloc_context` 因拿到非空 seek 回调而在库内置为 `AVIO_SEEKABLE_NORMAL`（`aviobuf.c:115`），即**总声明可 seek**。
8. **时长/seek 契约的细节**（§1「`durationMs == 0` 已允许」「seek 只保留 `seekToMs`」）：属实，但 `durationMs==0` 的 seek 空操作不是显式 `if (duration == 0)`，而是 `_setPosition` 的通用边界 `if (pos > durationMs) return;`（`MusicPlayer.cpp:407`）导致所有 `pos > 0` 被丢弃，`pos == 0` 仍会走 `seekToMs(0)`；UI 侧由 `mDurationSlider->setRange(0, durationMs/1000)`（`mainwindow.cpp:699-700`）间接禁用拖动。且 `ExtractorHelper::seekToMs` 基类默认返回 `INVALID_OPERATION`、无提取器 override，生产链定位走的是 `AudioStreamDecoder::seekToMs` 单槽 + `PacketReader::seekTo` 字节 seek。
