# 0019 本地数据源：探测后关闭，用租约计数决定开合

- 状态：已接受
- 日期：2026-11-22
- 来源：[地图：千曲库的本地数据源生命周期与句柄预算](https://github.com/wt159/SoundBridge/issues/273)、[开合契约](https://github.com/wt159/SoundBridge/issues/275)、[重开可行性研究](https://github.com/wt159/SoundBridge/issues/274)、种子 [千曲库受限于每曲常驻 FileSource](https://github.com/wt159/SoundBridge/issues/270)。规格落点 [`doc/streaming-playback-spec.md`](../../doc/streaming-playback-spec.md) §4.5。

## 背景

`FileSource` 在构造时 `_wfopen` 并常驻持有 `FILE*`，析构才 `fclose`（`sdk/extractor/FileSource.cpp:39-44`、`:69-81`）；它没有 `close()` 重写、没有重开能力、没有引用计数。播放列表条目把 `shared_ptr<DataSource>` 与 extractor 一直挂在 `MusicProperties::processProperties` 上，于是「列表里有多少条曲目」就等于「进程里开着多少个文件流」。

Windows / MinGW 的 msvcrt 并发流上限是 512，且 `_setmaxstdio` 在该运行时返回 `-1`。实测同一文件入列 500 条后，第 509 条起 `FileSource initCheck failed`，`getMusicCount()` 停在 508（[#270](https://github.com/wt159/SoundBridge/issues/270)）。Linux 侧上限是 `ulimit -n`（通常 1024），同一形态只是阈值不同。`doc/app-ui-spec.md` §8.6 把千曲量级当作可接受规模，`#268` 的 A1 判据因此只能从 1000 降到 500。

要让千曲库成立，必须把「条目持有数据源对象」与「条目持有打开的句柄」拆开。可选的做法有三类：惰性打开（入列不打开）、探测后关闭（入列打开、读完就关、播放前重开）、全局句柄池（配额与换出）。第一类会拆掉 `addTrack` 的失败契约与 `durationMs` 来源；第三类把判定语义变成调度问题，且跨平台阈值仍然存在。

## 决策

**四条，必须同时成立。**

### 1. 探测后关闭，而不是惰性打开

入列仍按现序打开文件、`initCheck`、建 extractor、读头、取 `AudioSpec` 与 `durationMs`，随后立即释放句柄。理由：`addTrack` 的失败即报是既有公开契约（`doc/player-state-contract.md` §4.1），缺失/坏文件必须在入列时被拒；`durationMs` 与 `AudioSpec` 也必须在此刻可得（`MusicPlayer.cpp:495` 的 prefill 门、`:1053` 的时长回调）。惰性打开会把这两件事同时推到播放期，等于重写契约，且省下的扫描 IO 可以忽略。

### 2. 开合由 `FileSource` 内的引用计数决定

`FileSource` 增加原子的 `m_openRefs` 与两个内部方法：`acquire()`（0→1 时打开并重求长度，失败回退到 0 并返回错误）、`release()`（归零时关闭）。`initCheck()` 退化为二态——「此刻可否读」，不再承担「是否曾经打开过」的含义。

不做「显式 open/close + 调用点守卫」：重播同一曲时，旧退役任务与新任务共享同一条目，任何一处收尾点漏写「owner 指针不同才关」，就会关掉正在解码的 fd——而在 m4a/asf/ape/mkv 上症状是旧 `size` / 假 EOF 的静默截断，不是崩溃。计数把这条正确性从「每个收尾点都要记得」变成「不可能写错」。

### 3. `PlaybackTask` 用 `SourceLease` 持有租约，靠声明顺序保证「关闭晚于 join」

`PlaybackTask` 成员顺序固定为 `owner`（最前）、`lease`、`ring`、`decoder`（最后）。逆序析构 ⇒ decoder 先析构（`stop()` 内 join），随后释放租约。这与 `owner` 保活到 join 完成是同一策略：把时序不变量编码进声明顺序，而不是靠调用点在 `stopCurrentTask` / `joinRetiredTask` / `reapRetiredTaskIfFinished` 三处各写一遍。

### 4. 探测租约在 `getAudioSpec()` 之后立即释放

WorkQueue 串行，扫描期峰值句柄数为 1。不做批量延迟关闭：峰值不可控，且没有收益。

## 考虑过的替代方案

**(a) 惰性打开：条目只留路径，播放时才建 `FileSource` 与 extractor。** 否决：`addTrack` 的失败契约（一次 `onError(既有码, detail, -1, path)`）与 `durationMs` / `AudioSpec` 的来源都依赖入列期的探测；缺失文件会静默入列成功，`test_playlist.cpp:399-420` 一类判据全部失效。这不是资源优化，是契约重写。

**(b) 全局句柄池 / LRU 换出。** 否决：判定语义变成「此刻池里有空位吗」，同一个操作在负载不同时成败不同；跨平台阈值问题仍在（512 / 1024 只是数字不同）；实现与判据成本远高于收益。

**(c) 显式 `open()` / `close()`，收尾点各自守卫。** 否决：见决策 2 的静默截断风险。租约把「谁还能读」变成计数，而不是靠指针比较。

**(d) 条目只留路径 + spec，播放前重建 `FileSource` 与 extractor。** 否决：研究已证零格式必须重建——10 个格式构造期都不持有 fd，demux/reader 面在播放期解码线程内懒建（`docs/research/datasource-reopen-facts.md`）。重建只增加每次切歌的读头成本，不解决任何正确性问题。

**(e) 调高 CRT 并发流上限。** 否决：`_setmaxstdio` 在本项目使用的 MinGW msvcrt 上返回 `-1`（实测），不可用；且即便可用，Linux 侧仍受 `ulimit -n` 约束，只是把阈值推远。

## 后果

**正向**

- 常驻句柄从 O(n) 降到 O(1)：当前曲 ≤1 + 退役保活 ≤1。千曲库在 Windows 与 Linux 上同时成立，不再依赖平台阈值。
- 「关闭晚于 join」与「重播同曲不误关」成为结构性事实，不依赖三个收尾点的实现纪律；`PlaybackTask::owner` 的 UAF 教训（`#268`）被同一种手法延伸覆盖。
- 公开面零改动：`ErrorCode` 不新增、`onError` 签名不变、`Player` 方法不变，ABI 保持 2（新增方法只在 `sdk/extractor/` 内部头）。

**负向 / 已知代价**

- 每次切歌多一次 `open()`。本地 SSD 上是亚毫秒级；网络盘或机械盘上的具体数值未测，属已知未知。若将来实测超过可接受窗口，可考虑「预开下一条」，但那会以句柄预算 +1 换取延迟，属新决策（已登记在地图 fog）。
- 探测期仍要读头，扫描总耗时不变。`doc/app-ui-spec.md` §8.6 的「千曲量级约数十毫秒」是否仍然准确，由判据票（#279）核对，本 ADR 不改该数值。
- `m_length` 按每次 `acquire()` 快照：文件在探测后变长会读不到追加部分，变短会短读或 `FAILED_TRANSACTION`。这是既有行为的显式化，不是新缺陷。
- 引用计数要求 `acquire` / `release` 严格配对。诊断依赖内部 gauge 与断言（由 #278 / #279 落地），不暴露给调用方。

**反悔条件**

- 若目标硬件上切歌因 `open()` 出现可感知延迟（例如网络盘），且预开被证明必要，应另开 ADR，把「预取」与「句柄预算」一起重新裁决。
- 若将来某个格式的实现改为必须常驻 fd（例如自带 demuxer 持有 `FILE*`），该格式需单独评估，不能默认沿用本 ADR。
- 若引入并行扫描（`addMusicDir` 并发探测），峰值句柄数不再为 1，需要重新给上限并补判据。

**收口注记（2026-11-22）**：来源地图 [#273](https://github.com/wt159/SoundBridge/issues/273) 已关闭，但本 ADR 的两条未决项**不随之闭环**：①切歌 `open()` 在网络盘 / 机械盘上的延迟仍未测；②并行扫描的句柄峰值上限仍未裁决。两者都属新决策（见上两条反悔条件），需各自另开地图，不在本图上继续。
