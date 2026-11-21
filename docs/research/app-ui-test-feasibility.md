# app 层 UI 自动化测试可行性：本机探针 + 工具链核对（票 #224）

> 本文件是**时点快照**（2026-11-21，`master @ b0543e3ce`），不是规格承诺。
> 只读调查：未改生产代码、未改 CI 配置、未新增 ctest 目标；探针源码与产物均在仓库外临时目录。
> 目的：回答「在现有 Qt 5.14.2 / MinGW 7.3.0 / qtbase-only CI 下，引入 QtTest + offscreen 的 UI 自动化值不值得」。

## 0. 环境与可用性

| 项 | 事实 | 证据 |
|---|---|---|
| 本地 Qt | 5.14.2，MinGW 7.3.0 全量安装（非 qtbase-only） | `D:/Qt/5.14.2/mingw73_64`、`D:/Qt/Tools/mingw730_64` |
| `Qt5Test` CMake 包 | 存在 | `D:/Qt/5.14.2/mingw73_64/lib/cmake/Qt5Test/Qt5TestConfig.cmake` |
| `Qt5Test` 运行库 | 存在 | `D:/Qt/5.14.2/mingw73_64/bin/Qt5Test.dll` |
| offscreen 平台插件 | 存在 | `D:/Qt/5.14.2/mingw73_64/plugins/platforms/qoffscreen.dll` |
| Linux CI 的 QtTest | `qtbase5-dev` 提供 `QtTest` 头与 `Qt5TestConfig.cmake`；运行时库 `libqt5test5` 是否随依赖安装**未在 CI 实测** | [packages.ubuntu.com noble/amd64/qtbase5-dev/filelist](https://packages.ubuntu.com/noble/amd64/qtbase5-dev/filelist) |
| Windows CI 的 QtTest | `scripts/setup_windows_env.ps1` 用 aqt `--archives qtbase`；**qtbase archive 是否包含 `Qt5Test.dll` 未实测**（本机是全量安装，不能作证） | `.github/workflows/sdk-windows.yml`、`scripts/setup_windows_env.ps1:141-142` |

结论（可用性）：本地闭环成立；CI 侧两个平台都还差一次实测确认。

## 1. 探针：QtTest + offscreen 能否真的跑起来

探针位于仓库外 `%TEMP%/qt_ui_probe`（两个文件，约 45 行）：

- `CMakeLists.txt`：`find_package(Qt5 COMPONENTS Widgets Test REQUIRED)`，`CMAKE_AUTOMOC ON`，链接 `Qt5::Widgets Qt5::Test`。
- `test_main.cpp`：两个用例——① `QWidget` + `QLabel` `show()` 后 `QTest::qWaitForWindowExposed(&w, 3000)`，断言可见、`findChild<QLabel*>("probeLabel")` 命中；② 10 ms `QTimer` 在事件循环里至少推进 3 次（`QTRY_VERIFY_WITH_TIMEOUT`）。

构建与运行（Git Bash，PATH 前置 MinGW 与 Qt）：

```bash
cmake -S . -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_PREFIX_PATH="D:/Qt/5.14.2/mingw73_64" \
  -DCMAKE_C_COMPILER="D:/Qt/Tools/mingw730_64/bin/gcc.exe" \
  -DCMAKE_CXX_COMPILER="D:/Qt/Tools/mingw730_64/bin/g++.exe" \
  -DCMAKE_MAKE_PROGRAM="D:/Qt/Tools/mingw730_64/bin/mingw32-make.exe"
cmake --build build
QT_QPA_PLATFORM=offscreen ./build/probe.exe -o result.txt,txt
```

原始结果（`result.txt`）：

```text
********* Start testing of UiProbe *********
Config: Using QtTest library 5.14.2, Qt 5.14.2 (x86_64-little_endian-llp64 shared (dynamic) release build; by GCC 7.3.0)
PASS   : UiProbe::initTestCase()
QWARN  : UiProbe::widgetBecomesVisibleOffscreen() QFontDatabase: Cannot find font directory D:/Qt/5.14.2/mingw73_64/lib/fonts.
PASS   : UiProbe::widgetBecomesVisibleOffscreen()
PASS   : UiProbe::timerAndEventLoopAdvance()
PASS   : UiProbe::cleanupTestCase()
Totals: 4 passed, 0 failed, 0 skipped, 0 blacklisted, 72ms
********* Finished testing of UiProbe *********
```

三个操作性坑（都实测过）：

1. **测试日志不会出现在终端/管道里**：`./probe.exe` 与 `./probe.exe 2>&1 | tail` 都是空输出、exit 0；`--help` 有正常输出。必须 `-o result.txt,txt`（或 `-o -,txt`）落文件再读。CI 里若直接 `ctest`，脚本/日志步骤要按这个事实写。
2. **offscreen 下没有字体目录**：`QFontDatabase` 报 `Cannot find font directory .../lib/fonts`。app 侧有 `:/fonts/NotoSansCJK-Regular.ttc` 兜底（`app/main.cpp:83-95`），但测试二进制若不走 `initCJKFont()`，文本度量会退化；断言不要依赖像素宽度。
3. **探针不含音频**：`SDL_AUDIODRIVER=dummy` 未在本探针验证；任何经 `soundbridge::Player` 的用例要另行确认 dummy 设备可用。

## 2. 可测性改造：卡点不在链接

现状（`app/CMakeLists.txt:15-38`）：`main.cpp` / `mainwindow.*` / `playercontroller.*` 直接进 `add_executable(SoundBridge WIN32 ...)`，链接 `Qt5::Widgets` 与 `sdk`。没有可链接的 app 库。

两条路线：

| 路线 | 改动 | 代价 | 风险 |
|---|---|---|---|
| A. 把 app 源直接编进测试目标 | 测试目标重复列 `mainwindow.cpp` / `playercontroller.cpp` / `res.qrc`，复制 `AUTOMOC` / `AUTORCC` 设置 | 源码编两遍；目标配置重复 | 低 |
| B. 抽 `app_core` STATIC 库 | `app/CMakeLists.txt` 拆成 `app_core`（界面 + 控制器 + qrc）与 `SoundBridge`（`main.cpp` + `app_core`），约 20 行 | 一次 CMake 重构；`WIN32` 子系统只留在 exe | 低；需双端 CI 验证 |

**真正的卡点是状态注入缝，不是构建。** `MainWindow` 私有持有 `std::unique_ptr<PlayerController>`（`app/mainwindow.h:52`），`PlayerController` 私有持有 `std::unique_ptr<soundbridge::Player>`，且构造时立即创建 Player。想断言「给定 `PlayerViewState` → 界面显示什么」，只有三条路：

1. 给 `MainWindow` 加只读渲染入口（如 `renderForTest(const PlayerViewState &)`）或测试构造参数——一次 app 层公开面改动；
2. 用真实播放驱动状态（合成素材 + SDL dummy + 等待回调）——慢、受音频链路影响，且 `scanSongs()` 在测试进程里的 `../../music` 路径大概率不存在；
3. 只测 `PlayerController` 的纯函数（`formatTime` / `errorReason` / `errorSubtitle`），不测控件——覆盖不到本规格的大部分判据。

## 3. 成本估算

| 项 | 量级 | 备注 |
|---|---|---|
| 新增目标 | +1 个 exe、+1 个 ctest | 违反现有「保持 14 个 ctest 目标」纪律，需要一次显式例外 |
| 本地构建 | 探针 813 KB、2 用例，构建数秒 | app 源进目标后预计 +5~15 s |
| CI 增量 | 两平台各 +1 构建目标，预计 +30~90 s | 未计入首次 QtTest 依赖确认成本 |
| CI 环境 | `QT_QPA_PLATFORM=offscreen`；播放类用例还需 `SDL_AUDIODRIVER=dummy` | `add_test` 的 `ENVIRONMENT` 可设 |
| 覆盖率 | app 代码不在现有覆盖率统计内；若测试目标引入 app 源，是否改变分母**未验证** | 需核对 `ENABLE_COVERAGE` 的 target 范围 |

## 4. 结论与建议

- **QtTest + offscreen 本身可用**（本机 4 用例 / 72 ms 全过），不是框架能力问题。
- **首版不引入**：阻塞项是「状态注入缝」这次 app 层 API 改动 + 新增 ctest 目标的纪律例外，成本高于规格本身；引入时机应是「界面行为开始频繁回归」或「执行票需要自动判据」。
- 若将来引入，前置三件事：① 给 `MainWindow` 一个不暴露 `Player` 的只读渲染入口；② 在 CI 上实测 Windows `qtbase` 含 `Qt5Test.dll`、Linux 装齐 `libqt5test5`；③ 明确新目标对 14 目标 / `TIMEOUT` 纪律的例外与预算。
- 建议把「UI 状态注入缝」单独立图或立票，**不要塞进本规格**——那是一次 app 层架构决策，不是文档补充。
