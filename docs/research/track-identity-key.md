# 曲目身份键的扩散面与路径归一化（#237 取证）

> 取证票：GitHub issue #237（`wt159/SoundBridge`，地图 #235 的 research 票，阻塞 #240）。
> 勘察日期：**2026-11-21**；基线 commit：**`881516d58`**（`git log -1 --format=%h`，2026-11-21，merge PR #234）。
> 取证方式：**只读源码 / 文档 + 读本机历史测试日志**。遵循 `research` 技能的一手来源优先原则：事实与推断分开标注，每条事实附「路径 + 符号名（+ 行号辅助）」。
> 本次**未修改任何生产代码 / 用例 / CI / 规格**，**未执行构建或测试**，**未做任何 git 写操作**（`git log` / `git ls-files` 只读命令除外）。
> Grep 一律限定 `path` 到 `sdk/` 子目录、`app/`、`doc/` 或具体文件，并以 `!sdk/3rdparty/**` 排除 8.4 万已追踪文件；未在仓库根做无范围检索。
> 行号会漂移（`docs/research/public-api-contract-facts.md` 头部已就此留过教训）；引用以**路径 + 符号**为准。

---

## 结论

**可行，但「直接透出 `fullPath`」不可行。** 材料是有的（`fullPath` 是唯一一个每个曲目都有、且只在单一入口赋值的字段），但这个字符串当前**不是稳定键**：同一物理文件在同一进程内就已经出现了两种写法。因此推荐形态是「**新增派生键 `id`，不把 `fullPath` 本身当契约**」。

具体裁定：

1. **新增 `std::string id` 到公开 `TrackInfo`，与 `name` 并列，不替换、不改名 `name`。** `id` 是**从 `fullPath` 派生**的不透明键，`fullPath` 继续保留原始值（原始值在打开文件、错误回调、日志三处被消费，见 §2 清单）。
2. **归一化只能做在产生侧，不能做在消费侧（app）。** 唯一 choke point 是 `MusicPlayList::_addMusic()`（`sdk/MusicPlayList.cpp:248`，赋值行 `:256`）——本地文件、`addMusic` 直调、URL 直通三条入口全部经过它。放在这里可以做到「一处赋值、覆盖全部入口、不改任何上层签名」。
3. **归一化口径（Windows）**：绝对化 → `lexically_normal()` → 分隔符统一 `/` → 宽字符小写折叠。**（Linux）**：绝对化 → `lexically_normal()`，不做大小写折叠。**不做** `canonical()` / `weakly_canonical()`（会解析符号链接、需要 IO，且文件删除后同一路径的 id 会变）。
4. **网络曲目**：`id` = 用户输入的原始 URL 字符串（当前即 `fullPath`），**不做 URL 归一化**。这是本票唯一「不用改」的地方：重定向目标从未写进 `fullPath`（`HttpSource` 只更新内部的 `m_host/m_port/m_path`），所以 id 不受嗅探 / 302 影响，反而稳定。
5. **成本可接受**：`onPlaylistChanged` 每次扫描完成发一次（`sdk/MusicPlayer.cpp:204-208`），不是每帧。千曲量级 ≈ 3 次深拷贝 × 1000 条 ≈ 3000 次堆分配、约 0.2 MB 瞬时内存、亚毫秒量级。**不需要** `shared_ptr` / 索引表 / 常驻 id 池。
6. **兼容**：源码兼容（全仓 `TrackInfo` 无聚合初始化点，唯一构造处是 `sdk/player_impl.cpp:74-79` 的逐字段赋值）；**ABI 不兼容**（公开结构体尺寸变化），须 `SOUNDBRIDGE_ABI_VERSION` +1（`sdk/include/soundbridge/version.h:8`）。

**否决「沿用 `name` 当键」**：`name` 是 `fileName`（去扩展名），本身既不唯一（同名文件）也不稳定（`FileProperties::parseFileName()` 只按 `/` 切，Windows 上反斜杠路径会让 `name` 带上整段目录前缀，见 `doc/app-ui-spec.md:132`）。

---

## 影响面清单

### 2.1 数据流与新增字段的落点

