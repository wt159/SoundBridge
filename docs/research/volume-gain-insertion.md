# 软件音量增益的落点与失真边界（票 #236）

> 研究快照：**2026-11-21**，基线 `master` @ `881516d58`（`git log -1` 只读读取）。
> 方法：只读源码。源码行号是本文件写入时的快照，后续提交会让行号漂移；引用时以「文件 + 符号」为准。
> 检索纪律：普通检索限定 `sdk/`（`!sdk/3rdparty`）；SDL2 检索只落在 `sdk/3rdparty/SDL-release-2.28.1/src/audio/` 与 `include/` 两个子目录，未做仓库根无范围检索。
> 本文**不做**：不改生产代码、不定义音量语义（归 #238）、不做听感实测。文中「毫秒」除标注「实测」外均为按字节率推算。
> 范围：软件增益（PCM 逐样本缩放）插在哪一层；不含硬件混音器 / 系统音量。

## 结论

**推荐候选 2：在 `MusicPlayer::Impl::consumeAudioData()` 的 ring → write 段，`ring->read()` 之后、`m_audioDev->write()` 之前，对 4096 B 缓冲原地缩放（`sdk/MusicPlayer.cpp:704-711`）。**

一句话理由：这是唯一能在**不改 `IAudioDevice` / 不扩散 ADR 0011 设备替身**的前提下落地的位置，且它与「队列阈值、静音分支、seek / 切歌净空」同处一个函数，生效延迟和交互都能就地兑现。

支撑判断（按权重排序）：

1. **接口成本**：生产链里 `AudioDevice` 是 `IAudioDevice` 的唯一实现，测试侧唯一实现是 `AudioDeviceDouble`（装饰器）。候选 1 要设置增益，只能给 `IAudioDevice` 加第 12 个纯虚方法（两份实现 + ADR 0011 修订）；候选 2 只动 `MusicPlayer::Impl` 的私有状态，接口零变化。
2. **线程没有区别**：候选 1 的 `write()` 也是被 `consumeAudioData()` 同步调用的，与候选 2 落在**同一条 WorkQueue 消费拍线程**上。候选 1 并不比候选 2 「更远离判据主循环」；真正被稳定性闸门禁止的是候选 3（解码线程）。
3. **格式没有区别**：进设备队列的字节由重采样目标决定，也就是 `m_devSpec.format`（`AudioStreamDecoder.cpp:972-994`）。候选 2 手里就有 `m_devSpec`；候选 1 手里有 SDL `obtained` 与 `m_devSpec` 两份，反而多一个「该按哪份分支」的歧义（见 §2.2 与「未决与风险」R3）。
4. **默认路径零成本**：增益默认 1.0 时整段跳过，既有判据看到的字节与顺序完全不变，对稳定性闸门的扰动最小。

候选 1 是次优（唯一优点是「所有字节只有一个必经点」，但当前生产写入点恰好只有两处、且都在同一个函数里，见 §2.8）；候选 3 否决（解码线程 + ring 延迟 + 每任务重建状态）。

## 候选对比

| 落点 | 线程归属 | 格式代价 | 生效延迟（滑杆 → 听到） | 对 ADR 0011 的影响 | 主要风险 |
|---|---|---|---|---|---|
| **① `AudioDevice::write()` 内、`SDL_QueueAudio` 前**（`AudioDevice.cpp:170-180`） | 写：UI 线程 → 需新增设备侧 setter；读：消费拍线程（`write()` 由 `consumeAudioData()` 同步调用） | 需按 `AudioSpec::format` 分支；生产实际只有 S16LE 一条路，见 §2.2 | 队列内旧字节不受影响，稳态 ≈1.51 s（44.1k/2ch/S16），dummy 实测速率下 ≈1.78 s；另加一个设备周期 20–24 ms | **大**：第 12 个纯虚方法，`AudioDevice` + `AudioDeviceDouble` 双实现，ADR 0011 的「11 方法」正文要改 | 设备层被塞进应用级音量状态；`obtained` 与 `m_devSpec` 格式歧义；纯 fake 替身会丢增益 |
| **② `MusicPlayer::Impl::consumeAudioData()` ring→write 段**（`MusicPlayer.cpp:704-711`） | 写：UI 线程 `atomic` store（与 `setAutoSkipOnError` 同型，`MusicPlayer.cpp:305-307`）；读：消费拍线程，每拍 load 一次 | 同上，但缓冲格式就是 `m_devSpec.format`，无歧义 | 与 ① 相同 ≈1.51 s / dummy ≈1.78 s（同一队列） | **无**：不碰 `IAudioDevice`，替身无需实现增益 | 落在消费拍（判据计数同线程）；增益阶跃粒度为 4096 B ≈23 ms，快速拖动可能有 zipper；只覆盖现有两个写入点 |
| **③ 解码输出后、入 ring 前（重采样之后）**（`AudioStreamDecoder.cpp:943-1013`） | 写：UI 线程需找到「当前任务的 decoder」；读：**解码线程** | 同上；但解码线程已承担解码/重采样，热路径更敏感 | 队列 256 KiB **加上** ring 256 KiB 的旧增益数据：最坏 ≈3.0 s（dummy ≈3.5 s） | 无接口变化，但要在解码器实例上挂状态 | **稳定性闸门直接禁止**（解码线程）；每次切歌新建 decoder（`MusicPlayList.cpp:539-542`）导致增益重置；ring 内新旧增益混杂产生拼接痕 |

