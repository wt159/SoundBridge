# 格式支持清单

本文件是「受支持格式」的**人读版本**；机器可读的唯一真源是 `sdk/extractor/ExtractorHelper.hpp` 里的
`defaultExtractorMap` 与 `ExtractorFactory::registeredExtensions()`。两者由
`sdk/test/unit/test_extractor.cpp` 的「受支持格式清单与注册表一致」用例锁定——改清单必须同时改这两处，
否则测试会红。

**本文件只讲好文件。** 文件坏了该退化到什么程度、用什么判据、报什么码，全部在
[`doc/bad-input-spec.md`](bad-input-spec.md) 里，两份文档不共用容差也不互相引用判据：
本文件的容差回答「这份素材的标称时长可不可信」，那份的容差回答「这个坏文件应该解出多少」。

判定标准（来源：[#20](https://github.com/wt159/SoundBridge/issues/20)）：

> **受支持 = 能解出与标称时长一致的 PCM，且被覆盖度断言覆盖。**
> 注册表里注册了不等于支持，枚举里有更不等于支持。

## 受支持

| 扩展名 | 提取器 | 解码路径 | spec 完整性 | 覆盖度断言 | 备注 |
|---|---|---|---|---|---|
| `.wav` | `WAVExtractor` | 项目自实现，PCM 直通 | 完整 | **有** | 兼容 `.w64` 之前先按不支持处理 |
| `.aiff` | `AIFFExtractor` | 项目自实现 | 完整 | **有** | |
| `.mp3` | `MP3Extractor` | ffmpeg + mp3 parser | **部分未知**：`format`/`bytesPerSample`=0 | **有** | 解码输出为 fltp（4 B/采样），真实格式由首帧回填 |
| `.aac` | `AACExtractor` | ffmpeg + adts parser | 完整 | **有** | |
| `.flac` | `FLACExtractor` | libFLAC 直解（不经 ffmpeg parser） | 完整 | **有** | 帧边界与 EOS 由 libFLAC 给出，不受分包策略影响 |
| `.ogg` | `OGGExtractor` | libvorbis 直解 | **部分未知**：`format`/`bytesPerSample`=0 | **有** | `durationMs` 由末页 granule 推导（#15） |
| `.m4a` | `M4AExtractor` | ffmpeg demux + 分包 | 完整 | **有** | 两份素材：AAC 与 ALAC。ALAC 无 parser，边界只能由提取器给，已实测（`music-alac.m4a`） |
| `.asf` | `ASFExtractor` | ffmpeg demux + 分包 | 完整 | **有** | 另有自研解析兜底路径，同样按数据包切片 |
| `.wma` | `ASFExtractor` | 同上（无嗅探注册） | 完整 | **有** | |
| `.ape` | `APEExtractor` | ffmpeg demux + 分包 | 完整 | **有**（含流式路径） | 2026-11 修复只解首帧的问题（`50b538f0c`） |
| `.mkv` | `MKVExtractor` | ffmpeg demux + 分包 | 完整 | **有** | 仓库内 `music-mkv.mkv` 的容器时长字段是错的（把采样数当毫秒），**故意保留**作为「不轻信容器声明」的回归样本 |

## 覆盖度基线（来源 [#16](https://github.com/wt159/SoundBridge/issues/16)）

用例在 `sdk/test/unit/test_media_coverage.cpp` 的 `MediaCoverage` 套件里：15 份素材覆盖上表 11 个
扩展名，随 `ctest -R sdk_unit_tests` 运行，净耗时约 2.5 秒。

**判据是双向的**——只有下界会漏报「重复解码」（#13 普查里 FLAC 重复 35× 仍判 PASS）：

```text
measuredMs = 解码 PCM 字节数 / (声道数 × 每采样字节) × 1000 / 采样率
tolerance  = max(declaredMs × 1%, 150ms)，且不超过 declaredMs × 25%
下界：防截断（APE 只解首帧 = 18%）
上界：防重复（35× 这类）
```

- 1% 是相对量；150ms 是绝对下限，覆盖帧/页量化误差（ogg 一页最多约 93ms、AAC 一帧 21ms）；
  25% 上限防止极短素材上的断言退化成「怎么都过」。
- **这套容差只对完好素材成立。** 矩阵里的 15 份都是完好文件，所以「短于标称」在这里是失败信号；
  换成一个**被截断的文件**，「短」就是正确行为，判据换成
  `expect = 截断字节比例 × 完好文件实测时长`、带宽 `±5pp`（见 `doc/bad-input-spec.md` 第 3 节）。
  两套数值不能互换：实测「字节比例 → 时长比例」最大偏 2.07 个百分点，沿用这里的 1% 会直接假红。
- 实测最大偏差是 `.wma` 的 +0.96%（259164 → 261642ms，+2478ms / 容差 2591ms，余量仅 113ms），
  其次是 `.asf` 的 −0.39%（34874 → 34737ms，−137ms / 容差 348ms）；其余 13 份为 0.00%。
  这是 2026-11-04 的基线：#22 把 `durationMs` 来源单点前移后，asf / wma 的 declared 分别
  +208ms / −2478ms，而 15 份的 measured 都没变。**1% 容差在 wma 上已用掉 95.6%**，
  别再按旧记录里的「5× 余量」估计；再动时长推导要盯这一行。
- **两侧数值一致**（2026-11-04 复核：CI run `41763075682` vs Windows 本地
  `UnitTests -ts=MediaCoverage -s`）：15 行 `declared/measured` 逐项相同、15/15 全绿，
  两项非零偏差如上。CI 的 `Report coverage matrix numbers` 步骤每次都用
  `-ts=MediaCoverage -s` 把这张表打进日志——换 runner、升 ffmpeg 后直接比日志，不必本地复现。
- **ALAC 分支已实测**（2026-11-03，`music-alac.m4a`，44.1kHz 立体声 12s）：`declared 12000ms /
  measured 12000ms`，零偏差。这条路径是 #17 决策里唯一没被素材打到的分支——ALAC 在 ffmpeg 里
  没有 parser，切包只能靠解复用边界；只解首包的话会得到约 93ms（ALAC 一帧 4096 采样），
  实测 12000ms 说明边界确实按包给全了。素材由 `小镇姑娘-陶喆.flac` 截 12s 无损转码而来。

**实测侧必须取「解码后的 spec」**（`AudioDecodeProcess::getDecodeSpec()`），不能取提取器 spec：
mp3/ogg 的提取器 `bytesPerSample` 是 0（会除零），而 aac/asf/mp3/wma 的解码输出是 fltp
（4 B/采样），按 2 B 换算会得到虚假的 2 倍——#19 的「mp3 2.03 倍」正是这么来的。

采样格式**取决于 codec，不取决于扩展名**：`.m4a` 下 AAC 解出 fltp（4 B/采样），ALAC 解出 s16
（2 B/采样）。所以任何按扩展名套字节数的换算都是错的，只能读解码后的 spec。

**没有白名单**。`durationMs == 0` 在受支持格式上是硬失败而不是跳过（#15 已让 11 个扩展名都能推导
时长）；缺素材同样计入失败，并在输出里打 `[SKIP]` 便于定位。新增格式时，
`every supported extension has a real-media coverage case` 会强制要求同时补一行素材——
「受支持」的定义就是「有覆盖度断言」，不接受登记了却没人测。

**测量路径**：矩阵走 `AudioDecodeProcess`（同步整段，唯一能给出解码后 spec 的路径）；生产流式路径
（`start()` → `threadFunc()`）由 `APE decode covers advertised duration` 用同一套容差守一条端到端。

## 不支持（明确摘除，别再注册回去）

| 扩展名 | 现状 | 原因 |
|---|---|---|
| `.amr` | 已从注册表与 `defaultExtractorMap` 摘除 | 此前被指向 `ASFExtractor`，对真实 AMR-NB/WB 只会静默解错。内容恰为 ASF/WMA 的文件仍可被魔法字节嗅探识别，那是内容嗅探而非扩展名支持 |
| `.opus` | 已从 `standardExtractors` 枚举与映射中删除 | 从未有过 `OpusExtractor`，也没有 Opus 解码器；`VorbisDecode` 不能顶替 |
| `.au` / `.nsp` / `.w64` / `.wsig` | 从未注册 | 素材或许可解，但补支持属于「扩格式面」，不在正确性基线范围内 |

## spec 完整性约定（来源 [#21](https://github.com/wt159/SoundBridge/issues/21)）

`sampleRate` / `numChannel` 必须有效——契约是 `ExtractorFactory::createExtractor` 在出口校验，不合格直接返回 `nullptr`。
`format` / `bytesPerSample` 允许为 `Unknown` / 0：mp3、ogg 的容器不声明 PCM 采样格式，真实值由解码首帧回填。
**因此任何按 spec 算字节数或时长的代码都必须先处理这两个字段为 0 的情况**——不能直接拿来做除法或乘算。

**现状与这条契约有差**：`ExtractorFactory.cpp:143-149` 遇到不合格 spec 只打 `LOGW` 就放行，所以 aac 的
全零头能带着 `sr=96000 ch=0` 一路走到播放列表。这条差已在
[`doc/bad-input-spec.md`](bad-input-spec.md) 第 10 节列为实现项（#14，含给测试替身的 `allowEmptySpec` 逃生口）；
在那条修好之前，**不要**把「`createExtractor()` 非空」当成「文件可播」的证据。

## 维护规则

1. 新增格式 = 新提取器 + 注册 + 本表加行 + 更新一致性用例里的期望列表 + 覆盖度矩阵加一行素材，五件事一起做。
2. 摘除格式 = 删注册 + 删映射/枚举 + 本表移到「不支持」+ 同步用例。
3. `FileSearch` 以 `defaultExtractorMap` 作为扫描白名单，所以本表的增删会直接改变播放列表能看到什么。
