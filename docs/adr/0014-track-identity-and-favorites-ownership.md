# 0014 曲目身份键派生自 fullPath，收藏归 app 持有

- 状态：已接受
- 日期：2026-11-21
- 来源：[地图：占位控件转正](https://github.com/wt159/SoundBridge/issues/235)、[曲目身份键的扩散面与路径归一化](https://github.com/wt159/SoundBridge/issues/237)、[TrackInfo 身份键形态与收藏状态归属](https://github.com/wt159/SoundBridge/issues/240)。规格落点 [`doc/app-ui-spec.md`](../../doc/app-ui-spec.md) §8.4；取证 [`docs/research/track-identity-key.md`](../research/track-identity-key.md)。

## 背景

收藏要跨启动持久化，就必须有一个**跨扫描稳定**的曲目键。此前公开 `TrackInfo` 只有 `{index, name}`：`index` 是本次扫描的序号（增删后整体漂移），`name` 可重名。内部 `FileProperties.fullPath` 存在，但从未出公开面。

取证给出一个直接反例：同一进程内、相差 0.58 s 的两条日志里，同一个物理文件分别写作 `D:/workspace/SoundBridge/music/music.wav` 与 `D:/workspace/SoundBridge/music\music.wav`。根因是 `fullPath` 是原始入参、零归一化（`MusicPlayList.cpp:256` 裸赋值），而 `parseFileName()` 只按 `/` 切目录。

## 决策

**给公开 `TrackInfo` 追加派生键 `std::string id`，`fullPath` 保留原值不动；收藏集合由 app 持有。**

- **键的形态**：新增字段而不是替换 `fullPath`。`fullPath` 是"用户给的原样路径"，`id` 是"归一化后的身份"，两者语义不同，合并会丢掉前者。
- **归一化落在唯一 choke point**：`MusicPlayList::_addMusic()`。本地目录扫描、`addMusic` 直调、URL 直通三条入口都经此处。
- **归一化口径**：绝对化 → 词法归一（消 `.` 与 `..`）→ 分隔符统一 `/` → Windows 下按宽字符 `towlower` 折叠大小写。**不做** `canonical()` / `weakly_canonical()`：解析符号链接需要 IO（扫描期每个文件一次），且文件删除后同一路径的 `id` 会变，反而破坏"稳定"。
- **URL 原样保留**：`http://` / `https://` 不做任何归一化。公开 `Player` 当前没有 URL 入口，这条是为 `MusicPlayList` 层与将来入口预留。
- **收藏归 app**：集合留在 `library/favorites`（`QStringList`），SDK 只提供键、不提供状态。理由见下。
- **`id` 只保证"跨扫描稳定"，不保证"列表内唯一"**：`addMusic` 没有去重，同一路径入列两次会得到两条同 `id` 记录。收藏按 `id` 判定，因此两条同 `id` 记录会同时显示已收藏——这是可接受的（它们确实是同一个文件）。
- **悬挂条目保留**：文件删除/改名后收藏项不清理；同路径文件重新出现即恢复。不做启动期 GC。

## 考虑过的替代方案

**(a) 直接透出 `fullPath` 当 id。** 零归一化成本。否决：已证同一文件产生两种写法，收藏会在"用 `/` 打开"与"用 `\` 打开"之间静默丢失，且这种错误只在用户视角可见（点了收藏、重启后没了），日志里什么都不留。

**(b) app 侧用 `name` 当键。** 零 SDK 改动。否决：同名文件互相串味，且改名即丢收藏；"静默错误"比"多改一个公开契约"贵得多。

**(c) 收藏不跨启动（仅会话内）。** 成本最低。否决：目的地直接缩水一半——"收藏"的价值就在跨会话。

**(d) 收藏由 SDK 持有**（新回调或 `PlayerViewState` 字段）。否决：收藏不是播放器事实，它是用户对曲库的标注；放进 SDK 要新增回调、扩状态面、评估 ADR 0004 的三态边界，代价远大于收益。规格 §1.1 因此新增"本地偏好"作为并列的第四条真相源，并写明它不参与回调链。

**(e) 归一化做在 app 侧。** 否决：`PlayerController` 只拿到 `name`（现加 `id`），拿不到原始 `fullPath`；即使透出，app 侧归一化意味着每个消费方都要重复实现一遍口径，迟早漂移。

## 后果

- 公开契约变更两处：`TrackInfo` 加字段、`SOUNDBRIDGE_ABI_VERSION` 1 → 2（`sdk/include/soundbridge/version.h`）。源码兼容：全仓无 `TrackInfo` 聚合初始化，唯一构造点是 `player_impl.cpp:74-79`。
- 内层 `MusicIndex`（`sdk/MusicPlayer.h`）必须同步加字段，否则 `id` 出不到外层。
- `onPlaylistChanged` 每次全量下发，千曲量级多一次字符串拷贝（≈3000 次堆分配、亚毫秒），不需要共享指针或索引化下发。
- 文本层风险为零：`toNativeString()`（UTF-8 → ANSI，有损）在生产链零调用，`fullPath` 全程 UTF-8；打开文件走 `toWideString` + `_wfopen`。