### 2.1 线程归属（逐候选）

**共同事实**

- 「10 ms 消费拍」不是独立线程：`startConsumerThread()` 在 `m_workQueue` 上注册重复定时任务（`MusicPlayer.cpp:821`），所有控制命令、消费拍、回调都在**同一条** WorkQueue 线程上串行执行（`docs/research/public-api-contract-facts.md` 的 T3–T5 已核）。
- 解码线程是 `AudioStreamDecoder::start()` 每次新建的独立线程（`AudioStreamDecoder.cpp:214-217`），只写 ring，不写设备。
- 因此：**软件增益无论放在 ①②，读侧都落在消费拍线程；只有 ③ 落在解码线程。**

**候选 ② 的写侧**

- 仓库已有同型先例：`setAutoSkipOnError()` 不经过 `m_workQueue`，直接在调用者线程 `m_autoSkipOnError.store(enabled)`（`MusicPlayer.cpp:305-307`），消费拍侧 `m_autoSkipOnError.load()`（`:985`）。音量 setter 照抄这一形状即可：任意线程可调用、不排队、不产生控制事件、不触发回调。
- 表示建议：`std::atomic<int>` 存 Q15（0…32768），避免每次消费拍读浮点原子、也避免 32 位平台上的浮点原子降级。内存序用 `relaxed` 足够：增益值不与其他数据构成发布/订阅关系；一次 4096 B 缓冲只取一次快照，块内不会撕裂。
- 播放期可变：可以。store 在下一拍（标称 10 ms，Windows 实测代理 ≈15 ms，见 `device-double-evidence.md` §4.3）进入链路；**可听**延迟由已排队字节决定（§2.5）。

**候选 ① 的写侧**

- `MusicPlayer::Impl` 只持有 `std::shared_ptr<IAudioDevice>`（`MusicPlayer.cpp:94`）。要设置增益，只能：给接口加方法（扩散）、或 `dynamic_cast<AudioDevice*>`（在测试装饰器下会失败，增益在测试里静默失效）、或读生产全局变量（违反 ADR 0010 的钩子纪律）。三者都不干净，故「候选 1 不加接口」实际不成立。

**候选 ③ 的写侧**

- decoder 是 per-task 的：切歌时 `replacePlaybackTask()` 新建 ring + decoder（`MusicPlayList.cpp:539-542`），音量必须随任务重建重新灌注；`_setPosition` / `_next` / `_previous` / `stop` 各有一条路径要记得同步。状态面明显比 ② 大。

### 2.2 格式分支：`AudioSpec::format` 的实际取值集合

**枚举全集**（`sdk/audio/common/AudioCommon.hpp:5-23`，共 16 个）：`S8 / U8 / S16 / S16BE / U16 / U16BE / S24 / S24BE / U24 / U24BE / S32 / S32BE / FLT32 / FLT32BE / DBL64 / DBL64BE`，外加 `Unknown`。

**赋值点分三组：**

1. **设备侧**：`SDLAudioSpec2AudioSpec()` 只映射 SDL 支持的 10 种——`AUDIO_U8/S8/U16/U16MSB/S16/S16MSB/S32/S32MSB/F32/F32MSB` → `U8/S8/U16/U16BE/S16/S16BE/S32/S32BE/FLT32/FLT32BE`（`AudioDevice.cpp:245-279`）。**SDL2 没有 24-bit 格式**，所以设备侧永远不会产出 `S24/U24/DBL64`。
2. **提取器侧**：除 mp3 / ogg 不声明 PCM 格式外，其余提取器统一用 `getAudioFormatByBitPreSample()`（8→`U8`、16→`S16`、24→`S24`、32→`S32`、64→`DBL64`，`AudioCommon.hpp:25-41`），例如 `WAVExtractor.cpp:191`、`AACExtractor.cpp:157`。**无符号 8-bit 确实存在**：WAV 的 8-bit PCM 就是无符号，会得到 `AudioFormatU8`。
3. **解码器侧**：ffmpeg 输出映射为 `U8 / S16 / S32 / FLT32 / DBL64`（`AudioDecode.cpp:218-238`），从不产出 24-bit、从不产出大端。

