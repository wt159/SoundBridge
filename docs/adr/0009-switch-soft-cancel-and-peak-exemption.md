# 0009 切歌软取消：任务退役与切换窗口峰值豁免

- 状态：已接受
- 日期：2026-11-16
- 来源：[地图：播放链路缓冲与会话切换策略](https://github.com/wt159/SoundBridge/issues/173)、[切歌软取消：任务退役所有权与切换窗口峰值豁免](https://github.com/wt159/SoundBridge/issues/179)、[设备队列净空的统一出口](https://github.com/wt159/SoundBridge/issues/176)。规格落点 [`doc/buffering-switching-spec.md`](../../doc/buffering-switching-spec.md) §3 / §5。

## 背景

现状切歌是硬切：`replacePlaybackTask()` 先 `stopCurrentTask()`（`decoder->stop()` → `join()` → 丢 ring），再建新 ring / decoder（`sdk/MusicPlayList.cpp:456-490`）。`join()` 与 10 ms 消费定时任务同处一条 WorkQueue 线程，所以切歌时消费节拍被直接顶住；旧解码线程若卡在网络读，最坏等到 `rw_timeout=3 s`。

规格 `doc/streaming-playback-spec.md` §4.2 已经给自动跳轨留了一个例外：「旧解码线程 join 允许延后到下一次切歌或析构」，但普通切歌没有用这条规则。§4.3 的 E4 又明令「prev/next 预缓冲只留曲目元数据，不留 ring / decoder 实例」。

## 决策

**切歌时旧任务只 abort、不 join，移入单槽退役；新任务立即启动；join 推迟到下一次切歌 / stop / 析构；切换窗口允许 2×ring 峰值豁免；不预解下一首。**

- **所有权**：退役槽 `MusicPlayList::m_retiredTask`，与 `m_currentTask` 同受 `m_taskMutex` 保护。任务（ring + decoder）的生命周期本来就归播放列表，`MusicPlayer::Impl` 只借快照。
- **只 abort 不 join**：新增 `AudioStreamDecoder::requestStop()`，只置 `m_stopRequest` / `m_abortDecode`；`stop()` = `requestStop()` + `join()`。`m_abortDecode` 仍是唯一取消信号。
- **单槽**：若退役槽已有旧任务，先 `joinRetiredTask()` 再移入；最多一个待回收任务，不建队列。
- **启动顺序**：退役（abort + 移槽）→ 新 decoder `start()` → 发布新 current。旧任务用旧 ring，新任务用新 ring，天然隔离，旧 PCM 不可能流进新曲。
- **join 点**：下一次 `replacePlaybackTask()`、`stopCurrentTask()`、`~MusicPlayList`。`stop` 是终态，同步 join 两槽，不走退役槽。
- **失败不上报**：退役任务的 `ERROR` / `EOS` 不被轮询；`handleDecodeError` 只对 current 生效；退役任务不得触发 `onError`、自动跳轨或写 `curPositionMs`。
- **峰值豁免**：切换窗口 = 新任务 `start()` 起 → 旧任务 join 完成止；窗口内允许 `2 × 262144(ring) + 2 × P + 2 × 64KB(AVIO) + O(1)`，窗口外回落到稳态公式。`peakHeldBytes` 不含 ring 且 per-decoder，判据拆成「两个 decoder 各自 ≤ P」+「两个 `PlaybackTask` 并存（`weak_ptr` 观察）+ 每个 ring 固定 262144 B」。
- **不预解下一首**：E4 只解冻「旧任务延迟退出」这一半；`prev/next` 预缓冲仍只留元数据，不建下一首的 decoder / ring。

## 考虑过的替代方案

**(a) 保持同步 stop + join。** 现状，最简单。否决：join 顶住消费节拍，网络源最坏 3 s；这是切歌卡顿的直接原因，也是本 effort 的靶心之一。

**(b) 预解下一首（提前建下一首的 ring + decoder）。** 能把切歌延迟压到最低。否决：E4 明令只留元数据；两套 decoder 长期并存会把常驻上界从「当前曲目」变成「当前 + 下一首」，内存与所有权复杂度都不抵收益。

**(c) 后台回收线程 / detach 旧解码线程。** 否决：规格 §4.2 明令「join 必须由发起新任务的同一线程完成，禁止 detach / 后台 join」；新增线程还会把析构时序复杂化。

**(d) 用 WorkQueue 异步任务做回收。** 否决：WorkQueue 本身就是那条被顶住的单线程，异步回收只是把 join 从「切歌函数内」挪到「同一个线程的下一拍」，消费节拍照样停。

**(e) 切歌也同步 join，但把 join 挪到消费线程之外。** 否决：没有第二个控制线程；新增线程即 (c)。

## 后果

- 切歌延迟不再包含 join；代价转移到下一次切歌 / stop（最坏 `rw_timeout=3 s`）。这是用「一次性延迟后移」换「每次切歌都不被 join 顶住」。
- 切换窗口内常驻峰值翻倍，属**过渡态**；稳态上界不变，`streaming-playback-spec.md` §5 既有行一字不动，本规格 §5.3 单列豁免条款。
- 需要新判据：旧 task 在新 task 启动后仍存活（`weak_ptr` 未过期）、下一次控制事件后过期、退役任务 ERROR 不上报。
- 风险集中在任务生命周期：退役 task 必须持有 ring 的 shared_ptr 直到 decoder join 完成，`PlaybackTask` 的成员声明顺序（decoder 在 ring 之后）不得改。
- 实现属图外 P2 effort，必须在 P1（首播门槛 + 欠流可观测）之后落地，否则无法判断软取消是否把欠流推高。

## 附注（实现后补记，2026-11-16）

上面的决策正文不动，这里只补实现落地时对「join 点」的细化：

- 退役槽由消费拍每拍调 `MusicPlayList::reapRetiredTaskIfFinished()`，只在 `AudioStreamDecoder::isThreadFinished()` 为真时才 join。旧解码线程已退出（本地文件路径下是毫秒级）⇒ 下一次消费拍（≤10 ms）就把退役槽收掉，实际 join 点**早于**「下一次切歌 / stop / 析构」；旧线程卡在网络读 ⇒ 该拍什么都不做，join 仍由下一次切歌 / stop 承担，「后果」第二条的代价不变。
- 不这么做的代价：join 只挂在控制事件上时，稳态播放期间会恒有 2 个 `PlaybackTask` 并存，切换窗口永不结束，规格 §5.3「窗口外回落到 `262144 + P + 64KB`」不成立。
- 判据随之从「下一次控制事件后过期」改为「退役线程退出后由消费拍回收」：旧 task 在新 task 启动后仍存活（`weak_ptr` 未过期）→ `isThreadFinished()` 为真 → `reapRetiredTaskIfFinished()` 后过期。见 `sdk/test/unit/test_playback_lifecycle.cpp` 的 `PlaybackLifecycle`「switching tracks retires the previous task and reaps it once its thread exits」。

## 附注二（2026-11-20）：per-decoder 峰值的字面口径与实测不符

上面的决策正文不动。决策正文把窗口判据拆成「两个 decoder 各自 ≤ P」+「两个 `PlaybackTask` 并存、每个 ring 固定 262144 B」两半；落地核对后第一半的字面数值不成立：

- `peakHeldBytes` 记的是解码器持有的 payload，稳态下同时约 4 包（本机实测 266240 B，`P = 65536 B`），所以「各自 ≤ P」在原供给形态下本来就量不到。
- 自动化判据因此取 `AudioRingBuffer::kDefaultCapacity + 2 × P + 64 KB`（与 `StreamingBudget` 套件（`sdk_streaming_tests`）的稳态上界同式），它比字面口径宽；分辨力由「整片常驻」这一类回归保证（把 `WAVExtractor::heldPayloadBytes()` 临时改成返回 `m_dataSize` 时实测 1062068 B 越界）。用例见 `PlaybackLifecycle`「both decoders stay inside the held-bytes cap across a switch」。
- 差异只登记、不改本 ADR 的决策正文与规格 §5.3 既有行；规格侧登记在 `doc/buffering-switching-spec.md` §7.1.2，取值写法的理由见 [ADR 0010](0010-test-injection-seams-and-criteria.md) 的「补充（2026-11-20）」。
- ring 口径那一半（窗口内 2 × 262144 B、窗口外回落）仍按 §5.3 公式手算，没有自动化用例。