| 位置 | 当前用法 | 新增 `id` 后的影响 |
|---|---|---|
| `sdk/extractor/FileSearch.cpp:29` | `fileVec.push_back(sdk_utils::toUtf8String(iter->path()))` | 本地路径的诞生点。此处已有 `boost::filesystem::path` 对象，是做归一化最省事的点；但它只返回 `vector<string>`，要在此算 id 就得改返回类型 |
| `sdk/MusicPlayer.cpp:193` | `std::vector<std::string> musicList = recursiveFileSearch(dir);` | 不改；但 `dir` 的字符串形态直接决定所有子条目 id 的稳定性（见 §2.4） |
| `sdk/MusicPlayer.cpp:199-200` | `m_musicList->addMusicWithNotify(path, ...)` | 不改 |
| `sdk/MusicPlayList.cpp:256` | `fileProperties.fullPath = musicPath;` | **唯一赋值点**；新增 `id` 应在此处由 `fullPath` 派生（推荐），或在此处一并接收 |
| `sdk/MusicPlayList.cpp:362` | `m_musicListProperties.push_back(musicProperties)` | **无去重**：同一路径调两次 `addMusic` 会入列两条、id 相同。`id` 因此只保证「跨扫描稳定」，不保证「列表内唯一」 |
| `sdk/MusicPlayList.h:26` | `std::string fullPath` | 保持原值不变（打开文件 / 错误回调 / 日志消费它） |
| `sdk/MusicPlayer.cpp:868-871` | `MusicIndex index; index.index = …; index.name = property->fileProperties.fileName;` | **必须同步加字段**，否则 `TrackInfo` 拿不到 id：这里是 id 从 sdk 内层流向外层的唯一通道 |
| `sdk/MusicPlayer.h:31-34`、`:43` | `struct MusicIndex { int index; std::string name; }`；`onMusicPlayerMusicListChanged(std::list<MusicIndex>)` **按值传** | 加字段 = 每跳多一次字符串深拷贝；此回调本身已按值传列表（一次多余拷贝） |
| `sdk/player_impl.cpp:74-79` | `TrackInfo info; info.index = …; info.name = item.name;` | 补一行 `info.id = item.id;`。这是全仓唯一的 `TrackInfo` 构造点 |
| `sdk/include/soundbridge/player.h:15-18` | `struct TrackInfo { int index; std::string name; };` | 公开契约变更；尺寸 40 → 72 字节（MSVC x64 / libstdc++ 同为 `std::string` 32 B） |
| `app/playercontroller.cpp:207-215` | 只读 `track.name`，拼 `QStringList` | **不改也能编译**（无聚合初始化、无 `sizeof` 断言）。要用 id 需自己加一份并行 `QStringList` |
| `app/playercontroller.h:15` | `PlayerViewState::playlist` 是 `QStringList` | 若 UI 要用 id，需加 `QStringList trackIds` 或换成 `QList<条目结构>`；这是 app 内部结构，不影响 SDK |
| `sdk/test/mocks/PlayerMocks.hpp:140-149` | `track_names[track.index] = track.name;` | 编译不受影响。要断言 id 才需扩 probe |
| `sdk/test/TestSdkSuite.cpp:102-108` | 同上（`PublicPlayerProbe`） | 同上；`:590-625` 的既有断言只查索引连续性与数量，**不会因新增字段变红** |
| `sdk/test/unit/test_bad_input.cpp:199` | `onMusicPlayerMusicListChanged(std::list<sdk::MusicIndex>) override { }` | 空实现，只受 `MusicIndex` 加字段影响（仍是重写、不需改） |
| `sdk/test/TestMusicPlayer.cpp:50` | 同上（listener 记录 `music_list`） | 同上 |
| `sdk/MusicPlayer.cpp:356` | 遗留 `void MusicPlayer::Impl::_addMusic(const std::string &) { }` — **空实现** | 遗留 `MusicPlayer` 路径里这个 `_addMusic` 是死代码，`MusicPlayer` 实际只走 `m_musicList->addMusicWithNotify()`（`:200`）。**无需同步**，但改 `MusicPlayList` 签名时别以为它是调用方 |
| `sdk/test/unit/test_playlist.cpp:89-107` | 直接构造 `FileProperties` 并断言 `parseFileName()` | 不受影响（不涉及 `TrackInfo` / 新字段） |
| `sdk/include/soundbridge/version.h:8` | `#define SOUNDBRIDGE_ABI_VERSION 1` | 加字段属 ABI 破坏 → +1 |

### 2.2 问 1：产生点与编码

**事实（赋值点）**
- 只有一处赋值：`sdk/MusicPlayList.cpp:256` `fileProperties.fullPath = musicPath;`，在 `_addMusic()` 内、任何 `DataSource` 构造之前。`FileProperties` 其余三字段由 `parseFileName()` 从 `fullPath` 派生（`sdk/MusicPlayList.h:31-62`）。
- 本地路径字符串的诞生：`sdk/extractor/FileSearch.cpp:29`，`boost::filesystem::path` → `sdk_utils::toUtf8String(iter->path())`。
- 目录扫描的唯一生产调用者：`sdk/MusicPlayer.cpp:193`（`addMusicDir()`）。