**真正会进设备队列的格式**：`AudioStreamDecoder::writeDecodedBytes()` 在 `m_srcSpec != m_devSpec` 时把 PCM 重采样到 `m_devSpec`，再把重采样输出写 ring（`AudioStreamDecoder.cpp:972-994`、`:996-1013`）；相等时原样写。`m_devSpec` 在 `MusicPlayer::Impl` 构造时由 `AudioDevice::getDeviceSpec()` 取一次（`MusicPlayer.cpp:164-165`），而它来自构造函数里的默认值 `44100 / AUDIO_S16SYS / 2ch / 882`（`AudioDevice.cpp:21-24`、`:73-80`）——`open()` 只更新 `m_sdlSpec = obtained`（`:120`），**不回写 `m_devSpec`**。所以生产链进队列的字节恒为 **S16LE 立体声 44100**（Windows/x86 上 `AUDIO_S16SYS` = `AUDIO_S16LSB`）。

**各格式缩放代价（按 4096 B/拍估算）：**

| 格式 | 会到设备吗 | 缩放写法 | 每样本代价 | 备注 |
|---|---|---|---|---|
| `S16`（LE） | **是，唯一实际路径** | `(int32)s * g15 + 0x4000 >> 15`，再钳 `[-32768, 32767]` | ≈3–5 cycle，最便宜的有符号整型路径 | 2048 样本/拍 ≈ 0.8 万 cycle/拍 |
| `S16BE` | 仅大端平台 | 同上 + 字节序处理 | ≈2× | x86 不出现 |
| `S32` | 当前不会 | 必须 `int64` 中间量（`s*g` 会溢出 32 位） | ≈2× S16 | |
| `FLT32` | 当前不会（除非将来改设备 spec） | 一次乘法，可选钳 `[-1,1]` | ≈2 cycle，最便宜 | 无舍入问题 |
| `U8` | 不会（设备 spec 是 S16） | `(x-128)*g + 128`，钳 `[0,255]` | ≈5 cycle | 枚举与 8-bit WAV 会产出，但被重采样吃掉 |
| `U16 / S8` | 不会 | 中点偏移 / 小整型 | 低 | 重采样把 `U16` 当 `S16` 处理，是既有的潜在错 |
| `S24 / U24 / *BE` | **不可达** | 3 字节非对齐读写 | 最高 | `AudioResample` 的 switch 没有 24-bit 分支（`AudioResample.cpp:222-252`），SDL 也没有 24-bit 格式 |
| `DBL64` | 不会 | 双精度乘法 | 中 | 重采样支持，但设备 spec 不可能是它 |

结论：**只需实现 `S16` 一条，`FLT32` 作可选的廉价兜底，其余格式原样放行并打一次 warning。不要为 24-bit 写代码**——它既到不了设备，也会把「格式分支」的面积撑大。

S16 的舍入细节：`(s * g + 0x4000) >> 15` 是「加半再算术右移」。它在数学上是**向 +∞ 的四舍五入**：正半轴「半数进位」、负半轴「半数向零」，因此有最多 0.5 LSB（约 −90 dBFS）的正向偏置，听感上可忽略，但单元测试不要按「对称舍入」写期望值。若要求对称，可对负样本改用 `+0x3FFF`（每样本一个符号判断），本票不认为值得。`g ≤ 32768` 时 `s * g` 的 int32 中间量最大约 1.07e9，不会溢出，结果也落在 `[-32768, 32767]`；钳制仍建议保留（见 §2.3）。

### 2.3 削波策略与是否支持 >1.0

- **增益 ∈ [0, 1]**：按 §2.2 的定点公式，int32 中间量不溢出、结果也不越界，**理论上可以不钳**。仍建议钳制：实现一旦改成 >1.0、换成浮点/其他舍入式或加块内渐变，钳制就是最后一道保险；两条比较指令的成本可忽略。
- **增益 > 1.0**：会溢出，必须选硬钳制或软限幅。
  - 硬钳制：每样本 `if`，实现简单、无状态、可测试；代价是削波谐波失真，且失真量随增益非线性增长。
  - 软限幅：需要包络/前后瞻状态（attack/release），从「无记忆的逐样本函数」变成「有状态的流处理」，测试面和状态面都扩大，本票不建议。
- **建议**：v1 的公开契约限制在 `[0, 1]`；把「>1.0 的 Boost」留给单独票，并明确届时采用硬钳制。这样本票的实现就是一个纯函数，可用单元测试锁死舍入与钳制。
- **抖动（dither）**：不做。16-bit 乘任意系数会损失低位分辨率，但加抖动会把「确定性纯函数」变成「带随机源」，与可测性冲突；音量滑杆场景不值得。

### 2.4 性能量级与是否需要 SIMD

- 数据量：每拍最多 4096 B = 1024 个立体声帧 = 2048 个样本；标称 100 拍/s → **204,800 样本/s**（按 15 ms 拍长则约 13 万样本/s）。
- S16 缩放按 4 cycle/样本估算 → **≈0.8 M cycle/s**，约 2.5 GHz 单核的 **0.03%**；即使按 10 cycle/样本也只有 0.08%。
- 内存流量：原地缩放 = 每拍读 4 KiB + 写 4 KiB = 8 KiB/拍 ≈ **0.8 MB/s**；且可直接复用 `consumeAudioData()` 每拍已经分配的 `std::vector<char> buf`（`MusicPlayer.cpp:705`），**不需要新分配**。
- 对照既有每拍工作：`ring->read()` 的 4 KiB 拷贝、`SDL_QueueAudio` 内部再拷贝进 SDL 数据队列（`SDL_audio.c:597-601`），必要时还有设备侧格式转换流。逐样本缩放只占这条链路的很小一部分。
- **SIMD 不值得**：收益上限是 0.03% → 0.01%，却引入平台分支、对齐要求和不可移植的构建约束。写成朴素循环即可，编译器会自动向量化；不要手写 SIMD。
- 真正需要控制的是**稳定性闸门**，不是 CPU：默认增益 1.0 时整段早退，既有判据的字节与耗时完全不变；只有显式设过音量的用例才付这份成本。

