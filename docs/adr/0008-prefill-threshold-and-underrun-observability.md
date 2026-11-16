# 0008 首播门槛与欠流观测归消费侧，不引入水位驱动

- 状态：已接受
- 日期：2026-11-16
- 来源：[地图：播放链路缓冲与会话切换策略](https://github.com/wt159/SoundBridge/issues/173)、[首播门槛的定量口径与超时降级](https://github.com/wt159/SoundBridge/issues/177)、[欠流与设备写失败的观测口径](https://github.com/wt159/SoundBridge/issues/178)；事实基线 [实测取证](https://github.com/wt159/SoundBridge/issues/174) / [仪表核查](https://github.com/wt159/SoundBridge/issues/175)。规格落点 [`doc/buffering-switching-spec.md`](../../doc/buffering-switching-spec.md) §2 / §4。

## 背景

现状的起播顺序是「建任务 → 设备 `open` → 设备 `start` → 起消费定时器」（`sdk/MusicPlayer.cpp:315-337`），全程没有预填充。规格 `doc/streaming-playback-spec.md:42/47` 写着「首播阈值 = ring 可读量 ≥ 4096 B」，但代码里没有任何一处读 `availableRead()` 做门槛——设备在队列为空时就被 `SDL_PauseAudioDevice(..., 0)` 打开。

4096 B 在 44.1k/2ch/S16 下只有 23.2 ms，不足以覆盖起播抖动。另一方面，欠流完全不可见：消费端取不到数据时静默跳过本轮（`sdk/MusicPlayer.cpp:547-570`），`noteSilenceFill()` 的窗口判据只看得到 `SEEKING` / `m_stopRequest` / `m_abortDecode`（`sdk/audio/decode/AudioStreamDecoder.cpp:318-321`），`StreamMetrics.h:20-23` 却声称覆盖 shuttingDown / switching / SEEKING 三类窗口。

#174 的实测把量级钉死：11 种格式 210 个任务样本，攒够 150 ms PCM 的中位 0.44–4.44 ms、最慢 9.05 ms（m4a-AAC）；CI 同 commit 的可比口径是 ogg 998 µs / wma 1470 µs。没有素材在 300 ms 内攒不够。

## 决策

**冷启动时在设备 `start()` 前按时间口径攒 150 ms PCM（下限 4096 B、等待上限 300 ms），超时照常起播；欠流以「10 ms 拍中 ring 为空」独立计数，只观测、不设阈值；10 ms 定时 push 与 ring 无锁模型不变。**

- **按时间而不是按字节**：4096 B 随采样率与声道变化，23 ms 太浅；`prefillBytes = max(4096, rate × channels × bitsPerSample/8 ÷ 1000 × 150)`，按设备 spec 换算，生产链 26 460 B。
- **等待只能发生在控制线程、设备 `start` 之前**：设备一旦 start 就进入实时消耗，之后攒数据只会欠流；等待条件只能是 ring 累计写入量，不能是「设备首次 write」——后者由消费者产出，而消费者在 `_play()` 返回后才启动，会自锁（#175）。
- **顺序固定**：`startCurrentTask()` → `open()` → 等待门槛 → `start()` → `startConsumerThread()`。等待期设备已 open 未 start，SDL 不消耗队列。
- **超时是设计路径**：300 ms 到点照常 `start()`，记 `prefillTimeouts`，不报错、不清队列、不临时降门槛。网络源首帧受 `rw_timeout=3 s` 支配，300 ms 上限会例行打满，到点放行是唯一正确处置。
- **pause → resume 不等待**：复用同一任务，ring 里的 PCM 还在。
- **欠流口径**：`state()==DECODING` 且 `!shouldFillSilence()` 且 `availableRead()==0` 的拍数；`SEEKING` / switching / shuttingDown / 跨曲 / `EOS` / `ERROR` / `IDLE` / 门槛等待期全部豁免。只有 0 字节算欠流，不足 4096 B 仍有声音，不计。
- **落点**：`StreamMetrics` 新增 `underrunTicks`、`deviceWriteFailures`、`prefillTimeouts`；消费端调 `noteUnderrun()` / `noteDeviceWriteFailure()`，与 `m_silenceFills` 同款原子计数。`silenceFills` 的语义与稳态判据完全不动。
- **只观测**：按 `doc/observability-contract.md` §3.2 与 #104，不设任何数值阈值、不作为门禁。

## 考虑过的替代方案

**(a) 只把 4096 B 的门槛补上。** 改动最小、与规格字面一致。否决：44.1k/2ch/S16 下只有 23.2 ms，跨不过一次驱动调度抖动；且不同采样率/声道下时长不同，判据不可预期。

**(b) 改成水位 / 事件驱动消费（解码端唤醒消费者）。** 能省 5 ms 量级延迟。否决：规格 `:47` / `:99` 明令「不改 10 ms 定时 push、不给 ring 加 mutex」；改驱动模型会触发 `streaming-playback-spec.md:251` 与 `observability-contract.md:77` 的判据重开，收益与成本不匹配。

**(c) 低水位加速。** 看似符合「高低水位线」。否决：10 ms 每拍 4096 B ≈ 23 ms 音频，本来就是 2.3× 实时，消费者一直在超产，低水位没有可加速的空间；造一个假反馈回路只会增加状态。

**(d) 等「设备首次 write」再 start。** 否决：设备首写由消费者产出，消费者在 `_play()` 返回后才启动，必然自锁。

**(e) 把 `shouldFillSilence()` 加上「ring 空」条件，复用 `silenceFills` 判欠流。** 否决：`silenceFills` 的语义与 `== 0` 稳态判据会被改写，触发 `observability-contract.md` §3.4 第 3 条重审；且解码器看不到 `m_switching` / `shuttingDown`，窗口判据本来就落不了地。

## 后果

- 首播门槛的 150 ms / 300 ms / 4096 B 是**实现参数**，不进公开 API、不进 CI 门禁。
- 网络源到点放行是设计行为；不得据此把上限调大。
- `underrunTicks` 与 `deviceWriteFailures` 只可观测；若日后要设阈值，须按 #104 的单开票纪律另裁。
- `StreamMetrics.h:20-23` 的既有误述（声称覆盖三类窗口）只登记不修，避免在观测票里顺手改注释造成口径漂移。
- 实现属图外 P1 effort；P1 落地前不得启动切歌软取消，否则无法判断软取消是否把欠流推高。