**事实（编码）**
- `fullPath` 在进程内**始终是 UTF-8**。入口 `toUtf8String()`（`sdk/utils/Utf8Path.h:13-28`）在 Windows 上走 `p.wstring()` → `WideCharToMultiByte(CP_UTF8, …)`（`:16`、`:19`），非 Windows 走 `p.string()`（`:26`）。
- 中间不再有任何编码转换：`MusicPlayList` 的 `musicPath` 参数一路到 `FileSource`，`FileSource` 用 `toWideString()` + `_wfopen()`（`sdk/extractor/FileSource.cpp:40-42`）打开——宽字符路径，绕开 ANSI。
- **`toNativeString()`（UTF-8 → ANSI/`CP_ACP`，有损）在生产链上零调用**：全仓 grep（限定 `app/`、`sdk/`、`doc/`，排除 `sdk/3rdparty`）只有定义（`sdk/utils/Utf8Path.h:54`）、内部实现里对 `toWideString` 的调用（`:60`）与文档引用（`AGENTS.md:135`），**没有任何生产调用点**。
- 因此：**直接透出 `fullPath` 不会把 ANSI 有损转换带进公开契约。** 公开面的风险不是编码，而是 §3 的形态不唯一。

**事实（`toWideString` / `toNativeString` 分工，`sdk/utils/Utf8Path.h` 原文注释）**
- `toWideString`（`:34`）：UTF-8 → Windows 宽字符，注释写明「打开文件应该走这条路径：宽字符与系统代码页无关，中文 / 日文 / emoji 之类的名字在任何语言的 Windows 上都能命中（见 FileSource 的 `_wfopen`）」。
- `toNativeString`（`:54`）：UTF-8 → ANSI(`CP_ACP`)，注释写明「**有损** …… 映射不上的字符会被替换成 `'?'` …… **不要再用它打开文件**，改用 `toWideString` + `_wfopen`」。
- 仓库规则：`AGENTS.md:134-136`（「Windows 上非 ASCII 文件名必须按宽字符打开」）。

**事实（运行时佐证）**：本机历史日志里 `fullPath` 含中文且按 UTF-8 落盘，例如 `D:/workspace/SoundBridge/music\俱乐部音乐循环-Freesound.aiff`（`sdk/test/log/unittest_10068_20261111_222100_00000.log:2819`）。该文件是本地未跟踪的测试产物，不是版本库内容。

### 2.3 问 2：网络曲目形态

**事实（`fullPath` 存的是原始输入串）**
- `fullPath` 在 `_addMusic` 第一段就赋值（`:256`），而 URL 分支（`:271-322`）在其后。`UrlSource` / `HttpSource` / `FileSource` 都不会回写 `fullPath`。
- `HttpSource` 的 `m_name` 保存构造入参 URL（`sdk/extractor/HttpSource.cpp:602`），但 `m_name` 只服务 `toString()`，**不进入播放列表**。
- 302 跟随只改内部成员：`HttpSource::probe()` 里 `m_host = host; m_port = port; m_path = path;`（`sdk/extractor/HttpSource.cpp:714-716`），`m_name` 不变 → **嗅探后的重定向 URL 不进 `fullPath`**。
- `UrlSource` 存 `m_url` 供 `getUri()`（`sdk/extractor/UrlSource.h:26`）；`HlsExtractor` 在 open 时取 `m_source->getUri()` 存进自己的 `m_url`（`sdk/extractor/hls/HlsExtractor.cpp:82-88`）再喂 `avformat_open_input`。这条链也**不回写** `fullPath`。
- 结论：网络曲目的 `fullPath` == **调用方传进 `addMusic` / `addMusicWithNotify` 的那一个字符串**（原样，含 query / fragment / 端口写法 / 大小写）。

**事实（两条入口的同一性问题）**
- 「目录扫描」**永远不会产出 URL**：`recursiveFileSearch()` 只做 `fs::recursive_directory_iterator` + 扩展名过滤（`sdk/extractor/FileSearch.cpp:23-32`）。所以「同一 URL 经目录扫描与直接 addMusicWithNotify」这个组合在现状下不存在。
- 更关键：**公开 `Player` 层没有 URL 入口**。`sdk/include/soundbridge/player.h:45-81` 只有 `addMusicDirectory(const std::string &)`（`:54`），没有 `addMusic` / `addUrl`。URL 只能经 `MusicPlayList::addMusic/addMusicWithNotify`（`sdk/MusicPlayList.h:138-139`）进入，其生产调用者只有 `MusicPlayer::Impl::addMusicDir()` 里的目录扫描（`sdk/MusicPlayer.cpp:200`），以及测试（`sdk/test/unit/test_http_source.cpp:425`、`test_live_source.cpp:70`、`LiveSmoke.cpp:102`）。
- 所以：**现状下「公开契约里不会出现 URL 形态的 id」**。`id` 的 URL 语义是为 `MusicPlayList` 这一层及未来公开入口预留的；#240 若只按「本地文件」设计，不会立刻踩到 URL 分支。