### 2.5 生效延迟

延迟由两段构成：**已排队但未播的旧增益 PCM** + **一个设备周期 / OS 缓冲**。软件增益只影响「之后新入队」的字节，改不动队列里已有的字节（SDL 队列没有「重写队首」的 API）。

| 量 | 数值 | 依据 |
|---|---|---|
| 应用侧队列阈值 | `256 * 1024` B，判定是 `>` | `MusicPlayer.cpp:699-700` |
| 单拍最大追加 | 4096 B | `MusicPlayer.cpp:704-711` |
| 队列可能达到的上界 | 262,144 + 4,096 = **266,240 B** | 阈值判断在写入之前 |
| S16 / 44.1k / 2ch 字节率 | **176,400 B/s** | 44100 × 2 × 2 |
| 稳态可听延迟（标称硬件） | 266,240 ÷ 176,400 ≈ **1.51 s** | 推算；稳态队列贴着阈值 |
| dummy 实测消耗速率 | 149.5–149.8 KB/s（≈146 KiB/s） | `device-double-evidence.md` §1 / §5.1，3528 B/拍、23.56 ms/拍 |
| 稳态可听延迟（dummy） | 266,240 ÷ 149,800 ≈ **1.78 s** | 推算 |
| 设备周期附加 | 882/44100 = 20 ms；dummy 实测 23.6 ms；部分后端会把 882 向上取 2 的幂（1024 → 23.2 ms） | `AudioDevice.cpp:23`、`SDL_audio.c:1403-1405` |
| ring 附加（仅候选 ③） | 262,144 ÷ 176,400 ≈ 1.49 s → 总计 ≈**3.0 s** | `AudioRingBuffer.h:18` |

要点：

- **稳态下队列不是「半满」，而是贴着阈值**：消费拍每 10 ms 追加 4096 B = 409.6 KB/s，远高于 176.4 KB/s 的消耗速率（dummy 实测也只有 149.8 KB/s），所以队列在起播后约 1.1 s 就涨到 256 KiB 并停在那里。也就是说，**稳态播放时「拖滑杆到听到」的典型值就是上界 ≈1.5 s（dummy ≈1.8 s）**，不是平均值。
- 延迟上限会在 `clearQueue()` 之后被重置：seek / 切歌 / stop 把队列清空，接下来约 1.1 s 内延迟从 0 线性涨回 1.5 s。**刚 seek 完的一小段时间内调音量几乎立即生效**，之后又回到 1.5 s。
- 若 #238 想承诺「<200 ms」，必须同时改队列阈值（例如 256 KiB → 32 KiB，代价是欠流余量从 ≈1.5 s 降到 ≈0.18 s），或改在音量变化时清队列（会丢掉已缓冲 PCM，可能造成可听断点）。**本票不建议在 v1 承诺 <200 ms。**

### 2.6 与 seek / 切歌 / LIVE 重连 / stop 的交互

| 事件 | 设备是否重开 | 队列是否净空 | 候选 ② 的增益是否保持 | 说明 |
|---|---|---|---|---|
| seek | 否 | **是**（`MusicPlayer.cpp:493-495`） | 是 | 净空后新 PCM 立刻用当前增益，无 1.5 s 残留 |
| next / previous / setIndex | 否（设备保持 open，只换 decoder） | **是**（`MusicPlayer.cpp:570-572`） | 是 | 新任务新建 decoder，但增益在 `MusicPlayer::Impl`，不随之重建 |
| 曲目自然 EOS 推进 | 否 | 是（同上路径） | 是 | |
| pause / resume | 否 | 否 | 是 | 暂停期间队列保留，恢复后先播旧增益的 ≤1.5 s 缓冲 |
| stop | **是**（`_releaseAudio()` → close，`MusicPlayer.cpp:450-462`） | 是 | 是（`Impl` 对象不销毁） | 再 `play()` 会重新 `open()`，增益仍在 |
| LIVE 重连（`reopenConnection`） | 否 | 否 | 是 | 重连只动提取器/解码器（`AudioStreamDecoder.cpp:1221`），设备与 `MusicPlayer::Impl` 都不参与 |
| 解码错误 | 是（`MusicPlayer.cpp:793-796`） | 是 | 是 | 停在 Paused 或自动跳轨，增益不受影响 |

