# APP KNOWLEDGE BASE

## OVERVIEW
`app/` 是 Qt Widgets 壳层：负责窗口、按钮、列表、设置持久化，以及把 SDK 状态翻译成界面状态。

## WHERE TO LOOK
| 任务 | 位置 | 备注 |
|------|------|------|
| 启动与字体 | `main.cpp` | 创建 `QApplication`，处理 CJK 字体降级 |
| 主界面布局/交互 | `mainwindow.cpp`, `mainwindow.h` | 按钮槽、列表刷新、错误提示、QSettings |
| SDK 适配层 | `playercontroller.cpp`, `playercontroller.h` | `soundbridge::PlayerCallbacks` 汇总成 `PlayerViewState` |
| 浮层（音量 / 播放列表面板） | `popuppanel.cpp`, `popuppanel.h` | `PopupPanel` 自绘 `Qt::Popup` 底座，`VolumePopup` / `PlaylistPopup` 两个子类；选型与删除 `mListMask` 的理由见 `docs/adr/0015-popup-base-and-list-mask-removal.md` |
| 资源样式 | `res.qrc`, `style.qss`, `images/`, `fonts/` | UI 资源入口 |

## CONVENTIONS
- UI 代码不要直接摸底层 audio/extractor；统一经 `PlayerController` 调 `soundbridge::Player`。
- `PlayerController` 负责把 `std::string` 转成 `QString`，以及把播放状态整理成 `PlayerViewState`；不要把这些转换散回 `MainWindow`。
- `MainWindow` 里已有大量旧式 `connect(... SIGNAL ... SLOT ...)`；在这个目录内改动时先跟随现状，不混入另一套信号风格造成半新半旧。
- 浮层一律挂在 `PopupPanel` 底座上：`Qt::Popup` + 自绘，关闭时机（点外 / Esc）、焦点回触发按钮、上方空间不足翻转都在底座里，子类只管内容。`QMenu` / `QDialog` 已被否决，不要再新增第二套机制（ADR 0015）。
- 持久化项集中在 `QSettings` 的三个命名区：`window/*`（窗口几何与最大化）、`playback/*`（播放参数：模式、autoSkip、音量、静音）、`library/*`（曲库级本地状态：收藏集合）。键表以 `doc/app-ui-spec.md` §6.1 为准，新增键必须先登记。

## ANTI-PATTERNS
- 不要在 `MainWindow` 里直接维护播放状态真相源；以 `PlayerViewState` 为准。
- 不要把打包、DLL/so 路径、toolchain 细节写回 `app/README.md`；这些归根文档管理。
- 不要假设任意系统都有中文字体；`main.cpp` 已有字体回退逻辑，相关修改要保留跨平台兜底。

## LOCAL CHECKS
```bash
cmake --build build --target SoundBridge
ctest --test-dir build -R sdk_player_tests --output-on-failure
```

## NOTES
- `app/README.md` 只说明资源来源；构建/运行/打包说明以仓库根 `AGENTS.md` 和 `README.md` 为准。
- `MainWindow::scanSongs()` 目前按 `QCoreApplication::applicationDirPath() + "/../../music"` 扫描样本目录；改启动目录或打包路径时要同时顾及构建态与 portable 运行。
- 这个目录文件不多，但职责与 SDK 明显不同，适合单独挂本地说明。