### 2.4 问 4：消费者影响

**事实**
- 生产消费者只有一个：`app/playercontroller.cpp:207-215`（`onPlaylistChanged` → `QStringList`），它**只读 `name`**（`:211`），并且通过 `publishViewState()` 把 `QStringList` 塞进 `PlayerViewState.playlist`（`app/playercontroller.h:15`）。
- 转发链上还有一次结构转换：`sdk/player_impl.cpp:70-92` 把 `std::list<MusicIndex>` 拷成 `std::list<TrackInfo>`，并另存一份 `m_trackNames` 供 `currentTrackName()` 使用（`:82-88`、`:115-119`）。
- 测试侧所有 `TrackInfo` / `MusicIndex` 触点：`sdk/test/mocks/PlayerMocks.hpp:140`、`:253`；`sdk/test/TestSdkSuite.cpp:54`、`:102`；`sdk/test/TestMusicPlayer.cpp:50`；`sdk/test/unit/test_bad_input.cpp:199`。全部是「存 `name` / 空实现」，**没有一处做聚合初始化或结构体尺寸断言**。
- 遗留 `MusicPlayer`：`sdk/MusicPlayer.h` 的 `MusicIndex` 是内层结构，`MusicPlayer::Impl::_addMusic`（`sdk/MusicPlayer.cpp:356`）是空函数、不是调用方。**`MusicPlayer` 本体不需要为 id 做任何同步**；只有 `MusicIndex` 需要加字段（否则 id 出不到 `player_impl.cpp`）。

**推断（编译影响）**：新增 `std::string id` 对上述所有位置都是**源码兼容**的，唯一会「需要改」的地方是 `player_impl.cpp`（不补赋值则 id 恒空）与 `MusicPlayer.cpp:868-871`（不补则内层就没这个值）。

### 2.5 问 5：全量下发成本

**事实（频率）**
- `onPlaylistChanged` 只在 `MusicPlayList::_updateList()` 里发一次（`sdk/MusicPlayList.cpp:404-408`），而 `_updateList()` 只在「本批 pendingAdd 归零」时被调一次（`sdk/MusicPlayer.cpp:204-208`），或扫描结果为空时一次（`:196`）。**一次目录扫描 = 一次全量下发**，与播放进度无关（进度走 `onPositionChanged`）。

**推断（量级，按千曲估算）**
- 现状每次下发已经深拷贝三遍：`vector<MusicPropertiesPtr>`（只拷指针）→ `list<MusicIndex>`（按值，`sdk/MusicPlayer.h:43`）→ `list<TrackInfo>`（`sdk/player_impl.cpp:72-79`）→ `QStringList`（`app/playercontroller.cpp:210-212`）。
- 加 `std::string id` 后，后三跳各多一份路径深拷贝。路径长度普遍 > 15 字节，**必然绕过 `std::string` 的 SSO**，每条一次堆分配：
  - 1000 曲 × 3 跳 = **约 3000 次 malloc/free**；
  - 每条约 80~120 B（路径）+ 16~32 B 分配头，净增 **约 0.1~0.15 MB** 瞬时内存；
  - 量级判断：亚毫秒到个位数毫秒（单次扫描完成时），相对 `addMusicDir` 本身的逐文件 `open + probe` 开销可忽略。
- 结论：**不需要 `shared_ptr` / 常驻 id 表 / 索引化下发**。若日后要优化，优先级更高的是把 `onMusicPlayerMusicListChanged` 的按值传参改成 `const &`（`sdk/MusicPlayer.h:43`），那是既有浪费，与本票无关。

---

## 归一化建议

### 3.1 仓库内先例：**无**

**事实**
- 限定 `sdk/utils`、`sdk/extractor`、`sdk/*.cpp` 的 grep（`normalize|lexically_normal|weakly_canonical|canonical|absolute\(`）只命中三类**非路径**归一化：
  - `ExtractorFactory::normalizeExtension()`（`sdk/extractor/ExtractorFactory.cpp:29`）——扩展名字符串，只管大小写/点号；
  - `normalizeAudioSpec()`（`sdk/extractor/ExtractorHelper.hpp:224`，各 extractor 调用）——音频参数；
  - `seek_absolute()`（`sdk/audio/decode/FLACDecode.cpp:152`）——采样定位。