补充：`selectDevice()` 在生产侧仍是 TODO（`AudioDevice.cpp:94-98`），「运行期换设备」暂不存在；一旦实现，候选 ② 的增益天然跨设备保持。

### 2.7 `AudioSpec` 是否够用

**够用，不需要新增字段。**

- 缩放只需要三件事：样本格式（`format`）、每样本字节数（`bytesPerSample`，或由 format 推出）、声道数（`numChannel`，仅用于校验块长是否为整帧）。`AudioSpec` 三者都有（`AudioCommon.hpp:72-97`）。
- 延迟承诺需要「字节 → 毫秒」换算：用 `sampleRate × numChannel × bytesPerSample` 即可。仓库已有同型算式 `prefillTargetBytes()`（`MusicPlayer.cpp:575-602`），直接复用，不必往 `AudioSpec` 里塞 `durationMs` 之类的派生字段（该字段在设备 spec 上本来就是 0）。
- 一个真实的缺口不是字段，而是**语义**：`m_devSpec` 是「请求的」spec，不是 SDL `obtained`。见 R3。

### 2.8 若在 `IAudioDevice` 加方法：ADR 0011 的扩散与替代路径

**扩散面（机械上很小）：**

- `IAudioDevice.h`：+1 纯虚（现为 11 个，`:23-35`）。
- `AudioDevice.h`：+1 声明；`AudioDevice.cpp`：+1 转发 + `Impl` 内一个原子成员。
- `AudioDeviceDouble.hpp`：+1 override（现为全直通，`:41-94`）。
- 仓库内 `public IAudioDevice` 的实现者只有这两个（`AudioDevice.h:22`、`AudioDeviceDouble.hpp:19`），所以是 **4 个文件、约 10 行**。

**但概念成本不小：**

- ADR 0011 正文把接口冻结为「覆盖 `AudioDevice` 的全部 11 个公开实例方法」（`docs/adr/0011-device-double-and-injection-surface.md:21`），并在后果里写明「`AudioDevice` 的 11 个方法变虚」。加第 12 个方法必须修订 ADR，并回答「替身是否承担音量保真责任」——否则未来有人写纯 fake 替身时，增益会静默消失。
- 设备层从此有应用级状态，`AudioDevice` 不再是「薄 SDL 包装」。

**不加接口的替代路径：**

1. **候选 ②（推荐）**：增益完全留在 `MusicPlayer::Impl`，`IAudioDevice` 一字不动，替身不需要知道音量存在。
2. 「只改 `AudioDevice::Impl` + 具体类 setter」不可行：`MusicPlayer` 只持 `shared_ptr<IAudioDevice>`。
   - `dynamic_cast<AudioDevice*>`：在测试装饰器下会失败（`AudioDeviceDouble` 不是 `AudioDevice` 的子类），增益在测试里静默失效。
   - 额外再存一个 `shared_ptr<AudioDevice>` 具体指针：生产可行，但 `setAudioDeviceForTest()`（`MusicPlayer.h:88`）替换接口指针后两个指针会不同步，测试注入设备时增益要么丢失、要么写进被换下的旧设备。ADR 0011 明确「生产路径直接持本类，不加转发层」，这种双指针正好破坏了该前提。
3. 「进程级全局增益」不可行：生产路径读全局变量就是 ADR 0010 意义上的钩子，且会让两个 `MusicPlayer` 实例互相污染。

### 2.9 额外候选：`SDL_MixAudioFormat`

**结论：能用于 S16，但不比手写缩放更优，不建议采用。**

一手事实（SDL 2.28.1，`sdk/3rdparty/SDL-release-2.28.1/`）：

- 语义是**加法混合**：`dst = clamp(dst + src * volume / 128)`（`src/audio/SDL_mixer.c:84-86`、`:135-156`）。它要求 `dst` 与 `src` 同格式、同长度，且 `dst` 是「已有内容」；不能原地对 `src` 用（`dst == src` 会变成 `src*(1+vol)`）。
- `volume` 范围 **0–128**，128 即 1.0（`include/SDL_audio.h:1121`）——**无法表达 >1.0**。
- `volume == 0` 时函数**直接 return，完全不写 `dst`**（`SDL_mixer.c:91-93`）。这意味着静音必须靠「先把 `dst` 清零」，否则会把上一拍的残留样本当成静音输出——一个很容易踩的坑。
- 整数路径用 `/` 截断而非四舍五入（`SDL_mixer.c:84-86`），负半轴的舍入不对称，低音量下有轻微零偏；`F32` 路径的钳制是 `±FLT_MAX` 而非 `±1.0`（`:291-293`），并不是真正的满刻度钳制。
- **不支持 24-bit**（SDL 根本没有这个格式），与我们「24-bit 不可达」的结论不冲突，但也意味着它不能作为通用兜底。
- 代价：需要一块清零的临时缓冲（每拍 4096 B memset）+ 两遍内存访问；手写原地缩放是一遍读写、无额外缓冲。

