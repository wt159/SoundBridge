# ASF EXTRACTOR KNOWLEDGE BASE

## OVERVIEW
`sdk/extractor/asf/` 不只是 `.asf` 解析器；它同时承接 `.wma`、`.amr` 的共享实现，并在 FFmpeg demux 失败时回退到自定义 ASF object/data packet 解析。

## WHERE TO LOOK
| 任务 | 位置 | 备注 |
|------|------|------|
| 扩展名入口 | `../ExtractorFactory.cpp` | `.asf` 走 sniff 注册，`.wma` / `.amr` 直接映射到 `ASFExtractor` |
| 优先路径 | `ASFExtractor::initWithFFmpegDemux()` | 自定义 `AVIOContext` + `avformat_open_input/find_stream_info` |
| 回退路径 | `ASFExtractor::init()` | demux 失败后扫描 GUID/object header |
| 头对象解析 | `parseHeaderObject()` | 提取 audio stream、`WAVEFormatEx`、`m_audioStreamNumber` |
| 数据包解析 | `parseDataObject()`, `parsePayloadData()` | 只抽音频 stream payload，拼成 `m_metaBuf` |
| 回归入口 | `../../test/unit/test_extractor.cpp`, `../../test/TestSdkSuite.cpp` | `ASF/WMA/AMR` 都在这里验证 |

## CONVENTIONS
- 调试 `ASFExtractor` 时先看 `initWithFFmpegDemux()` 是否已经成功；只有 demux 失败或拿不到可用音频流时，才继续看手写 parser 路径。
- `ASFExtractor` 的局部职责不只是 metadata；它还要产出 `m_codecExtraData`、`m_bitRate`、`m_blockAlign` 和拼接后的 payload buffer，供上游 decode 使用。
- `formatTag2AudioCodecID()` 与 `WAVEFormatEx2AudioSpec()` 一起决定 `AudioCodecID` 和 `AudioSpec`；碰到 WMA/AMR 行为异常时先核这层转换。
- `m_audioStreamNumber` 是 packet 过滤关键字段；data object 里看到 payload 但没有音频输出时，优先核 stream number 是否对上。

## ANTI-PATTERNS
- 不要只修 `.asf` 案例却忘了 `.wma`、`.amr` 也走这个目录；这里是三扩展共享实现。
- 不要在 `avformat_close_input()` 之后再访问 `fmt->duration`、`streams` 或 `codecpar`；需要的字段必须先拷出来。
- 不要把 FFmpeg demux 路径和手写 packet 解析路径混着改；先确定问题发生在哪条分支，再改对应逻辑。
- 不要假设所有 packet 都属于音频流；`parsePayloadData()` 依赖 `m_audioStreamNumber` 过滤。

## LOCAL CHECKS
```bash
cmake --build build --target TestSdkSuite
cmake --build build --target UnitTests
ctest --test-dir build -R sdk_extractor_spec_tests --output-on-failure
ctest --test-dir build -R sdk_media_smoke --output-on-failure
ctest --test-dir build -R sdk_decode_tests --output-on-failure
```

## NOTES
- `test_extractor.cpp` 明确覆盖 `.asf`、`.wma`、`.amr` 三条路径；改这个目录至少要回看这三组样本，而不是只盯 `.asf`。
- 这个子目录虽然只有 2 个文件，但实现体量超过 1.2k 行，而且包含完整 GUID/object/data packet 解析；它和普通“单 extractor 目录”不是一个复杂度级别。
- 其他 FFmpeg-backed 目录（`m4a/`、`ape/`、`mkv/`）暂时仍由父级 `sdk/extractor/AGENTS.md` 承接，因为它们没有像这里这样同时承担多扩展入口 + 双路径解析架构。