- **`lexically_normal` / `weakly_canonical` / `boost::filesystem::canonical` / `boost::filesystem::absolute` 在 first-party 源码里零引用**（`sdk/3rdparty` 除外）。本票若做归一化，是仓库**第一处**路径归一化，需要新增 `sdk/utils` 单测。

**事实（可用 API）**：随仓 boost 1.81 已具备所需原语——`path::lexically_normal()`（`sdk/3rdparty/boost_1_81_0/boost/filesystem/path.hpp:1003`）、`absolute()`（`operations.hpp:93`）、`canonical()`（`:103`）、`weakly_canonical()`（`:164`）。无需新依赖。

### 3.2 做在哪一层

推荐：**`sdk/utils` 新增一个纯函数 `makeTrackId(const std::string &rawPath)`（或 `TrackId::fromPath`），唯一调用点放在 `sdk/MusicPlayList.cpp:256` 紧邻 `fullPath` 赋值处。**

理由（都是本票查到的事实）：
1. `_addMusic()` 是**唯一** choke point：目录扫描（`MusicPlayer.cpp:200`）、`addMusic` 直调（`MusicPlayList.cpp:42`）、URL 直通（同函数内 `:271-322`）全部经过。
2. 放在 `FileSearch` 需要改 `recursiveFileSearch` 的返回类型（现在是 `vector<string>`，`FileSearch.h:6-9`），会波及 `sdk/test/TestFileSearch.cpp` 与该函数的所有调用方；放在 `_addMusic` 零签名变更。
3. 放在 app 层不成立：app 只拿到 `name`，拿不到路径；且 app 不该承担 SDK 的身份口径。
4. 放在 `MusicPlayer::updateMusicList()`（`MusicPlayer.cpp:860-874`）也可行，但那时已经是「下发前最后一次机会」，且要重新从 `fullPath` 解析——不如在入列时算一次、与 `fullPath` 同生命周期。

### 3.3 做什么 / 不做什么

| 维度 | 建议 | 依据 |
|---|---|---|
| 绝对化 | **做**。`boost::filesystem::absolute()`（相对根 → 绝对） | 实证：相对根会原样留在路径里（`../../music\48000_fltp_1.aac`），换个 CWD 启动就是另一个 id |
| `.` / `..` | **做**，`lexically_normal()` 折叠 | 实证：`..` 已经进入过 `fullPath`（同上日志）；app 侧还会主动拼 `..`（`app/mainwindow.cpp:605`） |
| 分隔符 | **做**，Windows 上统一成 `/`（先 `lexically_normal` 再替换 preferred separator） | 实证：同一进程内既有 `.../music/music.wav` 又有 `.../music\music.wav` |
| 大小写 | **Windows 做，Linux 不做**。折叠时用宽字符（`LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE)` 或 `towlower`），**不要对 UTF-8 字节做 `tolower`** | Windows 文件系统不区分大小写是平台事实（仓库内无代码证据，属外部常识）；UTF-8 逐字节小写会破坏非 ASCII 路径 |
| 符号链接 | **不做**（不解析）。若 #240 要「同一文件经软链去重」，才改用 `canonical()`，代价是 id 随链接变化、且需要文件存在 | `canonical()` 会解析并需要 IO；`weakly_canonical()` 亦对存在前缀做解析 |
| 末尾斜杠 | **做**（`lexically_normal` 已顺带处理） | 未实测「带斜杠的根是否产出相同子路径」，见 §5 |
| URL | **不做任何归一化**，原样保留 | query / token 区分大小写，任何「规范化」都可能把两个不同资源并成一个，或把同一资源拆成两个 |
| `fullPath` 本身 | **不改值**，只新增派生 `id` | `fullPath` 被打开文件（`FileSource.cpp:40`）、错误回调（`MusicPlayer.cpp:386/394/409/784/790`）、日志与 metrics（`MusicPlayList.cpp:562/585`）消费；改它会扩大回归面，且换不来收益 |

### 3.4 已知的「归一化也救不了」的情形（必须在契约里写明）

- **跨机 / 跨盘移动**：id 变。收藏条目会成为悬挂。
- **同一 URL 换 query（token 刷新）**：id 变。
- **同一文件以硬链接存在两处**：id 不同（这是正确的，除非 #240 想要"同 inode 合并"）。
- **`id` 在列表内不唯一**：`addMusic` 无去重（`MusicPlayList.cpp:362`）。

---

## 证据

### 4.1 产生点与编码