**判断**：`SDL_MixAudioFormat` 是「混两路流」的原语，不是「增益」原语。用它做增益要额外处理「零缓冲、静音陷阱、无 Boost、无 24-bit」，反而比 20 行手写 S16 缩放更复杂。仅当团队完全不想维护格式分支时，可作为 S16-only 的临时实现；即便如此也要显式 `memset(dst, 0, len)` 并禁止 `dst == src`。

## 证据

> 下列行号对应 `881516d58`（2026-11-21）。每条一行原文。

**设备层**

- `sdk/audio/device/AudioDevice.cpp:24` — `static constexpr int m_defaultFormat   = AUDIO_S16SYS;`
- `sdk/audio/device/AudioDevice.cpp:23` — `static constexpr int m_defaultSamples  = 882;`
- `sdk/audio/device/AudioDevice.cpp:80` — `SDLAudioSpec2AudioSpec(&m_sdlSpec, m_devSpec);`
- `sdk/audio/device/AudioDevice.cpp:114` — `m_deviceID = SDL_OpenAudioDevice(nullptr, 0, &m_sdlSpec, &obtained, 0);`（最后参数 0 = `allowed_changes` 全不允许）
- `sdk/audio/device/AudioDevice.cpp:120` — `m_sdlSpec = obtained;`（此后**没有**再调用 `SDLAudioSpec2AudioSpec` 回写 `m_devSpec`）
- `sdk/audio/device/AudioDevice.cpp:175` — `int ret = SDL_QueueAudio(m_deviceID, data, len);`
- `sdk/audio/device/AudioDevice.cpp:187` — `return SDL_GetQueuedAudioSize(m_deviceID);`
- `sdk/audio/device/AudioDevice.cpp:245-279` — SDL→`AudioFormat` 映射只有 10 种，无 24-bit、无 `DBL64`
- `sdk/audio/device/IAudioDevice.h:23-35` — 11 个纯虚方法（`getDeviceList` … `clearQueue`）

**播放器层**

- `sdk/MusicPlayer.cpp:164-165` — `m_audioDev = std::make_shared<AudioDevice>();` / `m_audioDev->getDeviceSpec(m_devSpec);`
- `sdk/MusicPlayer.cpp:305-307` — `m_autoSkipOnError.store(enabled);`（不排队、直接原子写的既有先例）
- `sdk/MusicPlayer.cpp:664-666` — 静音分支：`std::vector<char> silence(len, 0);` 后 `m_audioDev->write(...)`
- `sdk/MusicPlayer.cpp:699-700` — `const size_t kQueueThreshold = 256 * 1024;` / `if (m_audioDev->getQueuedBytes() > kQueueThreshold) {`
- `sdk/MusicPlayer.cpp:704-711` — `kChunkSize = 4096` → `ring->read(...)` → `m_audioDev->write(buf.data(), got)`（候选 ② 的落点）
- `sdk/MusicPlayer.cpp:494` — `m_audioDev->clearQueue();`（seek）
- `sdk/MusicPlayer.cpp:571` — `m_audioDev->clearQueue();`（切歌成功）
- `sdk/MusicPlayer.cpp:458` — `m_audioDev->clearQueue();`（stop / `_releaseAudio`）
- `sdk/MusicPlayer.cpp:821` — `m_workQueue.startTimerTask(true, 10, [this]() { this->consumeAudioData(); });`（消费拍 = WorkQueue 线程）
- `sdk/MusicPlayer.cpp:575-602` — `prefillTargetBytes()` 的「字节 ↔ 毫秒」既有算式

**解码 / 重采样 / ring**

- `sdk/audio/decode/AudioStreamDecoder.cpp:972-994` — 有 resampler 时输出到 `m_resampleBuf`，目标规格是 `m_devSpec`
- `sdk/audio/decode/AudioStreamDecoder.cpp:1007` — `size_t got  = m_ring->write(outData + written, once);`
- `sdk/audio/decode/AudioStreamDecoder.cpp:1221` — `if (!m_extractor->reopenConnection()) {`（LIVE 重连只动提取器，不动设备）
- `sdk/audio/resample/AudioResample.cpp:222-252` — 输入格式 switch 无 `S24/U24` 分支，默认 `AV_SAMPLE_FMT_NONE`
- `sdk/utils/AudioRingBuffer.h:18` — `static constexpr size_t kDefaultCapacity = 262144; // 256 KB`
- `sdk/MusicPlayList.cpp:539-542` — 每次切歌 `make_shared<AudioRingBuffer>` + `make_shared<AudioStreamDecoder>`

**格式枚举 / 赋值**

- `sdk/audio/common/AudioCommon.hpp:5-23` — `enum AudioFormat` 16 个值
- `sdk/audio/common/AudioCommon.hpp:28-30` — `case 8: return AudioFormatU8;`（无符号 8-bit 存在）
- `sdk/audio/common/AudioCommon.hpp:72-79` — `AudioSpec` 的 `sampleRate / format / numChannel / bitsPerSample / bytesPerSample`
- `sdk/extractor/wav/WAVExtractor.cpp:191` — `m_audioSpec.format = getAudioFormatByBitPreSample(m_audioSpec.bitsPerSample);`
- `sdk/audio/decode/AudioDecode.cpp:221-223` — `case AV_SAMPLE_FMT_U8: ... return AudioFormatU8;`

