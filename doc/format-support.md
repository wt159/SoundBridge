# 格式支持清单

本文件是「受支持格式」的**人读版本**；机器可读的唯一真源是 `sdk/extractor/ExtractorHelper.hpp` 里的
`defaultExtractorMap` 与 `ExtractorFactory::registeredExtensions()`。两者由
`sdk/test/unit/test_extractor.cpp` 的「受支持格式清单与注册表一致」用例锁定——改清单必须同时改这两处，
否则测试会红。

判定标准（来源：[#20](https://github.com/wt159/SoundBridge/issues/20)）：

> **受支持 = 能解出与标称时长一致的 PCM，且被覆盖度断言覆盖。**
> 注册表里注册了不等于支持，枚举里有更不等于支持。

## 受支持

| 扩展名 | 提取器 | 解码路径 | spec 完整性 | 覆盖度断言 | 备注 |
|---|---|---|---|---|---|
| `.wav` | `WAVExtractor` | 项目自实现，PCM 直通 | 完整 | 待补（#16） | 兼容 `.w64` 之前先按不支持处理 |
| `.aiff` | `AIFFExtractor` | 项目自实现 | 完整 | 待补（#16） | |
| `.mp3` | `MP3Extractor` | ffmpeg + mp3 parser | **部分未知**：`format`/`bytesPerSample`=0 | 待补（#16） | 解码输出为 fltp（4 B/采样），真实格式由首帧回填 |
| `.aac` | `AACExtractor` | ffmpeg + adts parser | 完整 | 待补（#16） | |
| `.flac` | `FLACExtractor` | libFLAC 直解 | 完整 | 待补（#16） | 无 parser，边界由 libFLAC 逐帧给出 |
| `.ogg` | `OGGExtractor` | libvorbis 直解 | **部分未知**：`format`/`bytesPerSample`=0 | 待补（#16） | `durationMs` 由末页 granule 推导（#15） |
| `.m4a` | `M4AExtractor` | ffmpeg demux + 分包 | 完整 | 待补（#16） | ALAC（无 parser）路径尚未验证 |
| `.asf` | `ASFExtractor` | ffmpeg demux + 分包 | 完整 | 待补（#16） | 另有自研解析兜底路径，同样按数据包切片 |
| `.wma` | `ASFExtractor` | 同上（无嗅探注册） | 完整 | 待补（#16） | |
| `.ape` | `APEExtractor` | ffmpeg demux + 分包 | 完整 | **已有**：`APE decode covers advertised duration` | 2026-11 修复只解首帧的问题（`50b538f0c`） |
| `.mkv` | `MKVExtractor` | ffmpeg demux + 分包 | 完整 | 待补（#16） | 仓库内 `music-mkv.mkv` 的容器时长字段是错的（把采样数当毫秒），**故意保留**作为「不轻信容器声明」的回归样本 |

## 不支持（明确摘除，别再注册回去）

| 扩展名 | 现状 | 原因 |
|---|---|---|
| `.amr` | 已从注册表与 `defaultExtractorMap` 摘除 | 此前被指向 `ASFExtractor`，对真实 AMR-NB/WB 只会静默解错。内容恰为 ASF/WMA 的文件仍可被魔法字节嗅探识别，那是内容嗅探而非扩展名支持 |
| `.opus` | 已从 `standardExtractors` 枚举与映射中删除 | 从未有过 `OpusExtractor`，也没有 Opus 解码器；`VorbisDecode` 不能顶替 |
| `.au` / `.nsp` / `.w64` / `.wsig` | 从未注册 | 素材或许可解，但补支持属于「扩格式面」，不在正确性基线范围内 |

## spec 完整性约定（来源 [#21](https://github.com/wt159/SoundBridge/issues/21)）

`sampleRate` / `numChannel` 必须有效——`ExtractorFactory::createExtractor` 在出口校验，不合格直接返回 `nullptr`。
`format` / `bytesPerSample` 允许为 `Unknown` / 0：mp3、ogg 的容器不声明 PCM 采样格式，真实值由解码首帧回填。
**因此任何按 spec 算字节数或时长的代码都必须先处理这两个字段为 0 的情况**——不能直接拿来做除法或乘算。

## 维护规则

1. 新增格式 = 新提取器 + 注册 + 本表加行 + 更新一致性用例里的期望列表，四件事一起做。
2. 摘除格式 = 删注册 + 删映射/枚举 + 本表移到「不支持」+ 同步用例。
3. `FileSearch` 以 `defaultExtractorMap` 作为扫描白名单，所以本表的增删会直接改变播放列表能看到什么。
