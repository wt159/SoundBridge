# 0011 设备替身与注入面：接口边界、装饰器脚本与三条设备层判据

- 状态：已接受
- 日期：2026-11-21
- 来源：[地图：设备替身与设备层判据的可判定化](https://github.com/wt159/SoundBridge/issues/200)、[dummy 驱动下的队列语义与容量上限](https://github.com/wt159/SoundBridge/issues/201)、[写失败的注入脚本形状与两个写入点](https://github.com/wt159/SoundBridge/issues/202)、[替身契约：spec、枚举转发与生命周期](https://github.com/wt159/SoundBridge/issues/203)、[失败脚本的落点与静态 open 钩子的去留](https://github.com/wt159/SoundBridge/issues/204)、[三条判据的断言式与能红变异点](https://github.com/wt159/SoundBridge/issues/205)。规格落点 [`doc/buffering-switching-spec.md`](../../doc/buffering-switching-spec.md) §7.2 / §11。

## 背景

`doc/buffering-switching-spec.md` §11 把三条设备层契约记为不可判定：`deviceWriteFailures` 在 dummy 驱动下不可构造（「永久口径」）、`AudioDeviceStartFailed` 分支不可达（`AudioDevice::start()` 恒返回 0）、切换窗口的 ring 口径峰值靠 §5.3 公式手算。

三者的根因是同一个：**设备不可替换**。`MusicPlayer::Impl` 直接 `std::make_shared<AudioDevice>()`（`sdk/MusicPlayer.cpp:156`），`AudioDevice` 是具体类，仓内没有任何设备替身；`sdk/test/mocks/AudioMocks.hpp` 只 mock 了 decode/data 回调。

ADR 0010 曾否决「设备后端替身」，理由是该 effort 的目的地不含写失败、「dummy 队列已够真实、替身换不来精度」。那条否决成立的前提是「替身只为提高精度」；本文件的目的是**构造不可构造的失败**，前提已变。ADR 0010 原文保留不动，其末尾附注（2026-11-21）指向本文件。

一手事实见 [`docs/research/device-double-evidence.md`](../research/device-double-evidence.md)：dummy 播放侧按 20 ms 名义拍长消耗（实测 23.52–23.60 ms、≈150 KB/s）、SDL 侧无队列容量上限、经项目 API 可稳定复现的 `write()` 失败只有「未 open」与「已 close」、同进程只允许一个输出设备、device id 会复用。

## 决策

**抽 `IAudioDevice` 纯虚接口，生产路径直接持真实实现，测试用装饰器包住真实设备并按调用序号注入失败；三条设备层判据由该替身与一个退役槽读出口支撑。**

- **接口边界**：`IAudioDevice` 覆盖 `AudioDevice` 的 11 个公开实例方法（`getDeviceList` / `getDeviceSpec` / `selectDevice` / `open` / `close` / `isOpen` / `start` / `stop` / `write` / `getQueuedBytes` / `clearQueue`）。静态测试钩子不进接口。设备枚举与选择**转发给真实设备、不模拟**——`MusicPlayer` 从不调用这两个方法，替身不承担枚举保真责任。
- **生产路径不包转发层**：`Impl` 持 `shared_ptr<IAudioDevice>`，实际对象就是 `AudioDevice`。代价是每拍一次虚派发；收益是不引入「真实设备行为」的中间人。
- **注入点**：遗留类 `MusicPlayer` 的实例级 `setAudioDeviceForTest(std::shared_ptr<IAudioDevice>)`，与 `queuedBytesForTest()` / `metricsForTest()` 同类出口；`play()` 前调用。**设备已 open 时拒绝**（返回 `false` + 日志、保持原指针）：无条件替换会让已 open 的真实设备无人 `close`，而同进程只允许一个输出设备，后续用例会集体失败。不进 `soundbridge::Player`。
- **替身形态**：测试代码里的装饰器，内持 `shared_ptr<IAudioDevice>`（通常就是真实设备），默认全直通；脚本命中的调用返回 −1，`close()` 时清零。脚本生命周期跟 `close()` 走，与 `deviceWriteFailures` 按播放任务累计、`start()` 清零的口径对齐，避免用例顺序相关的假红。
- **脚本形状**：一次性计数脚本——第 N 次调用返回 −1，其余放行。只有它能支撑「恰好 +1」的等值断言；开关式失败每拍累积、计数无上界，只能断言非零，分辨力为零。
- **两个写入点分别覆盖**：`MusicPlayer::Impl::consumeAudioData()` 的静音分支与正常分支都调 `noteDeviceWriteFailure()`，各一条用例。静音分支的脚本条件收窄为「仅全零负载的 `write()` 失败」，素材用非零样本的 tone wav——静音填充块恒 4096 B 全零，因此失败只能落在静音分支，分支归属由脚本条件保证而不是靠旁证推断。
- **纪律判定**：失败脚本只存在于测试代码，生产路径没有任何钩子读。消费拍经虚函数进入测试替身**不是** ADR 0010 意义上的「钩子落在消费拍」，不新增例外。ADR 0010 的例外仍只有 `FileSource::setReadDelayForTest()` 一处。
- **`getDeviceSpec()` 是死值**：`m_devSpec` 在 `Impl` 构造时取一次（`sdk/MusicPlayer.cpp:157`）并传给 `MusicPlayList`（`sdk/MusicPlayList.cpp:21-26`），注入发生在其后；替身的返回值不进入重采样判定（`sdk/audio/decode/AudioStreamDecoder.cpp:173-180`）、`deviceSpecSupportsResample()`（`:111`）或位置换算（`:466`）。不做可配置项。
- **静态 open 钩子退役**：`AudioDevice::setForceOpenFailureForTest(bool)`（票 #110 的「一次限定例外」）在替身落地后删除，「open 失败 → `AudioDeviceOpenFailed` → 状态」判据改用替身脚本重写。删除与重写必须在同一个 PR 内完成，不留「两端都缺」的中间态；复位责任从进程级静态移到替身析构。
- **三条判据的断言式**：

  | 判据 | 断言 | 余量 / 变异点 |
  |---|---|---|
  | 写失败（正常分支） | `deviceWriteFailures == 1`，注入后设备队列继续增长 | 等值断言；删记账或改成每拍 +1 都会红 |
  | 写失败（静音分支） | `deviceWriteFailures == 1` 且 `silenceFills >= 1` | 分支归属由「仅全零负载失败」保证 |
  | start 失败 | 2 s 内上报 `AudioDeviceStartFailed`，`state() == StoppedState` | 真实 `start()` 恒 0 ⇒ 去掉脚本必红 |
  | ring 口径峰值 | 切歌后 500 ms 内 `retiredTaskForTest()` 过期，`currentTask()` 仍有效 | 回收节拍 10 ms，≥25× 余量；删 `reapRetiredTaskIfFinished()` 必红 |

- **`start()` 失败的边界**：只断言「上报 + 未进 `PlayingState`」，不断言清理。失败后设备仍 open、解码任务仍 start，清理语义另开票，不在本决策内。
- **退役槽读出口**：`MusicPlayList::retiredTaskForTest()` 返回 `weak_ptr<PlaybackTask>`，不延长生命周期；这是规格 §5.3 已写下的「`weak_ptr` 观察」判据形式缺的那一个出口。

## 考虑过的替代方案

**(a) 播放中 `close()` 设备，零接缝构造写失败。** 能省掉整个接口抽取。否决：`m_isOpen` / `m_deviceID` 是普通字段（`sdk/audio/device/AudioDevice.cpp:12-19`），`close()` 在控制线程写、`write()` 在消费线程无同步读，判据会建在数据竞争上；device id 会复用，close 后 `write` 未必稳定拿到 −1（可能静默写进新设备且 `rc=0`）；失败次数由线程调度决定，只能断言非零，抓不到重复计数。

**(b) 在现有静态钩子上继续加（`setForceWriteFailureForTest` / `setForceStartFailureForTest`）。** 不动构造链，改动最小。否决：每补一条缺口就多一个永久钩子，而 ADR 0010 已把钩子纪律收紧为「只此一处」；三个静态开关并存后，读者需要判断哪个入口作数。

**(c) 全内存队列替身（纯 fake，不包真实设备）。** 能精确控制水位与时序。否决：ADR 0010 已论证 dummy 队列按真实节拍消耗、替身换不来精度；纯 fake 还会丢掉「真实 SDL 排队」这一层，判据与生产行为脱节。

**(d) 把 `getDeviceSpec()` 做成可配置，为将来测异构设备规格留口。** 否决：注入在 `Impl` 构造之后，该返回值是死值；做成可配置会造出一个看着能改、实际不生效的旋钮。

## 后果

- 生产代码多一层虚派发（每拍一次），`AudioDevice` 的 11 个方法变虚；它是 SDK 内部类型，不影响 `soundbridge::Player` 的公开面。
- 设备枚举与选择在替身下转发真实设备，替身不模拟枚举；真实设备行为仍只由人工冒烟覆盖。
- `deviceWriteFailures` / `AudioDeviceStartFailed` 从「永久口径 / 不可达」变为可判定，但**只在 dummy 驱动下成立**；真实设备（硬件水位、飞块、`SDL_QueueAudio` 报错）仍不可自动化。
- 删除静态 open 钩子后，`ForceAudioDeviceOpenFailure` 这类 RAII 复位辅助随之消失；测试改为持有替身、由析构复位，不再依赖进程级状态。
- ring 口径峰值仍不等于「窗口内 2×ring 的字节峰值」本身：判据观察的是**窗口是否关闭**（退役槽过期），不是逐字节累加。证明力边界写在此处，避免后来者把它当成字节级断言。
