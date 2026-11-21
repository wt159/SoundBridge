# 0013 音量只能是软件增益：0–100、独立静音、不承诺毫秒级生效

- 状态：已接受
- 日期：2026-11-21
- 来源：[地图：占位控件转正](https://github.com/wt159/SoundBridge/issues/235)、[音量软件增益的落点与失真边界](https://github.com/wt159/SoundBridge/issues/236)、[音量的语义契约](https://github.com/wt159/SoundBridge/issues/238)、[持久化键表扩展与落盘时机](https://github.com/wt159/SoundBridge/issues/242)。规格落点 [`doc/app-ui-spec.md`](../../doc/app-ui-spec.md) §8.5；取证 [`docs/research/volume-gain-insertion.md`](../research/volume-gain-insertion.md)。

## 背景

`btn_volume` 是三个占位控件之一。勘察结论是：**SDK 侧没有任何音量基础设施**——`IAudioDevice` 被 ADR 0011 冻结为 11 个纯虚方法，没有 `setVolume`；输出路径是 `SDL_QueueAudio` 直推已格式化 PCM；**SDL2 不提供设备级音量**。因此「音量」在本仓库只能是软件增益，且必须新开公开接口。

三个候选落点各有代价：

1. `AudioDevice::write()` 内、`SDL_QueueAudio` 之前；
2. `MusicPlayer::Impl::consumeAudioData()` 的 ring → write 段；
3. 解码输出之后、入 ring 之前（重采样之后）。

## 决策

**增益落在候选 2：`MusicPlayer::Impl::consumeAudioData()` 的 ring → write 段，对既有的 4096 B 缓冲原地缩放。**

- **不改 `IAudioDevice`**：ADR 0011 的「11 方法 + 测试装饰器」边界不动。生产链只有两处设备写入（`MusicPlayer.cpp:666` 静音分支、`:709` 正常分支），都在这一个函数内；候选 2 覆盖全部写入。
- **取值 0–100 整数**，对外 `Player::setVolume(int)` / `volume()`，越界按端点钳制。`100` = 增益 1.0，字节原样输出；`0` = 输出全零 PCM。**不支持 >100**：放弃软件放大，同时免掉一整套削波策略（硬钳制 / 软限幅 / 上限告警）。
- **静音是独立布尔**，`Player::setMuted(bool)` / `muted()`；静音不改 `volume` 值，取消静音恢复原值。静音与 `volume == 0` 的差别是「能否恢复」。
- **块内线性渐变**：每块以内从上一块的增益线性过渡到目标增益，避免 4096 B（≈23 ms）的增益阶跃产生 zipper。`from == to == 1.0` 时整段跳过，默认配置零开销。
- **格式按 `m_devSpec.format` 分派**：S16 是生产主路径（`AudioDevice` 默认 `AUDIO_S16SYS`），同时支持 S32 / FLT32 / U8；其它格式**直通不改**（一次 Warning），宁可音量失效也不破坏字节。
- **生命周期**：跨曲目、跨 `stop`/`play`、跨设备重开保持。增益是会话级设置，不随曲目/任务重建，因此不放在 `PlaybackTask` 或 decoder 上。
- **不承诺毫秒级生效**：稳态下设备队列贴着 256 KiB 阈值，每拍只追加 4096 B，改动要等已排队 PCM 播完才可听，量级 ≈1.5 s（dummy 设备 ≈1.8 s）。规格只写「最终一致」，取证等 2 s 再判。

## 考虑过的替代方案

**(a) 落在 `AudioDevice::write()` 内。** 语义上最接近"设备音量"。否决：要给 `IAudioDevice` 加第 12 个纯虚，`AudioDevice` 与 `AudioDeviceDouble` 双实现 + ADR 0011 正文修订；设备层从此持有应用级状态，不再是"薄 SDL 包装"。`dynamic_cast<AudioDevice*>` 与"双指针并存"两条绕行都会在测试注入替身时让增益**静默失效**——ADR 0011 明确禁止转发层。

**(b) 落在解码输出侧（重采样之后、入 ring 之前）。** 否决：直接落在解码线程上，违反稳定性闸门「不把新增工作放进解码线程」；且每次切歌新建 decoder（`MusicPlayList.cpp:539`）会把增益重置，再叠加 ring 的 256 KiB，最坏延迟翻倍到 ≈3 s。

**(c) 用 `SDL_MixAudioFormat`。** 否决：它是**加法混合**原语（`dst += src*vol`），要求目标缓冲先清零；`volume` 上限 128 无法表达 >1.0；`volume == 0` 时函数直接 return 而不写 `dst`——静音会退化成"保留上一块内容"的陷阱。

**(d) 支持 >100 的软件放大。** 否决：收益低（用户真正想要的是"不够响"，而 100 已是原始电平），代价是一整套削波策略与失真判据；且与「不做设备音量」叠加后，放大只会放大同一个 PCM 的量化噪声。

**(e) 把静音实现成 `volume = 0` 的特例。** 否决：会丢掉"静音前的音量"，取消静音只能回到固定默认值，用户每次静音都要重设一次。

## 后果

- 新增公开面 4 个方法 + ABI 1 → 2；`MusicPlayer` 同步暴露同名接口供遗留路径使用。
- 增益数学被抽成可测的纯函数（`sdk/audio/common/PcmGain.hpp`），判据 `UI-VOL1` / `UI-VOL2` 由 `PcmGain` 单测套件承担，不需要真实声卡。
- 已知且被文档化的延迟：改动在 ≈1.5 s 内最终一致。要改善只能改 `kQueueThreshold`（欠流余量 1.5 s → 0.18 s）或在音量变化时清队列（丢已缓冲 PCM、可能有可听断点），两者都不在本图内。
- `m_devSpec` 与 SDL `obtained` 的既有错配（设备不重采样时可能不一致）不在本 ADR 处理范围；增益按 `m_devSpec` 解释字节，与重采样目标一致。