- `sdk/MusicPlayList.h:26` — `    std::string fullPath;`
- `sdk/MusicPlayList.cpp:256` — `    fileProperties.fullPath              = musicPath;`（`_addMusic()` 内、构造任何 `DataSource` 之前）
- `sdk/extractor/FileSearch.cpp:29` — `            fileVec.push_back(sdk_utils::toUtf8String(iter->path()));`
- `sdk/utils/Utf8Path.h:16` — `    std::wstring ws = p.wstring();`
- `sdk/utils/Utf8Path.h:19` — `    int size = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, nullptr, 0, nullptr, nullptr);`
- `sdk/extractor/FileSource.cpp:40` — `        const std::wstring wide = sdk_utils::toWideString(filename);`
- `sdk/extractor/FileSource.cpp:42` — `            m_fp = _wfopen(wide.c_str(), L"rb");`
- `sdk/utils/Utf8Path.h:64` — `    int asize = WideCharToMultiByte(CP_ACP, 0, ws.c_str(), -1, nullptr, 0, nullptr, nullptr);`（`toNativeString`，**无生产调用者**）
- `AGENTS.md:135` — 「+ sdk_utils::toWideString；toNativeString 是 UTF-8 → ANSI(CP_ACP) 的有损转换，」（原文在反引号内，此处去掉内嵌反引号以便引用）

### 4.2 网络曲目形态

- `sdk/extractor/HttpSource.cpp:602` — `    m_name = url ? std::string(url) : std::string("<null>");`（只服务 `toString()`）
- `sdk/extractor/HttpSource.cpp:714-716` — `            m_host = host;` / `            m_port = port;` / `            m_path = path;`（302 只改内部成员）
- `sdk/extractor/UrlSource.h:26` — `    virtual std::string getUri() { return m_url; }`
- `sdk/extractor/hls/HlsExtractor.cpp:88` — `    m_url = uri;`（取自 `m_source->getUri()`，`:82`）
- `sdk/include/soundbridge/player.h:54` — `    void addMusicDirectory(const std::string &path);`（公开层唯一的入列入口，无 URL 入口）

### 4.3 形态不唯一（问 6 的实证）

**证据 A（主证据，同一进程内、同一物理文件、两种写法）** —— 本机历史测试日志 `sdk/test/log/unittest_10068_20261111_222100_00000.log`（未跟踪的本地产物）：

- `:2473` — `addMusicWithNotify : D:/workspace/SoundBridge/music/music.wav`（直接 `addMusic`，正斜杠）
- `:2815` — `addMusicWithNotify : D:/workspace/SoundBridge/music\music.wav`（同一目录经 `addMusicDir` 扫描，反斜杠）

两条相差 0.58 s、同一进程（PID 列同为 `0x00008e44`）。这就是「同一物理文件 → 两个不同 id」的最小复现：**根字符串的写法 + boost 追加子项时的 preferred separator**，与文件名无关。

**证据 B（分隔符混用 + 非 ASCII + 无去重）** —— `sdk/test/log/unittest_10228_20261109_090154_00000.log`：

- `:147` — `addMusicDir : C:\Users\25746\AppData\Local\Temp/sb_badinput_10228_batch_dir3`（根自身就已反斜杠与正斜杠混用）
- `:150` — `addMusicWithNotify : C:\Users\25746\AppData\Local\Temp/sb_badinput_10228_batch_dir3\a_good.wav`

同一日志里 `music.wav` 在 `:3734/3755/3777/…/3930` 连续出现十余次，说明**入列无去重**（对应 `MusicPlayList.cpp:362` 的裸 `push_back`）。

**证据 C（相对路径与 `..` 原样入库）** —— `sdk/test/log/sdk_test_suite_10100_20261117_235742_00000.log:212`：

- `addMusicWithNotify : ../../music\48000_fltp_1.aac`

**证据 D（代码层解释，仓库内成文）**——

- `sdk/test/unit/test_player_integration.cpp:1006-1007` — `// recursiveFileSearch 在 Windows 上回的是反斜杠路径，而 FileProperties 只按 '/'` / `// 切目录，所以 MusicIndex.name 会带上目录前缀；按后缀认文件最稳。`
- `doc/app-ui-spec.md:132` — 「现状形如 music\48000_fltp_1，属预期，不是缺陷。」（同一现象在 UI 规格里被当成已知现状接受）
- `sdk/MusicPlayList.h:36` — `        size_t lastSlashPos = fullPath.find_last_of("/");`（只认 `/`，是 name 带目录前缀的根因）

**证据 E（app 主动制造 `..`）**——

- `app/mainwindow.cpp:605` — `    QDir dir(QCoreApplication::applicationDirPath() + "/../../music");`
- `app/mainwindow.cpp:608` — `        mController->addMusicDirectory(dirbsolutePath.absolutePath().toUtf8().constData());`

