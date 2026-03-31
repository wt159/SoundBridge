# AUDIO DECODE KNOWLEDGE BASE

## OVERVIEW
`sdk/audio/decode/` 负责把 extractor 暴露的 codec/spec/数据源变成可消费 PCM，覆盖同步解码、整段解码和流式解码三条路径。

## STRUCTURE
```text
sdk/audio/decode/
├── AudioDecode.*          # FFmpeg codec 封装 + parser/packet/frame 生命周期
├── AudioDecodeProcess.*   # 一次性把 extractor 数据解成完整 AudioBuffer
├── AudioStreamDecoder.*   # 带线程、seek、EOS/ERROR 状态的流式解码器
├── FLACDecode.*           # FLAC 专用路径
└── VorbisDecode.*         # Vorbis 专用路径
```

## WHERE TO LOOK
| 任务 | 位置 | 备注 |
|------|------|------|
| FFmpeg codec 初始化失败 | `AudioDecode.cpp` | `avcodec_find_decoder` / `avcodec_open2` / parser fallback |
| codec 参数来自哪里 | `AudioDecodeProcess.cpp` | 从 extractor 读取 sampleRate/channels/blockAlign/extraData |
| 流式 seek / stop / EOS | `AudioStreamDecoder.cpp` | `m_state`、`m_seekRequestMs`、`m_abortDecode` |
| FLAC/Vorbis 特化路径 | `FLACDecode.*`, `VorbisDecode.*` | DataSource 与 metadata 两条入口 |
| 回归入口 | `../../test/unit/test_decode.cpp`, `../../test/TestSdkSuite.cpp` | doctest 覆盖最全，旧回归覆盖基础 decode |

## CONVENTIONS
- `AudioDecode` 是 FFmpeg 通用解码层；只有 FLAC/Vorbis 这类已有专用实现时，才走 `FLACDecode` / `VorbisDecode` 分支。
- `AudioDecodeProcess` 的职责是“整段解完再合并 buffer”；`AudioStreamDecoder` 的职责是“边解边写 ring + 管理状态机”，两者不要混用职责。
- `AudioStreamDecoder` 的 seek 不是同步完成语义；调用 `seekToMs()` 后要通过状态轮询和 ring drain 观察是否回到 `DECODING`/`EOS`。
- codec 问题排查要先确认 extractor 给出的 `AudioCodecID`、`AudioSpec`、`extraData`、`blockAlign` 是否合理，再看 FFmpeg 报错。

## ANTI-PATTERNS
- 不要忽略 `av_parser_init()` 失败后的 fallback 路径；当前实现允许 parser 不可用时直接 packet decode。
- 不要把 `AudioDecodeProcess` 用在超大流媒体场景里假装它是流式解码器；它会累计 `m_decBufVec` 再 merge。
- 不要直接假设 `start()` 后很快到 `EOS`；现有测试都用 `waitForDecoderState()` / `waitForDecoderStateWithDrain()` 轮询。
- 不要把 `AUDIO_CODEC_ID_NONE` 当成错误；对 WAV 之类无压缩数据，`AudioDecodeProcess` 会直接使用 extractor metadata。

## LOCAL CHECKS
```bash
cmake --build build --target TestSdkSuite
cmake --build build --target UnitTests
ctest --test-dir build -R sdk_decode_tests --output-on-failure
ctest --test-dir build -R sdk_unit_tests --output-on-failure
ctest --test-dir build -R sdk_player_tests --output-on-failure
```

## NOTES
- `test_decode.cpp` 既测构造与错误路径，也测真实媒体（AAC / FLAC / OGG / WAV）通过 `AudioDecodeProcess` 与 `AudioStreamDecoder` 的行为；改这里优先跑它。
- `AudioStreamDecoder` 依赖 `AudioRingBuffer` 容量与 drain 行为；如果测试卡在非 EOS，先看 ring 是否被及时读空。
- `AudioDecodeProcess` 对 FLAC/Vorbis 走专用 decoder，对其他 codec 走 `AudioDecode + AudioCodecConfig`；这正是排查“某格式独坏”时的第一分叉点。