**SDL 2.28.1（仅 `src/audio/` 与 `include/`）**

- `sdk/3rdparty/SDL-release-2.28.1/src/audio/SDL_audio.c:584-604` — `SDL_QueueAudio` 只是把数据拷进数据队列，格式由设备侧解释
- `sdk/3rdparty/SDL-release-2.28.1/src/audio/SDL_audio.c:1452-1461` — `build_stream` 时按 `obtained → device->spec` 建转换流（队列侧按 `obtained` 解释）
- `sdk/3rdparty/SDL-release-2.28.1/src/audio/SDL_audio.c:1470-1481` — 无回调时使用 `device->buffer_queue`，回调为 `SDL_BufferQueueDrainCallback`
- `sdk/3rdparty/SDL-release-2.28.1/src/audio/SDL_mixer.c:88-93` — `if (volume == 0) { return; }`
- `sdk/3rdparty/SDL-release-2.28.1/src/audio/SDL_mixer.c:135-156` — S16LE 路径：`dst_sample = src1 + src2;` 后钳到 `SDL_MAX_SINT16 / SDL_MIN_SINT16`
- `sdk/3rdparty/SDL-release-2.28.1/include/SDL_audio.h:1121` — `#define SDL_MIX_MAXVOLUME 128`
- `sdk/3rdparty/SDL-release-2.28.1/include/SDL_audio.h:1150-1160` — 「mixes it into `dst`, performing addition, volume adjustment, and overflow clipping」

**既有一手研究（仓库内）**

- `docs/research/device-double-evidence.md:197-202` — dummy 拍长实测 p50 23–24 ms、max 26–27 ms
- `docs/research/device-double-evidence.md:226` — 有效速率 149.5–149.8 KB/s、实测量子 3528 B
- `docs/adr/0011-device-double-and-injection-surface.md:21` — 「`IAudioDevice` 覆盖 `AudioDevice` 的 11 个公开实例方法」
- `docs/adr/0011-device-double-and-injection-surface.md:27` — 「失败脚本只存在于测试代码，生产路径没有任何钩子读」

## 未决与风险

**R1（证据边界）本文没有任何听感 / 真实声卡实测。** 所有毫秒数都是「队列字节 ÷ 字节率」的推算，只用了仓库内 dummy 驱动的既有实测速率做交叉校验。真实硬件的缓冲、驱动混音、蓝牙延迟都未测，真实延迟可能比推算更大。实现票若要把延迟写进判据，必须先在目标平台取一次真实测量。

**R2（稳定性闸门未验证）** 消费拍上增加逐样本循环，即使默认 1.0 早退，也改变了代码形状。本票没有跑 `ctest -j4` 10 轮对照。实现票应给出「默认增益下 10/10 全绿」的对照证据；若出现假红，优先怀疑新增分支改变了 `consumeAudioData()` 的指令布局，而不是判据本身。

**R3（`m_devSpec` 与 `obtained` 的既有错配）** `open()` 只把 `obtained` 存进 `m_sdlSpec`，`m_devSpec` 仍是构造时的默认值（`AudioDevice.cpp:120`）。如果某个后端 obtain 到非 S16 格式，重采样仍按 S16 产出，而 SDL 会按 `obtained` 解释队列字节——这条链路在加增益**之前**就已经错了。候选 ② 按 `m_devSpec.format` 缩放是「与缓冲区内容一致」的正确选择；但不要顺手把它「修」成按 `m_sdlSpec.format` 缩放，那会让缓冲区和缩放器各说各话。该错配应另开票处理。

**R4（增益阶跃粒度）** 每拍最多 4096 B = 1024 帧 ≈ **23.2 ms 音频**。一次 store 只影响下一块，块内是常数增益；快速拖动（例如 0→1 用 100 ms）会产生 4 个 25% 的阶跃，可能有 zipper 噪声。未做听感验证。若 #238 要求平滑，需要在实现里加「块内从上一增益线性渐变到新增益」的状态（成本仍然很低，但就不再是纯无状态函数）。

**R5（静音 / 拖动的手感是产品级问题）** 稳态队列贴着阈值，所以增益变化的可听延迟不是「最坏 1.5 s」而是「通常就是 1.5 s」。滑杆拖动会明显滞后，点静音后还会响约 1.5 s。可选的工程手段只有三种：调低 `kQueueThreshold`（牺牲欠流余量）、变更时清队列（丢缓冲、可能有断点）、或接受延迟。三者都是产品取舍，归 #238，不在本票裁定。

**R6（候选 ① 的「单点保证」无法完全复制）** 候选 ② 覆盖了当前仅有的两个写入点（正常 + 静音，`MusicPlayer.cpp:666`、`:709`），但未来若新增第三个设备写入点，它必须记得走增益。可以在实现时把「缩放 + 写设备」收成一个私有小函数，把风险压到「新增写入点必须调用它」。