**推断**：若 `QDir::absolutePath()` 原样返回已绝对的输入（Qt 文档语义是「可能含 `.` / `..`，需要 clean 请用 `canonicalPath()`」），则 app 生产路径里的 `..` 会一直落到 `fullPath`。**本票未能取到该 Qt 5.14 文档原文（`doc.qt.io` 返回 403，本机无 Qt 源码）**，故列入 §5 未决；但无论清洗与否，结论不变——归一化必须做。

### 4.4 归一化先例（无）

- `sdk/extractor/ExtractorFactory.cpp:29` — `std::string normalizeExtension(const std::string &extensionName)`（只处理扩展名）
- `sdk/extractor/ExtractorHelper.hpp:224` — `    static void normalizeAudioSpec(AudioSpec &spec)`（只处理音频参数）
- 限定路径 grep 结果：`lexically_normal` / `weakly_canonical` / `boost::filesystem::canonical` **在 first-party 源码中零命中**
- 可用原语：`sdk/3rdparty/boost_1_81_0/boost/filesystem/path.hpp:1003` — `    BOOST_FORCEINLINE path lexically_normal() const { return BOOST_FILESYSTEM_VERSIONED_SYM(lexically_normal)(); }`

### 4.5 消费者与成本

- `sdk/include/soundbridge/player.h:39` — `    virtual void onPlaylistChanged(const std::list<TrackInfo> &tracks) = 0;`
- `app/playercontroller.cpp:211` — `        names.push_back(QString::fromUtf8(track.name.c_str()));`
- `sdk/player_impl.cpp:76-77` — `                info.index = item.index;` / `                info.name  = item.name;`
- `sdk/MusicPlayer.cpp:870` — `        index.name  = property->fileProperties.fileName;`
- `sdk/MusicPlayer.h:43` — `    virtual void onMusicPlayerMusicListChanged(std::list<MusicIndex>) = 0;`（按值传）
- `sdk/MusicPlayer.cpp:204-208` — 最后一条 pending 完成时才 `reportScanSummary()` + `updateList()`（一次扫描一次下发）
- `sdk/MusicPlayList.cpp:404-408` — `_updateList()` 唯一实现：`m_callback->updateMusicList(m_musicListProperties);`
- `sdk/include/soundbridge/version.h:8` — `#define SOUNDBRIDGE_ABI_VERSION    1`
- `doc/player-state-contract.md:64` — `` `onPlaylistChanged` 先于由该次扫描引发的 `onTrackChanged` ``（这条既有契约让 app 可以「先拿快照、再按 index 查 id」）

---

## 未决与风险

1. **`QDir::absolutePath()` 是否清洗 `..`（未核实）**。已查范围：本机 `/d/Qt/5.14.2` 只装二进制与头文件、无 `qtbase` 源码；`doc.qt.io` 直连 403。影响：只影响「app 生产路径里是否真的有 `..`」，不影响「必须归一化」的结论。低成本验证：在 `MainWindow::scanSongs()` 里临时打印 `dirbsolutePath.absolutePath()`（本票未做，因不许改文件）。
2. **大小写折叠的实现细节未验证**。Windows 上对非 ASCII 做 `towlower` 的结果依赖 locale / CRT；建议 `LCMapStringEx` + `LOCALE_NAME_INVARIANT`。本票无实测，且**没有任何仓库内用例能覆盖**（Windows 大小写不敏感是平台行为，单测只能测「同一字符串折叠后相同」，测不出"两个真实文件是同一个"）。
3. **符号链接策略未定**。不解析 → 软链与目标算两个 id；解析（`canonical`）→ 需要文件存在、且链接改名会掉收藏。这是产品语义问题，建议交 #240 拍板，本票只给两个选项的代价。
4. **末尾斜杠 / 重复斜杠 / `.` 段的行为未实测**。`lexically_normal()` 应该能覆盖，但随仓 boost 默认 `BOOST_FILESYSTEM_VERSION` 为 4，v3 与 v4 在「末尾 `.`」上有差异；需要在写实现时补一条单测锁死。
5. **Linux 侧形态未实测**。本机只有 Windows（MinGW730）。按 `Utf8Path.h:26` 的 `#else return p.string()` 可推知 Linux 上分隔符天然为 `/`、无大小写折叠需求，但**没有跑过 CI 日志核对**。
6. **网络 URL 作为身份键的稳定性只有一个前提**：用户每次粘贴同一字符串。token / 时效参数一变 id 就变，这不是归一化能修的（剥 query 会破坏资源区分）。若 #240 的目标包含网络曲目，需要单独定义「URL 身份」策略。
7. **id 在列表内可能重复**（`addMusic` 无去重，`MusicPlayList.cpp:362`）。收藏若以 id 为键，同一文件被加两次会在 UI 上表现为「收藏了一个、另一个没收藏」。建议 #240 明确「以 id 为键，天然去重」还是「SDK 侧加 dedup」。
8. **本票未构建、未运行任何测试**；结论基于源码静态阅读 + 历史日志（日志来自 2026-11-09 / 11-11 / 11-17 的本机运行，非本次复现）。

