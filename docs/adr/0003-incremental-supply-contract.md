# 0003 增量供给以包为唯一层间单元

- 状态：已接受
- 日期：2026-11-05
- 来源：[#47](https://github.com/wt159/SoundBridge/issues/47)、[#42](https://github.com/wt159/SoundBridge/issues/42)；相关事实见 [#43](https://github.com/wt159/SoundBridge/issues/43)、[#44](https://github.com/wt159/SoundBridge/issues/44)、[#46](https://github.com/wt159/SoundBridge/issues/46)、[#48](https://github.com/wt159/SoundBridge/issues/48)、[#50](https://github.com/wt159/SoundBridge/issues/50)

## 背景

地图 #42 的目的是让本地文件满足「边解边播」。#43 的事实核查先纠正了方向：生产链本来就是逐帧 PCM 写 256 KB ring，并不存在「整首 PCM 全量解码」；真正随音频长度增长的是**整首压缩 payload 全量常驻** —— 提取器 init 期把全部包塞进 `vector`，`getMetaData()` 再拼一份等长缓存。

#44 进一步发现：`readAt(offset,size)` 与 ffmpeg `avioRead` 早已按 offset 拉数据，缺的只是**把 demux 生命周期从 init 期挪到播放期**。但 m4a / asf / wma / ape / mkv 的标称时长必须逐包累加，mov/moov 之类的全局索引在尾部，因此「不缓存整首」与「拿到标称时长」之间存在真实张力：如果只是把整首 `vector` 换成随包数增长的索引表或逐包时间轴，常驻内存会换个名字回来。

同时 #22 的坏输入规格已把「拒绝的唯一出口是 `createExtractor() == nullptr`」定为基石（ADR 0002）。新增的 init 期扫描一旦在坏文件上失败就返回 `nullptr`，会悄悄开出第二个拒绝出口。

## 决策

**增量供给以包为唯一层间单元，demux 句柄由解码侧在播放期持有，任何随包数增长的索引 / 时间轴产物都不允许常驻。**

- 提取器退化为「格式能力 + 首帧 spec + 包工厂」，被解码线程 pull `nextPacket()`；`getMetaData()` 的整块视图退出生产链（仅测试量测仪器可用），不新增同步解码入口（#18 继续成立）。
- init 期允许**只读扫描**全文取标称时长（「读过 ≠ 常驻」），但只能留下 O(1) 定长摘要；播放期按 offset 现场重算包边界。
- **init 期扫描不得把可解性当判据**：坏包跳过、时长允许 `0`、扫描失败不返回 `nullptr`；可解性只在播放期由「包耗尽 + ring 0 字节 + EOS 汇总」兜底。
- 五种「不可增量」格式**不特殊化**，统一走同一契约；实现顺序可以分先后（#54），契约不分叉。
- `positionMs` 归消费端，`durationMs` 由提取器 init 期给出且允许 `0`；`segmentBaseBytes` 是消费端 O(1) 段基准、**不是 ring 成员**。

## 考虑过的替代方案

**滑动窗口包 vector。** 保留 `vector` 形状，只缓存固定 N 个包。改动最小，但「一层持有压缩数据」的本质没变，`getMetaData()` 仍需整块视图，常驻上界要重新论证，且无法保证第三方 demux 不在内部缓存全量。否决。

**按格式分叉。** 可增量格式走增量，五种不可增量格式保留整块视图并声明降级。务实，但会让「边解边播」变成逐格式例外，#48 的常驻上界失去统一判据，#49 的「全格式可 seek」也会出现「不支持」档。否决。

**保留整块视图，改用进程 RSS 做外层 gate。** 不必改数据通路，只加内存门禁。否决：RSS 受 Qt / ffmpeg / 堆分配器影响，Windows 与 Linux 两套噪声，无法把增长归因到「当前曲目」，判据会假红。

**init 期扫描失败即拒绝。** 诊断更早。否决：直接违反 ADR 0002 的唯一出口，坏文件会在工厂处被判死，而规格要求「能解多少播多少」。

## 后果

- 十个提取器子目录（11 个扩展名）都要从「init 期全量装载」改成「播放期按需产包」；四个 `initWithFFmpegDemux` 是各格式私有方法而非基类虚函数，改造面比预期大，见 `doc/streaming-playback-spec.md` 第 9 节的 E1–E11 与 expand–contract 说明。
- m4a / mkv 播放期现场重算包边界带来额外 CPU 成本；该成本只记为独立指标、不设阈值，端到端 drift 仍未判。
- 坏输入规格整体成立、四条基石零改写，但 55 条用例里有 2 条（ogg 截断、m4a `moov` 断）要把断言位置从工厂移到 EOS 汇总；判据数值不变。
- `AudioDecodeProcess` 明确降级为测试量测仪器，覆盖度矩阵的 `measuredMs` 仍由它换算；生产链与量测仪器同构，B4「量测走播放路径」反被加强。
- 「流式」一词从术语表退役，canonical 术语为「边解边播」；代码符号 `startStreaming` / `AudioStreamDecoder` 保留历史命名，属于已知代价。
- 判据由 #48 收紧为「一切随包数增长的结构都纳入分配记账」，否则本决策会形同虚设。