**R7（格式分支的运行时验证）** 「生产只会出现 S16」是**代码路径推断**：默认 spec、重采样目标、SDL 映射三者交叉得出。没有在真实设备上枚举 `obtained.format` 的实测。若线上出现非 S16，当前实现应「原样放行 + 一次 warning」，不要猜格式。

**R8（SIMD 结论是估算）** 0.03% 的单核占用是按 4 cycle/样本估的，没有做微基准。但即使估错 10 倍也仍是 0.3%，不改变「不值得 SIMD」的结论。

**R9（公开 API 与 ADR 0012）** 本票只裁定落点。`Player` / `MusicPlayer` 是否暴露 `setVolume`、取值是 float 还是 0–100 整数、是否持久化，都属于 #238 与公开 API 契约；`docs/adr/0012-app-ui-contract.md` 可能需要同步一行。

**R10（测试替身的行为变化）** 若最终误选了「在 `AudioDeviceDouble::write()` 里缩放」之类的路径，ADR 0011 的「默认全直通」前提会被破坏，三条设备层判据的字节基线也会变。推荐路径（候选 ②）不触碰替身，这一点应在实现票里写成硬约束。

## 对下游票的输入

给 **#238（音量语义契约）** 的具体建议：

1. **取值范围**：公开语义用线性幅度 `[0.0, 1.0]`，默认 `1.0`；UI 可用 0–100 映射。理由：S16 满刻度之上没有余量，`>1.0` 必然进入削波策略，会把「音量」和「限幅器」两个契约绑死。
2. **内部表示**：`atomic<int>` Q15（0…32768），setter 直接 `store(relaxed)`，消费拍每拍 `load` 一次。不要用 `atomic<float>`：没必要，且会让「S16 缩放」和「浮点音量」之间多一次转换。
3. **静音**：`0.0` 定义为数字静音（输出全零），而不是「极小增益」。由于稳态队列贴着 256 KiB 阈值，「点静音后还会响约 1.5 s」——这大概率不符合产品预期。建议在下列方案中选定一种（或按场景拆分）：
   - 不清队列：承诺「≤1.5 s 内静音」，实现最简单，与现有缓冲语义一致；
   - 进入 `0.0` 时清队列：承诺「立即静音」，但要写明「丢弃已缓冲 PCM，可能产生 ≤1 拍的可听断点」；
   - 折中：只在「跨越 0」时清队列（进入静音 / 解除静音），连续拖动不清——这样滑杆手感仍是 1.5 s 延迟，但静音按键是即时的。
4. **生效延迟的写法（必须可兑现）**：
   > 「新的增益在下一个消费拍（标称 10 ms）进入输出链路；已排队 PCM 不受影响。可听延迟 = `设备队列字节数 ÷ 采样率 ÷ 声道数 ÷ 每样本字节数`。由于消费拍喂料速率（409.6 KB/s）高于播放消耗速率，稳态队列会填满 256 KiB 阈值，因此 44.1 kHz / 2 声道 / S16 下的**稳态可听延迟约为 1.5 s**。seek、切歌、stop 会清空队列，此后的约 1.1 s 内延迟从 0 线性涨回 1.5 s。」

   **不要写**「实时」「<100 ms」「无延迟」——按当前队列策略这些承诺兑现不了。若产品坚持低延迟，必须同时把「调低 `kQueueThreshold`」列为该票的范围（并重新评估欠流判据），而不是在音量票里偷偷改。
5. **阶跃粒度**：契约可以写「增益按 4096 B 块（≈23 ms 音频）生效」；若产品要求拖动无 zipper，在实现票里加块内线性渐变，并把「渐变时长 ≤ 1 块」写进契约。
6. **持久化**：v1 建议只做进程内状态（跨暂停/切歌/停止/LIVE 重连保持，重启回 1.0）。落盘是另一个契约，别混进来。
7. **线程与回调**：`setVolume` 可从任意线程调用，不排队、不产生状态回调、不改变 `MusicPlayerState`；与 `setAutoSkipOnError` 同型（`MusicPlayer.cpp:305-307`）。
8. **可测性（写给实现票）**：
   - 把缩放抽成自由函数：`scalePcmInPlace(void* data, size_t len, AudioFormat fmt, int gainQ15)`，单元测试锁死 S16 的舍入、正负满刻度、`gain=0`、`gain=32768`；
   - 集成层若要观察字节，用**测试代码里的**捕获装饰器包住 `AudioDeviceDouble`，不要往生产 `IAudioDevice` 加读数方法（ADR 0011 的 11 方法边界保持不动）；
   - 判据只需两条：`gain=1.0` 时写入字节与改动前逐字节相同；`gain=0.5` 时输出样本满足 `|out - in/2| ≤ 1`。不要在 CI 里断言听感或墙钟延迟。
9. **与 ADR 0011 的边界**：在 #238 里写一句「音量是播放器职责，不是设备职责；`IAudioDevice` 不新增音量方法」。若将来要做设备级硬件音量，另开票并重走 ADR。