---

## 对下游票的输入（#240）

### 6.1 字段形态

```cpp
// sdk/include/soundbridge/player.h
struct TrackInfo {
    int index;
    std::string name;   // 显示名，不变
    std::string id;     // 新增：跨扫描稳定的不透明身份键
};
```

- **位置**：追加在**末尾**。既有两字段偏移不变，误用风险最小；`#72` 的 `index` / `name` 语义一行不改。
- **类型**：`std::string`（UTF-8）。**不要** `uint64_t` 哈希——哈希会丢碰撞语义、且需要写盘持久化后仍可复算；`std::string` 直接把口径暴露给 #240 的存储层（`QSettings` 字符串列表可直接存）。
- **可空性**：**契约上不空**。理由（本票事实）：只有成功入列的曲目才有 `TrackInfo`；`https://`、`.mpd`、伪清单、ICY 在 `_addMusic` 就 `return`（`MusicPlayList.cpp:285-313/318-319`），不会进列表。但**实现上允许空**（老 `MusicIndex` 调用方 / 未来流式条目），消费方须按「空 = 无身份，不可收藏」处理，不要 assert。
- **语义边界**：`id` **不承诺可直接用于打开文件**。打开仍走 `fullPath`（`FileSource` + `_wfopen`）。这条要写进注释，防止将来有调用方拿 id 去 `fopen`。

### 6.2 兼容策略

- **ABI**：`SOUNDBRIDGE_ABI_VERSION` 1 → 2（`sdk/include/soundbridge/version.h:8`）。`TrackInfo` 尺寸 40 → 72，已编译的第三方调用方必须重编。仓库内所有消费方同仓编译，无实际影响，但版本号要诚实。
- **源码**：兼容。依据：全仓无 `TrackInfo` 聚合初始化（grep `TrackInfo\s*[={]` / `TrackInfo\{` 只命中定义本身），唯一构造点是 `sdk/player_impl.cpp:74-79` 的逐字段赋值。
- **`onPlaylistChanged` 不版本化**：`std::list<TrackInfo>` 的成员增加不改签名，无需第二套回调。`doc/player-state-contract.md:64` 的顺序契约（playlist 先于 track）不动。
- **内层同步**：`sdk/MusicPlayer.h:31-34` 的 `MusicIndex` 必须同步加 `std::string id`，否则 `sdk/MusicPlayer.cpp:868-871` 无值可传。`MusicIndex` 是 `sdk::` 内层结构（公开头不导出），加字段不构成对外 ABI 变更。
- **遗留 `MusicPlayer`**：不需要改 `MusicPlayer.h` 的公开方法；`MusicPlayer::Impl::_addMusic`（`sdk/MusicPlayer.cpp:356`）是空实现，别误当调用方补逻辑。
- **app**：`PlayerViewState` 若要带 id，建议加**并行** `QStringList trackIds`（与 `playlist` 同序），而不是把 `QStringList` 换成结构体列表——后者会波及 `mainwindow.cpp` 里所有按索引取 `playlist.at(i)` 的地方（`playercontroller.cpp:238` 等）。

### 6.3 收藏归属（本票只给事实与代价，不拍板）

- **app 持有（推荐）**：`id` 只是字符串，SDK 侧零状态、零新回调；`§1.1 三层真相源表` 只需加一行「收藏 = app 本地状态」。风险：多实例 / 多进程并发写要 app 自己处理。
- **SDK 持有**：需要新回调（`onFavoriteChanged`）+ 存储位置（SDK 不知道 `QSettings`），等于给 SDK 引入持久化面；且 `ADR 0004` 的「状态面边界」要重审。本票查到 SDK 目前**没有任何持久化设施**（无 `QSettings`、无配置文件读写），这条路成本明显更高。
- **#240 需要的额外决策**（本票无法回答）：文件被删除/移动后悬挂条目保留多久；`id` 空（未来流式条目）时的按钮禁用语义。

### 6.4 给 #240 的一条硬约束

**不要用 `name` 或 `index` 当键。** 依据：
- `index` 是扫描顺序（`MusicPlayList.cpp:252` `musicProperties->index = m_curIndex;`），换目录、加文件就会变；
- `name` 是去扩展名的 `fileName`，在 Windows 上还会带目录前缀（`doc/app-ui-spec.md:132`），既不唯一也不稳定。
