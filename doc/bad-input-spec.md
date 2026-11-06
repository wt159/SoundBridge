# 坏输入规格

本文件回答一个问题：**文件坏了，SoundBridge 应该做什么。**

读完这一份，写用例的人不需要再做任何决定：每类坏法都有定义、期望行为、错误码、用户可见提示、数值判据和素材清单。**实现不在本文件范围内**，代码动作集中列在第 10 节，按优先级排好，逐条对应到文件与行号。

来源是地图 [#22](https://github.com/wt159/SoundBridge/issues/22) 下的十张决策票（#23~#31 与汇总票 #32）。与 `doc/format-support.md` 的分工：那份讲**好文件**能支持到什么程度，这份讲**坏文件**该退化到什么程度。两份不重叠，也不互相引用判据。

## 0. 四条基石

后面所有条款都是这四条的推论。改动任何一条都要先改这里。

### B1 处置只看「解出多少帧」，不按格式分叉

能解出，就播；解不出，就拒绝。**拒绝是兜底，不是策略**。

推论：同一个坏法在 wav 上「一个字不播」、在 flac 上「播 99.75%」，这是实现缺口，不是规格例外。全仓只有一处记录在案的例外——ogg 的截断（见 3.3），原因是拒绝发生在第三方 demux 里，自有代码没有着力点。

### B2 拒绝的唯一出口是 `createExtractor() == nullptr`

「能不能播」只由工厂的返回值回答，调用方不再各自判断。工厂在出口处把三件事合并：

```text
creator() 返回对象
  ├─ initCheck() != OK                        → delete，返回 nullptr
  ├─ spec.sampleRate <= 0 || numChannel <= 0  → delete，返回 nullptr
  └─ 否则                                      → 返回对象
```

第二条不是新规矩，是**没兑现的旧契约**：`sdk/extractor/ExtractorHelper.hpp:46-48` 白纸黑字写着「sampleRate / numChannel 必须有效（> 0），`ExtractorFactory::createExtractor` 在出口校验，不合格直接返回 nullptr」，而实现（`ExtractorFactory.cpp:143-149`）只打了一条 `LOGW`。

`initCheck()` 保留原义（初始化做了什么、失败在哪），但**不再作为「能否播放」的判据**被调用方使用。

### B3 错误码只表达原因，处置交给 `ErrorInfo`

`ErrorInfo` 已经有 `action`（`SkipTrack` / `StopPlayback` / `CheckFile`）和 `recoverable`。错误码再表达「该怎么办」就是两处真源，必然打架。

见 `docs/adr/0001-error-code-expresses-cause.md`。

### B4 量测仪器是播放路径，不是离屏整段解码器

判据要回答的是「用户听到多少」。`AudioDecodeProcess` 在解码失败时**丢弃已解部分**（`AudioDecodeProcess.cpp:100-104` / `:129-131` 在 `mergeDecodeBuffer()` 之前就 `return`），而 `AudioStreamDecoder` 每帧就 `writeDecodedBytes()` 进 ring（`AudioStreamDecoder.cpp:428-431`），错误只改状态（`:368-370`）。

同一个 90% 截断的 ape 文件，离屏解码器报 0，用户耳朵里是 90%。**判据统一用 `start()` + 消费 ring。**

## 1. 名词

完整词表在 `GLOSSARY.md`，这里只列坏输入用到的五个词和四个判据量。

| 词 | 含义 |
|---|---|
| **截断** | 文件在尾部被截短，声明的长度大于实际可读字节数 |
| **坏帧** | 流内某个帧坏了，但它**之后还有完好的数据** |
| **头坏** | 头部或必需结构自身不自洽：读不出结构、读出的值违反格式硬不变量、或解析不终止 |
| **容器声明撒谎** | 头部自洽，但与实际数据不符（时长字段写大、采样率写错） |
| **不可检测** | 该格式的数据里没有第二来源可供比对，坏法在字节层面不可判 |

| 量 | 定义 |
|---|---|
| `good` | 同一素材、同一条解码路径、未做任何改写时的解码时长。**本次运行内实测定，不落库** |
| `r` | 字节截断比例。用例自己切的，所以已知 |
| `expect` | `r × good` |
| `band` | `expect ± max(5% × good, 150ms)` |

判据**不读坏文件自报的任何时长**。这是「既截断又撒谎」不会打架的唯一原因。

## 2. 总表：坏法 × 格式

一格一个期望行为。带 `缺口N` 的表示现状与规格不符，编号见下方的对照表。

| 格式 | 截断 | 坏帧 | 头坏 | 容器声明撒谎 | 超大元数据 |
|---|---|---|---|---|---|
| `.wav` | 播到坏处 `缺口1` | **不可检测** | 拒绝 | 时长不适用；sr/ch 不可检测 | 无自有 tag 读取 |
| `.aiff` | 播到坏处 `缺口1` | **不可检测** | 拒绝 | 同 wav | 无自有 tag 读取 |
| `.mp3` | 播到坏处（静默，见第 9 节） | 跳过继续 `缺口7` | **不可检测**（无必需头部） | 不适用（无独立时长字段） | 3MB 上限，超限跳过继续 |
| `.aac` | 播到坏处（静默，见第 9 节） | 跳过继续 `缺口7` | 拒绝 `缺口5` | 不适用 | 跳过 ID3 前缀 `缺口10` |
| `.flac` | 播到坏处 `缺口11` | 跳过继续（MD5） `缺口6` | 拒绝 `缺口2` `缺口3` | 半可判，表现为 `DataTruncated` | PICTURE 只跳不分配，无影响 |
| `.ogg` | **拒绝**（唯一例外） | 跳过继续（`OV_HOLE`） `缺口8` | 拒绝 | 已免疫（末页 granule） | 不解析 comment |
| `.m4a` | `moov` 在尾→拒绝；faststart→播到坏处 `缺口11` | 跳过继续 `缺口7` | 拒绝 | 落地后免疫 `缺口9` | ffmpeg 侧，不设限 |
| `.asf` / `.wma` | 播到坏处 `缺口11` | 跳过继续 `缺口7` | 拒绝 `缺口4` | 落地后免疫 `缺口9` | ffmpeg 侧，不设限 |
| `.ape` | 播到坏处 `缺口11` | 跳过继续 `缺口7` | 拒绝 | 落地后免疫 `缺口9` | ffmpeg 侧，不设限 |
| `.mkv` | 播到坏处（静默，见第 9 节） | **不可检测** | 拒绝 | 已免疫（逐包时间轴） | ffmpeg 侧，不设限 |

缺口编号对照（这里的编号与第 10 节的实现条目号是两套，不要混）：

| 编号 | 缺口 | 优先级 | 实现条目 |
|---|---|---|---|
| `缺口1` | wav / aiff 短读即全拒，不 clamp | P1 | #6 |
| `缺口2` | flac 整数除零，坏头直接崩进程 | P0 | #1 |
| `缺口3` | flac 16MB 栈 VLA | P0 | #2 |
| `缺口4` | asf / wma 死循环 + 日志洪水 | P0 | #3 |
| `缺口5` | aac 不校验 `syncWord` / `channelConfiguration` | P0 | #4 |
| `缺口6` | flac 的 CRC 只进日志，没有整段校验 | P1 | #10 |
| `缺口7` | ffmpeg 路径一坏就整段停 | P1 | #11 |
| `缺口8` | ogg 的 `OV_HOLE` 当失败；坏帧时 init 成功却 0 字节 | P1 | #12 #13 |
| `缺口9` | `durationMs` 读容器字段而非数据时间轴 | P1 | #16 |
| `缺口10` | aac 的 `init()` 不跳 ID3 前缀 | P1 | #19 |
| `缺口11` | EOS 时未比对解出样本数与标称时长，截断静默通过（只对 flac/ape/m4a/asf 有意义，D 类没有可对的声明） | P1 | #23 |

四条读表须知：

- **「播到坏处」不等于「静默」**。P1 截断必须报 `DataTruncated`，只有 P0（切在音频之后）才静默。
- **「不可检测」是结论，不是待办。** 它锁在反向用例里，见第 9 节。
- **「拒绝」在表格里是一句话，实现里是唯一出口**（B2），不是九个格式各写一遍。
- **同一行里「播到坏处」+ `缺口11` 是常见组合**：这些格式在播放路径上其实已经能播出部分（B 类实测 90%），缺的是「说出来」。

### 2.1 矩阵覆盖对照（#41 收口）

下表把 §2 总表的每一格对到可执行的用例。全部用例在
`sdk/test/unit/test_bad_input.cpp` 的 `TEST_SUITE("BadInput")` 下，共 **55 条 / 396 断言**，
没有新增 ctest 分组。`...` 表示同构用例的公共后缀。

| 坏法 | 覆盖的格子 | 用例 | 断言 |
|---|---|---|---|
| 截断 P1 | wav ×3、aiff ×1 | `wav truncated in the middle...`、`wav truncated at a different ratio...`、`wav missing only its last 128 bytes...`、`aiff truncated in the middle...` | `measured ∈ r × good ± max(5%×good,150ms)`，报 `DataTruncated` |
| 截断 P1（EOS 汇总） | flac | `flac truncated at 60 percent reports truncation instead of silent end` | 短于标称 > 5pp 必须报码（条目 23） |
| 截断 P0 | mp3、mkv | `mp3 cut inside the trailing tag stays silent`、`mkv cut at the tail stays silent` | 完整且 `Ok` |
| 截断 P2 | wav、ogg、m4a | `wav cut below one frame is rejected...`、`ogg truncation is rejected...`、`m4a with its trailing moov cut off is rejected` | ring 0 字节 + 报错 |
| 截断（D 类不可检测，反向） | aac | `aac truncation stays silent while the ratio judgement still holds` | 判据仍成立，但**不断言错误码** |
| 坏帧（跳过继续） | flac、mp3、aac、ape、m4a、asf、ogg | 7 条 `... bad frame plays to the end and reports corruption`（含 `ogg bad page...`） | `measured ≈ good`、一律 `DataCorrupted`、**不得** `DataTruncated` |
| 坏帧（护栏） | ffmpeg 路径 | `ffmpeg path gives up after 32 consecutive undecodable packets`、`a long run of undecodable packets is abandoned...` | 连续失败 ≥32 帧放弃 |
| 坏帧（不可检测，反向） | wav、aiff、mkv | 3 条 `... bad frame stays silent because it is undetectable` | 断言「确实检不出」 |
| 头坏（拒绝） | aac ×2、flac、工厂 ×4、9 格式 ×1 | `aac header with zero sync word...`、`aac header with zero channel configuration...`、`flac streaminfo with zero sample rate...`、`factory rejects...` 系列、`every rejected format exits through createExtractor returning null` | 唯一出口 `createExtractor() == nullptr` |
| 头坏（进程安全） | flac、asf、wma | `flac oversized metadata block...`、`asf object with zero size...`、`wma object with zero size...` | 不崩、不挂死、不写日志洪水 |
| 头坏（不可检测，反向） | mp3 | `mp3 zeroed head stays undetectable because there is no required header` | 断言「确实检不出」 |
| 超大元数据 | mp3 ×2、aac、批量 | `mp3 tag declaring more than the file holds...`、`aac with an id3 prefix...`、`batch scan does not count tag-wrapped files as rejected` | 零容差：有/无 tag 逐项相同 |
| 声明撒谎（时长） | m4a、asf、flac | `m4a duration follows the packet timeline...`、`asf duration follows...`、`flac total samples lie does not change how much audio is decoded` | 零容差差分（改声明后逐位不变） |
| 声明撒谎（不可检测，反向） | wav ×2、m4a | `wav sample rate lie stays undetectable`、`wav channel count lie stays undetectable`、`m4a container spec fields never reach spec or payload` | 断言「确实检不出」 |
| 上报链路与交互 | — | `corrupted mid-stream reports DataCorrupted...`、`player reports decode error instead of stalling silently`、`batch scan reports a summary...`、`auto skip ...` 等 6 条 | 错误码、文案、autoSkip；seek 后 EOS 用 `m_seekBaseMs + 解出时长`（#49，增量接缝见 §13） |

**结构性不覆盖**（总表里写了期望，但不属于「可造的坏文件」）：

- 「超大元数据」列里 wav / aiff / flac / ogg / mkv 的格子写的是「无自有 tag 读取」
  「不解析 comment」「ffmpeg 侧不设限」——描述的是**代码结构**，由 #31 的 clamp 清单
  与提取器工厂的结构性用例覆盖，不逐格造素材。
- 截断 × asf / ape / m4a-faststart 三格：判据与 wav / aiff 完全相同（T1 不按格式分叉），
  走的是同一条 EOS 汇总实现路径，已由 P1 组锁住；不重复跑流式解码（见 2.2）。
- **增量接缝（#50）**：#47 把 demux 生命周期挪到播放期后，原 init 期判死的 m4a `moov` 断 /
  ogg 页链断改判「播放期包耗尽 + ring 累计 0 字节 + EOS 汇总」，属 B1 兜底、不是第二拒绝出口；
  相应 2 条用例只移断言位置，矩阵不增行不增用例，详见 §13。

**跨格式一致性**由两处锁定，不另起重复解码的用例：总表「坏帧」一行的七个格式各有
一条用例、全部断言同一个 `DataCorrupted` 并显式排除 `DataTruncated`；「头坏」的拒绝格
由 `every rejected format exits through createExtractor returning null` 一条遍历九个格式
锁住唯一出口。再跑一遍同样的音频不带来新信息。

### 2.2 耗时：交付时的估算已更正

条目 28 交付时估「整套 < 3 s」。实测（2026-11-04，Windows / MinGW）：
**55 用例 / 396 断言 / 91.6 s**，`ctest -R sdk_` 全套约 120 s。

原因：基石 B4 要求量测仪器是**流式播放路径**，每条用例要把整份素材解到 EOS，
ring 背压按 5 ms 轮询（`drainUntilSettled()` 的 `pollMs=5`）；素材是 `music/` 里几十秒到数分钟的成品。3 s 是按
「15 份素材 × 字节手术」估的，没有算流式解码本身的开销。

**决定：不削用例。** 换成短片段会改掉 `r × good` 的锚，动到所有截断与坏帧判据；
省下的一分钟 CI 时间远不值这个风险。规格里的 3 s 以本节实测值为准。

## 3. 截断

### 3.1 定义与现状

文件在尾部被截短，声明的长度大于实际可读字节数。截断**不需要新增任何判据**——它只是让「解出的帧数」少于应有的帧数。

同样丢 128 字节，十个格式的结局可以归成四类（实测，`build/_agent/probe-r27/`）：

| 现状分类 | 格式 | 砍 128 B（≤0.016%）的结果 |
|---|---|---|
| **A 全拒绝** | wav、aiff、ogg | `initCheck` 失败，0 字节 |
| **B 解码期失败** | ape、m4a、asf | 提取器 init 成功、`declared` 恒为原值，解码返回 −40 |
| **C 部分成功** | flac | 296960 / 297693 = **99.75%** |
| **D 不可见** | mp3、mkv、aac | 100%（砍掉的是尾部 tag / 非音频区）或不差一帧 |

A 类里 wav/aiff 是**实现缺口**：`WAVExtractor.cpp:225-230` 与 `AIFFExtractor.cpp:132` 先按头里声明的长度分配缓冲，再 `readAt`，短读即 `NO_INIT`。改成 clamp 就变成 C 类。ogg 是第三方 demux 的页链校验，记为已知例外。

B 类的 `declared` 恒为原值（ape 恒 24869、m4a 恒 69892），D 类的 `declared` 会跟着文件缩水（mp3 207542→186788/124524/62260）——两种方向相反，所以**期望值只能锚在 `good` 上**。

### 3.2 期望行为：按切点落在哪一段分档

不需要区分「砍在帧中间」还是「砍在帧边界」——解出来的一定到最后一个完整帧为止，差别在一帧之内，判据容差吸收。真正要分的是切点落在文件的哪一段：

| 档 | 切点位置 | 期望 | 错误码 | 用户看到 |
|---|---|---|---|---|
| **P0 音频之后** | 尾部 tag / 索引 / footer（ID3v1、mkv 尾部非音频区） | `measured == good`，**且无错误** | 无 | 什么都不提示 |
| **P1 音频中间** | 任意 `r`，且至少解出 1 帧 | `measured ∈ band` | `DataTruncated` | 文件可能不完整，已播到 mm:ss |
| **P1₀ 一帧都解不出** | 极短文件 / 切在首帧前 | `measured == 0` 且报错 | `DataTruncated` | 文件可能不完整 |
| **P2 必需结构中间** | m4a 的 `moov`、wav/aiff 头之后不足一帧、ogg 页链断开 | 拒绝，ring 累计 **0 字节** 且报错 | `DataTruncated`（能判定短读时）/ `ExtractorInitFailed`（判定不了时） | 文件可能不完整 / 文件损坏 |

P0 在生产环境不需要任何特殊处理——它天然表现为「一切正常」。用例里判 P0 靠**切之前就确认尾部那 N 字节不是音频**（mp3 检查末 3 字节是否为 `TAG`），切完不满足就 SKIP 而不是 FAIL。

### 3.3 条款

**T1 处置统一成一句：能解出多少就播多少。** 拒绝只是没有别的选择时的兜底。判据只看「解出多少帧」，不按格式分叉。

**T2 wav / aiff 必须 clamp。** 把「声明的数据长度」改成 `min(声明长度, 文件实际剩余)`，按 clamp 后的长度读、按 clamp 后的长度算可播样本。`durationMs` **仍报头里声明的值**——文件确实声称那么长，符合「标称时长」，也保住了「实测短于标称」这个可判定的信号。这一处修完，wav/aiff 从 A 类变 C 类，判据自动统一；同一行还顺带堵住 #24 记的 4GB 分配。

**T3 ogg 不修，写死为已知例外。** 拒绝来自第三方 demux 的页链校验，自有代码没有 clamp 的着力点。规格明确「ogg 的截断一律拒绝」，避免以后被当成同一类 bug 反复提。

**T4 正常 EOS 时短于标称超过 5 个百分点，必须报 `DataTruncated`。** 现在 flac 截到 60% 是静默 `Completed`，用户看到进度条走到 177 秒就结束、界面还写着 297 秒。5pp 不是新拍的数：正常文件的量化误差（编码器 padding、seek 偏移、asf 的 +0.20%）都在它以内。

D 类（mp3/aac/mkv）不受 T4 影响——它们的标称时长本来就跟文件一起缩水，`measured ≈ declared`，天然不触发。这不是漏判，是「文件本身没有声称更长」。

**T5 提示文案「文件可能不完整」必须与「文件损坏」分开。** 可操作性不同：不完整意味着「重新下载/拷贝可能就好了」，损坏意味着「这个文件废了」。措辞归 app 层，本规格只要求「必须不同、必须带已播到的位置」。

**T6 判据带宽 `± max(5% × good, 150ms)`，相邻用例的 `r` 相差 ≥ 0.2。** 5pp 的依据：实测「字节比例 → 时长比例」最大偏 **2.07pp**（flac 在 r=0.90），沿用覆盖度基线的 1% 会直接假红。150ms 覆盖 ogg 一页 / flac 一帧（4096 采样 ≈ 93ms）的量化。r 差 ≥ 0.2 保证 10pp 的带子互不重叠。

### 3.4 素材清单

全部**字节手术合成、不入库**，写到系统临时目录（`TMPDIR`/`TEMP`，缺省 `./badinput_tmp`），跑完删。读源文件必须走 `FileSource`（中文名在 Windows 上只有它能读对）。

| # | 源素材 | 手术 | 期望 | 档 |
|---|---|---|---|---|
| 1 | `小镇姑娘-陶喆.128.mp3` | 砍末 128 B（先断言末 3 字节为 `TAG`） | `measured == good`，无错误 | P0 |
| 2 | `music-mkv.mkv` | 砍末 128 B | `measured == good`，无错误 | P0 |
| 3 | `小镇姑娘-陶喆.flac` | 砍到 60% | `measured ∈ band` + `DataTruncated` | P1 |
| 4 | `music.wav` | 砍到 60% | `measured ∈ band` + `DataTruncated`（T2 修好前是拒绝，用例先标已知失败） | P1 |
| 5 | `俱乐部音乐循环-Freesound.aiff` | 砍到 60% | 同 #4 | P1 |
| 6 | `music-ape.ape` | 砍末 1 KB | `≈ good`（99.9%）+ `DataTruncated` | P1 |
| 7 | `萨克斯机.asf` | 砍末 1 KB | `≈ good` + `DataTruncated` | P1 |
| 8 | `WavinFlag.aac` | 砍末 1 KB | `≈ good` + `DataTruncated` | P1 |
| 9 | `Ringtones-耳聆网.ogg` | 砍到 60% | 拒绝：ring 累计 0 字节且报错 | P2（例外） |
| 10 | `摇滚乐_Freesound.m4a` | 砍末 1 KB | 拒绝（`moov` 在尾部被切断） | P2 |
| 11 | `music.wav` | 砍到 20 字节 | 拒绝：ring 累计 0 字节 + `ExtractorInitFailed` | P2 |

## 4. 坏帧

### 4.1 定义与判别原则

流内某个帧坏了，但它**之后还有完好的数据**。这一句同时把坏帧和截断分开了，也直接回答了「一次坏就停还是跳过继续」：

> **坏帧之后还有数据，截断之后没有。**
> 截断 → 播到坏处为止（P1）。
> 坏帧 → 跳过这段，继续播到结尾，最后汇报跳过了几次。

一秒钟的干扰不该让后面十分钟都听不到。

### 4.2 现状：十种格式四种行为

实测方法：复制素材，在文件 50% 处零化一段字节（wav/aiff/flac/mp3/mkv/m4a/asf 400 B，ogg 100 B，ape/aac 200 B），解码后与完好文件比对「解出字节数 + PCM 的 FNV-1a」。

| 格式 | 解出 | 与完好比 | 现状 |
|---|---|---|---|
| wav、aiff、mkv | 等长 | 内容变了 | 解出等长垃圾，**无任何信号** |
| flac | 与完好完全一致（52513104 B） | 内容全变 | 日志里 3 次 `LOST_SYNC`，API 上什么都没有 |
| ogg | **0 字节** | — | init 成功、不解码、不报错 |
| mp3、aac、ape、m4a、asf | 0 字节 | 已解的 50% 一起丢 | **一坏就整段停** |

flac 那条的日志证据：`FLACDecode.cpp:272-275` 的 `error_callback` 只做了一件事——`LOGE`。**检测到了，但没人用它。**

### 4.3 检测路径

| 格式 | 检测依据 | 说明 |
|---|---|---|
| `.flac` | `STREAMINFO` 的 MD5 整段校验 | 打开 `FLAC__stream_decoder_set_md5_checking(true)`，EOS 时不符即损坏。**不能靠 `error_callback` 的调用次数**——实测 3 次 `LOST_SYNC` 而样本数一个不差，分不清是「跳了帧」还是「解了垃圾」 |
| `.ogg` | `ov_read` 的 `OV_HOLE(-3)` | 视为「跳页继续」，不再当成 `-1` 整段失败（`VorbisDecode.cpp:257-266`） |
| ffmpeg 路径（mp3/aac/ape/m4a/asf/mkv） | 单包 `decode() < 0` | 只跳 `AVERROR_INVALIDDATA`、`AVERROR(EINVAL)` 这类**单包**错误 |
| `.wav`、`.aiff`、`.mkv` | **无** | 没有帧级校验，坏字节就是错误样本，长度不变、内容变了 |

`AudioDecode.cpp:327-332` 现在把所有 `ret < 0` 一视同仁地 `return ret`，规格要求分两类：

- **单包解不出** → 跳过该包、计数、继续。
- **解码器/上下文级失败**（codec 打不开、`AVERROR(ENOMEM)`、格式不支持） → 中止并上报，与现状一致。

护栏：**连续失败 ≥ 32 帧（或 ≥ 500ms）就放弃**，上报 `DataCorrupted`。没有这条，一份纯乱码的 mp3 会让解码器把整个文件扫一遍找同步点。两种写法都接受，取实现省事的那个。

### 4.4 条款

**F1 跳过坏帧继续，全部解完后汇总一条 `DataCorrupted`。** 现状 10 个格式给出 4 种行为，这 4 种都要收敛到这一条。

**F2 优先级：同时满足「有帧级错误」和「短于标称」时，报 `DataCorrupted`。** `DataTruncated` 只留给「没有任何帧级错误、单纯短了」的情况。避免一次坏文件挂两条错误把用户搞晕。

**F3 判据只断言两件事，跳帧数与 PCM 内容都不断言。**

| 断言 | 内容 |
|---|---|
| 播放长度 | `measured ≈ good`（`band`，同 T6）——证明**没有因为坏帧提前结束** |
| 错误 | 有且只有一条汇总 `DataCorrupted`（`recoverable=true`、`action=SkipTrack`），**不得是 `DataTruncated`** |
| PCM 内容 | **不断言**。坏帧必然改变样本，比内容只会把用例绑死在某个 libFLAC/ffmpeg 版本的容错策略上 |
| 跳帧计数 | **不断言**。只进日志与 `detail`，不做机器可读字段 |

**F4 不可检测的格式明确写「不可检测」，不许伪造信号。** wav / aiff / mkv 的坏帧不产生任何错误，也不允许用「时长对不上」之类的旁证伪造成 `DataTruncated`。**「没报错」不等于「文件没问题」**——这是数据格式的极限，不是漏洞。

### 4.5 素材清单

「坏得可预测」的含义：**只有指定区间不同，其余字节完全等于原文件**。用 `ffprobe -v error -show_packets -of json` 取第 N 个包的 `pos`/`size`，把这个区间整段零化；生成后**逐字节比对原文件，断言只有该区间不同**——这条断言写进用例。ffprobe 给不出包边界时（如 ogg 页）退化为「对齐到页后零化一页」，并在用例注释里写明退化原因。

| # | 源素材 | 手术 | 期望 |
|---|---|---|---|
| 1 | `小镇姑娘-陶喆.flac` | 零化第 1/3 包 | `measured ≈ good` + `DataCorrupted`（走 MD5） |
| 2 | `像我这样的人-毛不易.128.mp3` | 零化一个包 | 同上 |
| 3 | `WavinFlag.aac` | 零化一个包 | 同上 |
| 4 | `music-ape.ape` | 零化一个包 | 同上 |
| 5 | `摇滚乐_Freesound.m4a` | 零化一个包 | 同上 |
| 6 | `萨克斯机.asf` | 零化一个包 | 同上 |
| 7 | `Ringtones-耳聆网.ogg` | 零化一页 | 同上（走 `OV_HOLE`） |
| 8 | `music.wav` | 零化 400 B | `measured == good` **且无错误** |
| 9 | `俱乐部音乐循环-Freesound.aiff` | 零化 400 B | 同上 |
| 10 | `music-mkv.mkv` | 零化 400 B | 同上 |

素材 #8~#10 是**反向用例**：它们锁住「不可检测」这条结论，防止将来有人加了个假信号让它们「变绿」。

## 5. 头部损坏

### 5.1 定义

头部或必需结构**自身不自洽**：读不出结构、读出的值违反格式硬不变量、或解析不终止。与第 6 节的分界线只有一条：

> **头坏 = 头自身不自洽（不需要看音频数据就能判死）。**
> **声明撒谎 = 头自洽但与数据不符（必须解码才能发现）。**

硬不变量只有两个：`sampleRate > 0`、`numChannel > 0`。其余字段（`durationMs`、`bitRate`、`bitsPerSample`）都不是硬不变量——任何能用的头都可能有偏，一律交第 6 节。

### 5.2 现状：三种结局，其中两种不是「拒绝」

实测：11 份素材做三种**长度不变**的字节手术，走「`FileSource` → `createExtractor(ext, sniff=true)` → `initCheck()` → `AudioStreamDecoder::start()`」。`magic0` = 前 12 字节归零；`struct128` = 12..140 归零（魔数留着，紧跟其后的必需结构被毁）；`head512` = 前 512 字节归零。

| 格式 | `magic0` | `struct128` | `head512` |
|---|---|---|---|
| wav / aiff / ogg / m4a / ape / mkv | −19 | −19 | −19 |
| **mp3** | **0**，spec 与完好逐项相同 | **0** | **0** |
| **aac** | **0，但 sr=96000 ch=0 dur=0** | 0，spec 正常 | **0，但 sr=96000 ch=0 dur=0** |
| **flac** | −19 | **进程崩溃（整数除零）** | −19 |
| **asf / wma** | −19 | **死循环（>25 s 不返回）** | **死循环** |

三个致命缺口：

1. **flac 崩溃**：`struct128` 抹掉 `STREAMINFO` 头，块头 `blockSize` 读成 0，`FLACExtractor.cpp:78-82` 用 `uint8_t data[size]`（VLA）读 0 字节并「成功」，于是 sr/samples 全 0，随后 `FLACExtractor.cpp:113` 除零，进程直接死（exit `0xC0000094`）。同一个 VLA 还有第二条路：`blockSize` 最大 `0xFFFFFF` = 16MB **栈**分配，超过 Windows 默认 1MB 栈就是栈溢出。
2. **asf/wma 死循环 + 日志洪水**：`ASFExtractor.cpp:131` 的 `while (offset <= fileSize)`，推进语句在 `:176`（`offset += obj.size - sizeof(objectHeader)`）。损坏后 `obj.size == 0`，`0 - 24` 在无符号下回绕，`offset` 反而退回原处，于是原地打转。6 秒内写了 **27128 条** `id=..., size=0` 加 27128 条 `Unknown object, skip it`——**每秒约 9 万行**，一个坏 wma 能把磁盘写满。同一份文件 ffprobe 是「秒失败」的，所以问题在我们自己的兜底解析循环。
3. **aac 不校验头**：`AACExtractor::init()` 读 ADTS 头后**从不检查 `syncWord == 0xFFF`**，也不检查 `channelConfiguration`（`AACExtractor.cpp:105-123`），最后无条件 `return NO_ERROR`（`:150`）。全零头被解析成 `sr=96000 ch=0 dur=0`，`initCheck()` 说 OK，`start()` 返回 0 且状态 running——**一份垃圾头被当成正常文件放进了播放列表**。

mp3 反而没事：裸流没有「必需头部」，ID3 被抹掉之后照样扫到同步字播下去。这是结论，不是缺陷。

### 5.3 扩展名与魔数打架

拿一份**完好的** `music.wav`，只改扩展名：

| 扩展名 | 走哪个提取器 | 结果 |
|---|---|---|
| `.wav` | WAV | init=0，正常 |
| `.xyz`（未注册） | sniff 命中 wav | init=0，**正常** |
| `.mp3`（已注册） | MP3 | **−19，被判「文件损坏」** |
| `.flac`（已注册） | FLAC | **−19** |
| `.aac`（已注册） | AAC | init=0 但 `sr=88200 ch=1 dur=0`（垃圾 spec，照放） |

根因在 `ExtractorFactory.cpp:187-200`：**扩展名一旦命中注册表就直接构造，永不看魔数**；只有扩展名未注册时才走 `sniffExtensionByMagic()`。于是「后缀写错」的处置完全取决于写错成了什么——写成一个不认识的反而救了回来，写成另一个认识的直接判死。用户无法理解。

### 5.4 条款

**H1 拒绝的唯一出口是 `createExtractor() == nullptr`**（B2）。

**H2 硬不变量只有 `sampleRate > 0` 与 `numChannel > 0`。** 违反它们不需要看任何音频数据就能判死，所以归本节。

**H3 错误码按「扩展名是否注册」分，不按「解析失败」分。**

| 事实 | 错误码 | 用户看到 |
|---|---|---|
| 扩展名已注册，工厂返回 nullptr | `ExtractorInitFailed` | 文件损坏，无法播放 |
| 扩展名未注册，魔数也认不出 | `ExtractorUnsupported` | 格式不支持 |
| 扩展名未注册，魔数认出受支持格式 | 正常打开（现状已如此） | — |
| 扩展名已注册，init 失败，但魔数认出**另一种**受支持格式 | 用魔数那一种打开 | — |

**H4 原扩展名的提取器 init 失败后，做一次魔数回退**；命中即用，不递归。理由就是 5.3 那张表——「改成不认识的后缀能播、改成认识的后缀不能播」是自相矛盾，而魔数探测这条通路本来就存在，只是被扩展名短路了。回退不削弱损坏判定：真损坏的文件魔数位通常先坏，回退也命中不了，最终仍是 `ExtractorInitFailed`。

判定「扩展名是否注册」需要查询口，加 `ExtractorFactory::isSupportedExtension()`，**不要靠 catch nullptr 再猜**。

**H5 拒绝时不调用提取器的任何数据接口。** `initCheck() != OK` 时禁止调 `getMetaData()` / `getPacketizedMetaData()` / `seekToMs()` / `getDataSize()`——它们内部的缓冲在 init 失败时可能没分配，现在的「安全」只是碰巧。这也回答了「拒绝后还显示封面吗」：SDK 根本没有标签 API（`ID3` 只用来读 `iTunSMPB`，`getAlbumArt()` 零调用，标题来自文件名），没有这条通路可做。

**H6 拒绝必须可见，且不许被 `shouldNotify` 吃掉。** `MusicPlayList.cpp:337-354` 只在「当前曲目」时回调 UI，后果是**批量扫描时除当前曲目外，被拒的文件只有日志**。规格：用户主动打开的文件被拒必须 `onError`；批量扫描每份被拒的文件至少进一份汇总（「N 个文件无法播放」），允许逐条上报但有上限，**不得完全静默**。被拒文件不进播放列表（现状正确，保持）——所以它永远不会成为「当前曲目」，这条通路只能靠汇总补。

**H7 mp3 的「头坏」不可检测，写成「无必需头部」。** mp3 的 ID3 损坏 = 标签损坏，不属于头坏，处置归第 7 节（跳过 tag 继续播）。aac 相反——它能坏而不被察觉，所以必须靠 `syncWord` 与 `channelConfiguration` 拦下来。

### 5.5 素材清单

三种手术都是**长度不变**的（不新增字节、不删字节），所以「文件没坏」的旁证不成立，只有提取器能分辨。

| 手术 | 字节区间 | 模拟 |
|---|---|---|
| `magic0` | `[0, 12)` 归零 | 魔数被毁 |
| `struct128` | `[12, 140)` 归零 | 魔数留着，紧跟其后的必需结构被毁 |
| `head512` | `[0, 512)` 归零 | 整个头没了 |

11 份素材全跑；关键断言五条：

| # | 素材 + 手术 | 断言 |
|---|---|---|
| 1 | 全部 11 份 + `magic0` | 除 mp3 外，`createExtractor(...) == nullptr`，错误码 `ExtractorInitFailed` |
| 2 | mp3 + `magic0` | `createExtractor(...) != nullptr` 且能解出与完好**等长**的样本（锁 H7） |
| 3 | aac + `magic0` | `createExtractor(...) == nullptr`（今天会是 `!= nullptr` + `ch=0`，用例先红） |
| 4 | flac + `struct128` | 进程不崩；`== nullptr`（今天 SIGFPE，用例先红） |
| 5 | asf / wma + `struct128` | 5 秒内返回 `== nullptr`（今天挂死，需靠 ctest 超时兜底） |

扩展名冲突单独一组：同一份完好 wav，分别用 `.wav` / `.xyz` / `.mp3` 三种扩展名打开，**三者的 `createExtractor()` 必须都非空且 spec 一致**——这一条同时锁住 H4 的魔数回退。

## 6. 容器声明撒谎

### 6.1 定义与实测

头部自洽，但与实际数据不符。实测方法：3 份完好素材做**长度不变**的字节手术，走播放路径，`devSpec` 固定 48000/2ch/s16（PCM 类的撒谎只有重采样时才会显形）。

| 素材 | 改了什么 | 提取器报的 | 实解 | 结论 |
|---|---|---|---|---|
| `music-mkv.mkv` 原件 | 文件本身把 393216 采样当毫秒写成 393.216 s | **24576ms** | 24575 | 已被逐包累加吸收 |
| `m4a_dur_lie` | `mdhd` + `mvhd` 的 duration ×2，`stts` 不动 | **139784ms** | 69891 | **0.500，完全外泄** |
| `flac_dur_lie` | `STREAMINFO.total_samples` ×2 | **595386ms** | 297692 | **0.500，且静默 `Completed`** |
| `wav_sr_lie` | `fmt.sampleRate` 16000→8000，`byteRate` 同步 | sr8000 dur **49739** | 49737 | `declared == decoded`，**不可检测**，实际是 2× 慢放降调 |
| `wav_ch_lie` | `fmt.numChannel` 1→2，`blockAlign`/`byteRate` 同步 | ch2 dur **12434** | 12433 | 0.5× 时长，声道错位成一团垃圾 |
| `m4a_sr_lie` / `m4a_ch_lie` | 改 `stsd` 里 `mp4a` 的 sampleRate / channelCount | 44100 / 2（**原值**） | 69891 | 字段**根本不进决策链**，逐项与原件相同 |

四条事实：

1. **PCM 类（wav/aiff）的采样率/声道撒谎不可检测**——改头之后 `declared` 和 `decoded` 依然 1.000 一致。
2. **ffmpeg 解复用格式的容器层 spec 字段根本不参与决策**——提取器读 `codecpar`，解码器读码流里的 ASC，两者同源。这类撒谎达不到。
3. **时长撒谎只在「有独立时长字段」的格式上外泄**，差别不在格式，在**提取器读的是哪个数**。
4. **flac 的「声明写大」与「文件少了尾巴」在数据上完全一致**——FLAC 帧里只有帧序号/样本序号，没有总数。

### 6.2 条款

**S1 权威来源单点：`durationMs` 由「数据自身的时间轴」推导，容器声明只兜底。**

```text
包/帧时间轴逐包累加  >  帧数 × 每帧样本 / 采样率  >  采样数精确换算（PCM / STREAMINFO）
                    >  容器时间戳（mdhd / ASF File Properties / APE header）
                    >  码率估算（mp3 无 Xing 时）
```

只有前三级可以进 `durationMs`；后两级仅在前面拿不到时作兜底，**并且必须打日志说明用了兜底**。上层（解码器、播放器、断言）不再二次校验——单点。

代价为零：`M4AExtractor.cpp:327`、`APEExtractor.cpp:290`、`ASFExtractor.cpp:322` 现在都写 `fmt->duration / 1000`，而这三个提取器**本来就在 init 期把全部包遍历进 `m_packetizedMetaBuf`**。把 `MKVExtractor.cpp:200-233` 已经写好的累加逻辑搬过去即可，不多一次 IO。

**S2 「差多少算撒谎」没有阈值——判据是差分，不是绝对值。**

> **篡改那个声明字段，`durationMs` 与解出时长必须逐位不变。**

零容差，因为它比的是同一份文件的两次运行，不存在量化误差。绝对值比对仍归既有容差（覆盖度的 `max(1%, 150ms)`、截断的 `±5pp`），不新增第二套数字。唯一新增的数值是**日志降噪用的 1%**（容器字段与推导值差超过 1% 才 `LOGW`），它不参与任何判定。

适用范围：

| 类 | 格式 | 处置 |
|---|---|---|
| 已吸收 | mkv、ogg | 无需改动，用例锁「改字段不变」 |
| 要吸收 | m4a、asf、wma、ape | 按 S1 改成时间轴累加 |
| 半可判 | flac | `total_samples` 是唯一来源，init 期拿不到第二份，只能 EOS 后比对（S4） |
| 不适用 | wav、aiff、aac、mp3 | 时长不是独立字段（`dataSize` / 帧数 / 码率），改它等于改数据 |

**S3 采样率/声道以解码首帧实测为准，且必须无条件覆盖。** 不是「按格式分叉」，是把现在隐含的分支变成硬规则：

- `AudioStreamDecoder::start()` 里**无条件** `emplace` 重采样器工厂（现在只在 `m_srcSpec != m_devSpec` 时创建，`:84`）；
- 首帧回调里**无条件**用 `out.spec` 覆盖 `m_srcSpec`（现在被 `m_lazyResample.isInit() && !IsValueCreated()` 挡住，`:396-407`——头里声明恰好等于设备规格时整块被跳过，实测到的就是头里的错值）；
- 覆盖后 `m_srcSpec != m_devSpec` 才 `Value()`，相等就丢掉工厂。

对 `AUDIO_CODEC_ID_NONE` / `FLAC` / `VORBIS` 覆盖是幂等的（首帧 spec 本来就来自提取器）；对 ffmpeg 路径这是唯一能保证「码流说了算」的位置。

**PCM / FLAC / Vorbis 的采样率不可检测，明确写进规格、接受。** 它们的头是唯一自述，没有第二个来源，撒谎就是变速降调，与第 4 节的「wav/aiff/mkv 坏帧不可检测」同一性质。不许为了「检测出来」去伪造旁证。

**S4 与截断的接缝：不区分，合并到 `DataTruncated`，文案中性化。** 保留 T4 的规则（正常 EOS 时短于标称 > 5pp → `DataTruncated`），**不为撒谎单开错误码**；文案用「文件可能不完整」，因为它同时覆盖「少了数据」和「时长字段写错」两种情况。这条误报是已知且接受的——两者对用户的正确处置相同（能播多少播多少、不拒绝、不跳过），差别只在解释，任何试图区分二者的实现都是在猜。S1 落地后 m4a/asf/ape/ogg/mkv 不再有这条误报，只剩 flac。

**S5 上报只加一条 `LOGW`，不新增错误码、不进 UI。** 音频本身是完好的，报错或弹窗是误伤。提取器在 init 期发现「容器声明 vs 推导值」差 > 1% 时打一条日志（含两个数值和所用来源），供定位坏封装工具。

**S6 记录一个已知后果，不修。** `MusicPlayer.cpp:333` 对 `pos > durationMs` 是静默丢弃（既不 clamp 也不通知），声明偏短时后半首永远拖不到。这是「声明不可信」的必然结果；S1 之后 mkv/m4a/asf/ape/ogg 已经拿到可信时长，**这五个格式的症状随之消失**。

### 6.3 素材清单

| # | 源素材 | 手术 | 断言 |
|---|---|---|---|
| 1 | `music-mkv.mkv` | 改 mkv 时长字段（原件已是错的） | `durationMs` 与解出时长 == 原件 |
| 2 | `摇滚乐_Freesound.m4a` | `mdhd` + `mvhd` duration ×2 | `durationMs` 与解出时长 == 原件（现在 139784，红） |
| 3 | `萨克斯机.asf` | `File Properties` 时长 ×2 | 同上 |
| 4 | `music-ape.ape` | APE header 时长 ×2 | 同上 |
| 5 | `小镇姑娘-陶喆.flac` | `STREAMINFO.total_samples` ×2 | 解出时长 == 原件；EOS 后报 `DataTruncated`（S4 的已知误报） |
| 6 | `摇滚乐_Freesound.m4a` | `stsd` sampleRate 44100→48000 | 解出 PCM 与原件**逐字节相同**，spec 逐项相同 |
| 7 | `摇滚乐_Freesound.m4a` | `stsd` channelCount 2→1 | 同上 |
| 8 | `music.wav` | `fmt.sampleRate` 16000→8000 | **不设断言**——锁的是「不可检测」，写成注释说明为什么没有断言 |

素材 #8 是负面用例：它不给期望值，只留一条注释，防止将来有人给 PCM 加一个假检测。

## 7. 超大元数据

### 7.1 先纠正一个前提

`getMetaData()` 返回的是**音频 payload 的整块视图**（包是唯一表示，整块是按需拼合），**不是标签**。SDK 也没有任何标签 API：`ID3` 类只在 `MP3Extractor.cpp:445` 被用来读 `iTunSMPB`（gapless），`getAlbumArt()` 有定义但**零调用**，UI 标题来自文件名。

所以：

- 「跳过 tag 后标题显示什么」是伪问题——标题本来就不来自 tag。
- 「3MB 上限对带大封面的 mp3 偏紧」不成立——封面不进我们的内存（`getAlbumArt()` 即使被调用也只是返回 tag blob 内的指针，不复制）。

### 7.2 实测：有没有危险，取决于我们读不读

| 素材 | 情形 | 提取器 | 解出 | 判定 |
|---|---|---|---|---|
| `base.mp3` | 4086 B ID3 | sr44100 ch2 dur 295740 | 295731 | 基线 |
| `mp3_id3_2mb.mp3` | 真实 2MB tag（上限内） | 295740 | 295731 | 不受影响 |
| `mp3_id3_10mb.mp3` | 真实 **10MB** tag（超上限） | 295740 | 295731 | **正常播放**，只丢了 gapless 信息 |
| `mp3_id3_huge.mp3` | 声明 **200MB**、文件 4.7MB | **sr=0 ch=0 dur=0** | **0 字节** | 整首不播 |
| `base.aac` | 无前缀 | sr48000 ch1 dur 10048 | 10048 | 基线 |
| `aac_id3_200b.aac` | 真实 **200 字节** ID3 前缀 | **sr=0 ch=4 dur=0** | **0 字节** | **合法文件被当垃圾** |
| `aac_id3_huge.aac` | 声明 200MB | sr=0 ch=4 dur=0 | 0 字节 | 同上 |

三条事实：

1. **「超大」本身不可怕**：10MB 的真实 tag 一点事都没有——`ID3` 类超限直接 `return false`，**根本不 malloc**（`ID3.cpp:169-173`）。上限生效的位置是对的。
2. **真正坏事的是「声明长度与文件长度矛盾」**：mp3 的同步搜索按声明值推进 offset（`MP3Extractor.cpp:195-211`，**没有任何 clamp**），声明 200MB 就把 offset 推到文件外，整首变 0 字节。这不是内存问题，是**定位问题**。
3. **aac 的 ID3 前缀 = 全损，200 字节就够**：`countAdtsFrames()` 明确支持跳 ID3（`AACExtractor.cpp:45-55`），但 `init()` 解析 ADTS 头时**从 offset 0 直接读**（`:102-104`），把 `I`/`D`/`3` 当成 ADTS 字段。两个函数对同一个文件的前提不一致——这是本类里唯一一个「正常文件播不了」的缺陷。

内存风险的实际分布，按「谁分配」逐个过一遍：

| 位置 | 是否按声明分配/跳过 | 现状 | 处置 |
|---|---|---|---|
| `ID3.cpp:169` tag blob | 按声明 malloc | 3MB 上限，超限跳过 | **保持**；顺带 `size` 与文件剩余取 min |
| `MP3Extractor.cpp:195-211` ID3 跳过 | 按声明推进 offset | **无 clamp**，越界即整首失败 | **加 clamp**（M2） |
| `WAVExtractor.cpp:225-230` data chunk | 按声明分配 | 无上限，44 字节头可要 4GB | 已在第 3 节定为 clamp（T2），不重复 |
| `FLACExtractor.cpp:78-82` `STREAMINFO` | 按声明 VLA（**栈**） | 最大 16MB，可栈溢出 | 归第 5 节（`缺口3`），本节只记接缝 |
| `FLACExtractor.cpp:124-129` PICTURE | **不分配**，只 `offset += size` | 16MB 封面无害 | 不处理 |
| `AACExtractor.cpp:128-133` 整文件读入 | 按**实际**文件大小 | 安全 | 不处理 |
| m4a/asf/wma/ape/mkv 的 tag | ffmpeg 内部 | 我们既不读也不用，**无公开 cap 接口** | **不设限、接受** |

### 7.3 条款

**M1 ID3 的 3MB 上限保持不动。** blob 的唯一用途是几十字节的 `iTunSMPB`。超限行为保持「跳过 tag、继续播」。封面不设限，因为它不进我们的内存。

**M2 tag 长度越界时只跳 10 字节头、当 tag 不存在，不拒绝文件。**

```text
declared = syncsafe(header.enc_size) + 10
if declared > fileSize - offset:  next = offset + 10    // 只跳头部，tag 视为不存在
else:                             next = offset + declared
```

理由：声明长度超过文件剩余，说明这个声明不可能是真的，而**音频字节物理上还在**（实测素材里就在 10 字节之后）。按「能解出多少就播多少」退化到「从 tag 头之后重新搜同步字」。

**M3 aac 的 `init()` 必须先跳 ID3**，与 `countAdtsFrames()` 抽出同一段逻辑共用；跳不过（越界）就按 M2 只跳头。

**M4 不造统一防线，换成一条可 review 的规则。**

> 任何按**声明长度**分配或跳过的地方，都必须先 clamp 到 `min(declared, fileSize - offset)`。

全仓现在只有 5 处（ID3 blob、mp3 ID3 跳过、WAV data、FLAC `STREAMINFO`、FLAC PICTURE），逐个核对；新增代码按这条 review。在基类上做「读 tag 前先比文件大小」没有意义——基类根本不知道哪个字段是声明长度。

**M5 ffmpeg 侧的 tag 不设限、接受。** 我们既不读也没有 cap 接口，改 `3rdparty` 是禁区；`m_packetizedMetaBuf` 只收音频流（`MKVExtractor.cpp:202` 判 `stream_index == audioIndex`），附件不会进我们的缓冲。

**M6 跳过 tag 对外完全静默。** `durationMs`、`getAudioSpec()`、`getMetaData()`（音频 payload）、UI 标题全部不受影响，所以不需要提示、不需要错误码。这一条同时否掉「拒绝整个文件」这个选项：拒绝一个能播的文件没有任何收益。

### 7.4 素材清单

| # | 素材 | 手术 | 断言 |
|---|---|---|---|
| 1 | mp3 | 真实 10MB tag | `durationMs` 与解出时长 == 无 tag 版本（295740 / 295731） |
| 2 | mp3 | 声明 200MB、文件 4.7MB | 解出时长 == 只剥 10 字节头的版本（现在 0 字节） |
| 3 | aac | 真实 200B ID3 前缀 | spec 与时长 == 无前缀版本（现在 `sr=0 ch=4`） |
| 4 | flac | 16MB `PICTURE` 块 | init 成功，`durationMs` 与解码量与无封面版本相同 |

全部零容差（比的是「同一段音频的两种包装」），不引入新数字。

## 8. 错误码与用户可见提示

### 8.1 错误码

| 码 | 值 | 含义 | `recoverable` | `action` | 来源 |
|---|---|---|---|---|---|
| `ExtractorUnsupported` | `0x020001` | 扩展名不受支持，魔数也认不出 | false | `SkipTrack` | 不变 |
| `ExtractorInitFailed` | `0x020002` | 头/容器结构坏，一个字都没解 | false | `SkipTrack` | 语义收窄为「头坏」 |
| `DecodeInitFailed` | `0x030001` | 解码器初始化失败（codec 不匹配、缺 extradata） | false | `SkipTrack` | 不变 |
| **`DataTruncated`** | `0x030003` | 文件可能不完整（声明长度 > 实际可读） | **true** | `SkipTrack` | **新增** |
| **`DataCorrupted`** | `0x030004` | 流内坏帧（CRC 错、长度越界、比特流乱码） | **true** | `SkipTrack` | **新增** |
| ~~`DecodeFailed`~~ | ~~`0x030002`~~ | — | — | — | **删除**：全仓零运行时调用者，只有 Info 表和测试引用 |

`DataTruncated` 与 `DataCorrupted` 分开而不是合成一个「坏文件」：两者的期望行为不同——截断是「播完剩下的，然后收尾」，坏帧是「跳过继续」。合成一个码，`action` 就没法填。

**位置复用 `curPositionMs`，不改任何回调签名。** 「坏在哪」已经在手——`MusicPlayer.cpp:458-469` 每次消费都算 `curPositionMs` 并已上报，错误发生时它天然停在坏处。`detail` 只放人类可读的原因（`"frame crc mismatch at frame 412"`），不放机器要解析的结构化数据；断言读 `curPositionMs`，不解析 `detail`。

### 8.2 静默卡死是缺陷，不是设计

> 解码进入 `StreamDecoderState::ERROR` 时，播放器**必须**上报一个可恢复错误；autoSkip 开启则跳到下一首，关闭则**停在坏处并把状态置为 Paused**。

现状是「ring 读空、消费线程只等 EOS、什么都不发生」——播放器继续显示 Playing 但在放空气。这条不写进规格，后面所有「播到坏处为止」就全是空话。判据见第 4 节 F3 与第 3 节 3.2。

### 8.3 用户可见提示

至少三句，按 `action` + 码分档，**不再只看 autoSkip 开关**（现状 `app/playercontroller.cpp:171` 只有 `"SKIPPING ERROR"` / `"PLAYBACK ERROR"` 两个串，且只取决于开关）：

| 场景 | 文案要求 |
|---|---|
| 格式不支持 | 说明格式不受支持，不要暗示文件坏了 |
| 文件损坏无法播放 | 「文件已损坏，无法播放」一类 |
| 播到损坏处已跳过 / 文件可能不完整 | **必须与上一句不同**，且**必须带已播到的位置** |

语言沿用 app 现状（英文 subtitle），不新起一套 i18n。具体措辞归 app 层，本规格只规定「必须分档、必须不同」。

## 9. 不可检测清单

这一节是**结论**，不是待办。每一条都锁在反向用例里（用例断言「确实检不出」），防止将来有人加假信号让它们「变绿」。

| 坏法 × 格式 | 为什么检不出 | 反向用例（断言「确实检不出」） |
|---|---|---|
| 截断 × mp3 / aac / mkv | 标称时长跟着文件一起缩水，`declared == measured`。**判据仍然能判**（用完好文件当锚、期望 `r × good`），只是运行期无法向用户报出来 | `mp3 cut inside the trailing tag stays silent`、`mkv cut at the tail stays silent`、`aac truncation stays silent while the ratio judgement still holds` |
| 坏帧 × wav / aiff / mkv | 无帧级校验，坏字节直接变成错误样本，长度不变 | 3 条 `... bad frame stays silent because it is undetectable` |
| 头坏 × mp3 | 裸流没有「必需结构」可坏，扫到同步字就能播 | `mp3 zeroed head stays undetectable because there is no required header` |
| 声明撒谎（采样率/声道）× wav / aiff / flac / ogg | 头是唯一自述，没有第二个来源；撒谎只表现为变速降调/声道错位 | `wav sample rate lie stays undetectable`、`wav channel count lie stays undetectable`（PCM 代表；aiff/flac/ogg 同构，头同样是唯一自述） |
| 声明撒谎（容器层 spec 字段）× m4a / asf / ape / mkv | 提取器读 `codecpar`、解码器读码流 ASC，容器层字段根本不进决策链 | `m4a container spec fields never reach spec or payload`（m4a 代表，四个格式共用同一条路径） |

这张表统一指**运行期能否检测**。「不可检测」不等于「判据不成立」：截断的判据永远成立（`r` 是用例自己切的），只是生产环境里没有第二来源可比，所以无法向用户报出来。写用例时这一区别很关键——不可检测的格式仍然要断言 `r × good`，只是**不能断言错误码**。

判断标准：**能不能在字节层面找到第二个独立来源**。找不到，就写进这张表，不许用时长旁证伪造检测。

## 10. 实现清单

本图只出清单，不实现。按优先级排，**P0 必须先落地**——它们是崩溃、挂死和 CI 卡死，不是体验问题。

### P0 致命

| # | 问题 | 位置 | 动作 |
|---|---|---|---|
| 1 | flac 整数除零崩溃 | `sdk/extractor/flac/FLACExtractor.cpp:113` | `sampleRate > 0` 才做除法，否则 `durationMs = 0` 并让 init 失败 |
| 2 | flac 16MB 栈 VLA | `FLACExtractor.cpp:78-82` | 换 `std::vector<uint8_t>`；`blockSize` 设上界（`STREAMINFO` 恒 34 字节） |
| 3 | asf/wma 死循环 + 日志洪水 | `sdk/extractor/asf/ASFExtractor.cpp:131/176` | 循环里强制「`offset` 严格前进」；`obj.size < sizeof(objectHeader)` 直接判损坏退出；另设对象数上限。按 `off64_t` 判，别写成 `int` |
| 4 | aac 不校验头 | `sdk/extractor/aac/AACExtractor.cpp:105-123,150` | 校验 `syncWord == 0xFFF`、`channelConfiguration != 0`，不合格 `NO_INIT` |
| 5 | ctest 无超时 | `sdk/test/CMakeLists.txt:174-177` | `set_tests_properties(sdk_unit_tests PROPERTIES TIMEOUT ...)`。没有这条，坏文件挂死会让 CI 卡到默认 1500s |

### P1 语义缺口

| # | 问题 | 位置 | 动作 |
|---|---|---|---|
| 6 | wav/aiff 短读即全拒 | `sdk/extractor/wav/WAVExtractor.cpp:225-230`、`sdk/extractor/aiff/AIFFExtractor.cpp:132` | 改成 `min(声明长度, 文件剩余)` 再读、再算可播样本；`durationMs` 仍报声明值。同一行堵住 4GB 分配 |
| 7 | `ERROR` 状态无人消费（静默卡死） | `sdk/MusicPlayer.cpp` `consumeAudioData()` | 加 `st == ERROR` 分支：上报 + 按 autoSkip 决定跳/停；停时置 `PausedState` |
| 8 | 错误码增删 | `sdk/ErrorCode.hpp/.cpp`、`sdk/include/soundbridge/error.h` | 加 `DataTruncated` / `DataCorrupted`，删 `DecodeFailed`，补 `ErrorInfo` 三行 |
| 9 | 已解部分被丢弃 | `sdk/audio/decode/AudioDecodeProcess.cpp:100-104/129-131` | 二选一：失败分支也 `mergeDecodeBuffer()`；或判据统一走流式路径。**倾向后者**——仪器应当和用户听到的路径同一条 |
| 10 | flac CRC 只进日志 | `sdk/audio/decode/FLACDecode.cpp:272-275` | 打开 MD5 checking；EOS 不符 → `DataCorrupted` |
| 11 | ffmpeg 一坏就整段停 | `sdk/audio/decode/AudioDecode.cpp:327-332` | 分「单包解不出（跳过+计数+继续）」与「上下文级失败（中止）」；连续失败 ≥32 帧或 ≥500ms 放弃 |
| 12 | ogg `OV_HOLE` 当 `-1` | `sdk/audio/decode/VorbisDecode.cpp:257-266` | `-3` 视为跳页继续 |
| 13 | ogg 坏帧 init 成功却 0 字节 | 待定位 | 先定位再加「跳过继续」 |
| 14 | 工厂出口不校验 spec | `sdk/extractor/ExtractorFactory.cpp:143-149` | 追平 `ExtractorHelper.hpp:46-48` 的契约。**留逃生口**：`registerExtractor` 加 `allowEmptySpec`（默认 false），否则会打破 `test_decode.cpp` 里的测试替身 |
| 15 | 无魔数回退 | `ExtractorFactory.cpp:187-200` | init 失败后做一次魔数回退；加 `isSupportedExtension()` |
| 16 | `durationMs` 读容器字段 | `M4AExtractor.cpp:327` / `APEExtractor.cpp:290` / `ASFExtractor.cpp:322` | 抄 `MKVExtractor.cpp:200-233` 逐包累加；差值 >1% 打 `LOGW` |
| 17 | `m_srcSpec` 被惰性门挡住 | `sdk/audio/decode/AudioStreamDecoder.cpp:84`、`:396-407` | 无条件建工厂；首帧无条件覆盖 |
| 18 | ID3 跳过无 clamp | `sdk/extractor/mp3/MP3Extractor.cpp:195-211` | `next = min(offset + declared, fileSize)`；越界只跳 10 字节 |
| 19 | aac init 不跳 ID3 | `AACExtractor.cpp:102` 与 `:45-55` | 抽公共「跳 ID3 前缀」函数，两处共用 |
| 20 | 批量拒绝静默 | `sdk/MusicPlayList.cpp:337-354` | 汇总上报（「N 个文件无法播放」），不得完全静默 |
| 21 | 自动跳过绕圈 | `sdk/MusicPlayer.cpp:597-608` | 同一曲目在一次播放会话内不因坏数据被重试两次 |
| 22 | 提示文案只有两档 | `app/playercontroller.cpp:171` | 按 `action` + 码分三句；「可能不完整」与「损坏」必须不同 |
| 23 | EOS 时未比对「解出样本数 vs 标称时长」 | `sdk/audio/decode/AudioStreamDecoder.cpp`（EOS 汇总处） | 短于标称 > 5pp 时上报 `DataTruncated`（T4）。这是 flac 截到 60% 仍静默 `Completed` 的修法，也是 B/D 类截断唯一能报出来的位置 |

### P2 用例

| # | 内容 |
|---|---|
| 24 | 新建 `sdk/test/unit/test_bad_input.cpp`，`TEST_SUITE("BadInput")`，挂进 `UNIT_TESTS_SOURCES`。**不新增 ctest 分组**（跟随 `sdk_unit_tests`，避免 CI 多起一次进程） |
| 25 | 素材全部字节手术合成、不入库；写临时目录、跑完删；读源走 `FileSource`（中文名必需） |
| 26 | 扩展名冲突组：同一份完好 wav 用 `.wav` / `.xyz` / `.mp3` 打开，三者 `createExtractor()` 均非空且 spec 一致（锁 H4） |
| 27 | 反向用例：#8~#10 的坏帧、第 9 节整张表，断言「确实检不出」 |
| 28 | 规模：源素材都 ≤ 4.8MB、字节手术合成不入库、跑完删；不新增 ctest 分组。**交付时估的「< 3 s」不成立**，实测 55 用例 / 396 断言 / 91.6 s，修正见 §2.2 |

### 一条 ADR

见 `docs/adr/0001-error-code-expresses-cause.md`（错误码表达原因、处置交给 `ErrorInfo`）与 `docs/adr/0002-bad-input-degrades-not-rejects.md`（坏数据的处置只看解出多少帧，不按格式分叉）。

## 11. 已知取舍

这几条是**决定不做**，写在这里是为了避免反复讨论。

| 取舍 | 理由 |
|---|---|
| ogg 的截断一律拒绝 | 拒绝来自第三方 demux 的页链校验，自有代码没有 clamp 的着力点 |
| flac 的「声明写大」与「真的被截断」不区分 | 字节层面完全不可区分，任何试图区分的实现都是在猜 |
| PCM / FLAC / Vorbis 的采样率撒谎不检测 | 头是唯一自述，没有第二来源 |
| wav / aiff / mkv 的坏帧不检测 | 没有帧级校验，不许用时长旁证伪造 |
| ffmpeg 侧 tag 不设上限 | 没有公开 cap 接口，改 `3rdparty` 是禁区，且我们不用 |
| 不新增标签 API、不读封面 | 现有 SDK 没有这条通路，规格不为它开新面 |
| `position > durationMs` 时静默丢弃不修 | 声明不可信的必然结果；S1 之后大部分格式已消失 |
| 不为「差多少算撒谎」设阈值 | 判据是差分，阈值这个问题不存在 |

## 12. 本文件不覆盖

- **空文件 / 非音频伪装扩展名**：已被 `ExtractorUnsupported` 挡住，有素材也有测试。
- **读盘中途 IO 失败、权限、路径编码**：属数据源层（`FileOpenFailed`），与「文件内容坏」不是同一个问题。
- **网络流 / URL / 边下边播**：属数据源层，不在本地文件规格内。另更正：`support stream programme`（#8）的正文是 *Streaming decoding and playback scheme for local music players*，讲的是**本地文件边解边播**，已由地图 #42 收口为 [边解边播规格](streaming-playback-spec.md)；本文件旧版把它写成「网络流」属措辞偏差。
- **播放列表自动跳过的交互重设计**：只判定它该不该覆盖「播到一半才坏」，不改 `m_autoSkipOnError` 的列表逻辑。
- **用例与断言的实现**：见第 10 节 P2。

## 13. 增量路径接缝（2026-11-05，地图 #42 收口）

地图 #42 把生产链改成「包为唯一层间单元 + demux 播放期存活 + 索引 O(1)」后，**本文件整体仍成立、四条基石零改写**（B1 / B3 只依赖累计标量；B4 反被加强：改造后生产链与量测仪器同构）。逐节影响表与 55 条用例清单见 #50 的 resolution comment，规格正文见 [边解边播规格](streaming-playback-spec.md) 第 7 节。这里只记必须落地的接缝：

- **唯一硬约束**：#47 新增的 init 期只读扫描**不得把可解性当判据** —— 坏包跳过继续扫、标称时长允许 `0`、扫描失败不返回 `nullptr`；工厂仍只按 `initCheck()` + 两个硬不变量拒绝。否则会给 B2 的唯一出口开第二个门。现状 `readHeader()` 只读 16 字节、根本没有「扫全文」动作，扫描是 #47 新增的。
- 原 init 期判死的 m4a `moov` 断 / ogg 页链断，改判「播放期包耗尽 + ring 累计 0 字节 + EOS 汇总」，属 B1 兜底、不是第二出口；对应 2 条用例（`ogg truncation is rejected and plays nothing`、`m4a with its trailing moov cut off is rejected`）**只移断言位置**，判据数值不变，迁移顺序由 #52 定为线性四步的第 ③ 步。
- EOS 原样成立、不新增时机，已消费 PCM 算数、不回滚不重播；零容差差分仍比 `declaredMs` / `decodedBytes` / `code` 三个累计标量、不扩展到 PCM 逐位；「不可检测」清单零增删；**#48 新增的四个观测点禁作坏输入旁证**（F4「不许伪造信号」）。
- seek 后 EOS 判定用 `m_seekBaseMs + 解出时长`；`durationMs == 0` 时 `haveNominal=false` 仍不报 `DataTruncated`。
- 55 用例 **53 条原样、2 条只移断言位置**，零条换量测仪器、零条新增；§2.1 不增行不增用例。
- 术语：canonical 是「边解边播」（`GLOSSARY.md`），「流式」列 `_Avoid_`；本票标题与正文保留历史措辞、不回改。
- 判据分工不变：本文件管坏文件（5pp / `r × good`），`doc/format-support.md` 管好文件（1%），边解边播的内存 / 首播 / 欠流判据另落独立 suite，三套口径不混用。
