# 0018 播放列表编辑：索引恒等于位置，位置不是身份

- 状态：已接受
- 日期：2026-11-22
- 来源：[地图：播放列表编辑的契约与规格](https://github.com/wt159/SoundBridge/issues/260)、[契约形态](https://github.com/wt159/SoundBridge/issues/262)、[索引语义](https://github.com/wt159/SoundBridge/issues/263)、[删除当前曲目](https://github.com/wt159/SoundBridge/issues/264)、[UI 入口与判据](https://github.com/wt159/SoundBridge/issues/265)。规格落点 [`doc/app-ui-spec.md`](../../doc/app-ui-spec.md) §8.9、[`doc/player-state-contract.md`](../../doc/player-state-contract.md) §4.1。

## 背景

在此之前，播放列表**只能整体扫描进来**：公开面只有 `addMusicDirectory` / `next` / `previous` / `setCurrentTrack` / `trackCount`，全仓对 `removeTrack` / `insertTrack` / `clearPlaylist` 零命中，`MusicPlayer::Impl::_addMusic` 是空实现。删除一条曲目这件事，在代码里没有任何入口。

要加这三个入口，先得回答两个此前被「没有删除」这个前提掩盖的问题：

1. **`TrackInfo.index` 会变吗？** 现状它恒等于 `m_musicListProperties` 的下标——但这是「只追加」的副产品，不是被维护的不变量。`m_curIndex`（`sdk/MusicPlayList.h:132`）实际是**条目个数**而不是当前下标，且从无自减；`m_selectIndex` 是**死字段**（构造后零读写）。同时存在两套「长度」：`m_curIndex` 用于取模与 `getMusicCount()`，`m_musicListProperties.size()` 用于越界检查（`MusicPlayList.cpp:266`）。只改一个，另一个立刻让 `.at()` 抛 `std::out_of_range`，而抛点在 WorkQueue 线程、无 catch，进程终止。
2. **`index` 是身份吗？** 有 5 处把它当身份用：`startCurrentTask` 的任务复用判据（`MusicPlayList.cpp:593`）、seek 守卫（`MusicPlayer.cpp:534`）、`shouldFillSilence`（`:703`）、`handleDecodeError`（`:837`）、位置会话过滤（`:1021-1022` / `:796`）。只要位置会变，这 5 处就会在**没有任何日志异常**的情况下分别表现为：暂停恢复时白扔缓冲、seek 静默失效、**声音消失而状态仍是 Playing**、**解码错误不上报且自动跳轨失效**、**进度条停住**。

`TrackInfo.id` 不能拿来当身份：它在 `player.h:18-22` 被明确写成**不保证列表内唯一**，同一路径入列两次会得到两条同 id 记录。

## 决策

**三条，必须同时成立。**

### 1. `TrackInfo.index` 恒等于列表位置（compaction）

删除即压缩重排，不留空洞。三条不变量写进规格：

- `MusicProperties::index == 该元素在 m_musicListProperties 中的下标`，对**每一次**变更成立；
- 原子计数（原 `m_curIndex`，改名 `m_count`）恒等于 `size()`，只在队列线程写、任意线程读；
- `m_selectIndex` 删除。

### 2. 位置不是身份：新增进程内 `uid`

`MusicProperties` 增加 `uint64_t uid`，在 `_addMusic` 里从成员计数器单调分配（与 `deriveMusicId` 同一个唯一产生点）。`PlaybackTask::trackIndex` → `trackUid`、`m_positionTrackIndex` → `m_positionTrackUid`、失败集合 `m_failedIndices` → `m_failedUids`，上述 5 处判据全部改为 uid 比较。`resetTrackProgressExcept(keepIndex)` 一并改按 uid。

uid 只保证「进程内、对象生命周期内」唯一：不跨启动、不写进 `TrackInfo`、不改任何公开签名。

### 3. 删除当前曲目是「补位」，不是一次 `next`

`next()` 允许没有下一首（Sequential 末尾 clamp、`SingleOnce` 直接返回 false，既有用例 `next at last track and previous at first track clamp` 钉住了它）；删除当前曲目**不允许没有当前曲目**——列表非空却没有当前曲目，会同时造出三个死路：`next()` 因 `m_selectMusicProperties == nullptr` 直接返回（`MusicPlayList.cpp:281`）、`play()` 报 `PlayerNoCurrent`、UI 拿不到可高亮的行。

于是补位序按模式生成，但**保证有落点**：

| 模式 | 补位序 |
|---|---|
| Sequential / Loop | 从删除点 `p` 起的环形顺序 `[p…n-1] [0…p-1]` |
| Random | 剩余全部条目的随机全序 |
| SingleOnce / SingleLoop | 模式不提供候选，**退化为顺序补位序** |

落点跳过 `m_failedUids` 中已知失败的条目；逐个试播失败则记账并继续；全部不可播或列表为空才停止。**Paused 下补位后保持 Paused**，不擅自恢复播放。

## 考虑过的替代方案

**(a) 墓碑：`index` 稳定、另设位置字段。** 好处是失败集合、在途任务、UI 高亮全都不用重映射。否决：`TrackInfo.index` 的公开语义已经在 app 侧被当作位置使用（`playlist.at(index)` 取标题、`mListWidget->item(index)` 取行），既有用例更直接断言 index 必须是 `0..n-1` **连续**（`sdk/test/unit/test_player_integration.cpp:2127-2140`）。允许空洞等于改掉一条公开语义，属 breaking change，且 UI 要多维护一层 `index → row` 映射，收益为零。

**(b) 把失败集合改成按 `TrackInfo::id` 记账。** 看似更「稳」，实际更错：id 不唯一，一条坏文件会**毒化同路径的所有副本**——用户故意把同一首加两次、其中一次解码失败，两条都永久跳过，而另一条可能根本没被尝试过。失败是**条目级**事实，不是**路径级**事实，键必须是条目身份。这也是选 uid 而不是选 id 的直接原因。

**(c) 删除当前曲目复用 `advanceToNextTrack()`。** 否决：它在末尾返回 false、在 `SingleOnce` 返回 false、在 `SingleLoop` 重播同一首——三种情况都违反「必须有落点」。也不能改用 `skipToNextPlayable()`：它忽略播放模式（纯顺序模运算）。

**(d) 单曲模式下删除当前曲目直接停止。** 否决：用户删掉一首歌会看到「播放器不响了，列表里还有 9 首」。地图裁定的停止条件只有「列表变空」。

**(e) 引入「已播集合」，把 Random 变成 shuffle-bag。** 否决（本图内）：compaction 之后随机模式的候选集 `[0, count)` 与真实位置恒等，错位问题自动消失，没有 bug 要修；而引入集合会改掉 `Random` 的既有可观测行为（同一会话内不再重复抽中同一首），属新语义。登记为将来 effort。

**(f) 三条决策拆成三篇 ADR。** 否决：它们互相依赖——补位语义依赖索引压缩（`p` 之后的下标会变），而两者都依赖「位置不是身份」。拆开会产生交叉引用与重复背景。按 ADR 三条件核对：主决策（索引恒等于位置 + 位置不是身份）难逆转、无上下文会惊讶、且有真实取舍；补位语义与失败集合口径是同一次契约扩张的组成部分，不是独立选择。

## 后果

**正向**

- 三个公开命令落地后，「删除后索引重排」在 SDK 内部是**唯一**的索引语义，不再有两套长度、不再有「index 恰好等于位置」的偶然事实。
- 5 处「拿位置当身份」的静默故障从根上消失；未来做重排（本图界外的将来 effort）不需要把这套同步补丁再写一遍。
- `Player` 公开面只增三个 `void` 方法，`PlayerCallbacks` 与 `TrackInfo` 一字未动，**ABI 保持 2**（`Player` 无虚成员、单成员布局，既有符号的修饰名与对象大小都不变）。

**负向 / 已知代价**

- 每次删除是 `O(n)` 重编号 + UI 全量重填。实测 1000 项 `clear()+addItem()` 为 1–2 ms，不构成约束；连续删 k 条是 `O(k·n)`，病态规模才需要「批量删除」命令（属新命令，本图不做）。
- `uid` 是**内部**概念，不进公开面。代价是客户端仍只拿得到 `index`，因此「当前曲目下标变了必须补发 `onTrackChanged`」成为一条硬契约（规格 §4.1 R2）——这是不给公开面加身份字段的必然代价。
- `m_failedUids` 里会留下已删除条目的 uid（无害且不增长过快，进程内有效）。不在删除路径清理，避免多一次全表扫描。
- `Delete` 的操作对象是「当前选中行」，而渲染路径总把播放行设为选中——所以它通常就是正在播的那一首。不引入「用户是否亲手选过」的标记（那会是第二个真相源），登记为已知代价。
- `addTrack` 没有 UI 入口：`QFileDialog` 是 `QDialog` 子类，被 ADR [0015](0015-popup-base-and-list-mask-removal.md) 否决。它今天的用途是测试造数据。

**反悔条件**

- 若将来出现「十万级列表 + 高频删除」的真实场景，`O(n)` 重编号需要重新评估；在此之前不预先优化。
- 若将来要让客户端持有跨命令稳定的曲目身份，应给 `TrackInfo` 加字段而不是让客户端缓存 `index`——那是一次 ABI 变更，须另开 ADR。
