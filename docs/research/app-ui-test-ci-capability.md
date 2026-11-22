# CI 双端 Qt5Test 与 offscreen 能力取证（票 #249）

> 本文件是**时点快照**（2026-11-22，`master @ 943b675f9`），不是规格承诺。
> 只读调查：未改 `.github/workflows/**`、未改 `scripts/**`、未改生产代码；探针源码与解压产物都在仓库外
> （`%TEMP%\qtoffscreen`、`D:\tmp\qta`）。证据命令可直接复跑。
> 判定严格三分：**已实测**（本次亲手执行并留下输出）/ **元数据可证**（权威归档目录、发行版包清单、Qt 源码可推）/
> **未验证**。凡「已实测」发生在开发机而非 runner 的，行内显式标「本机」。

## 0. 结论速览

| 平台 | Qt5Test（链接 + 运行） | offscreen 平台插件 | 判定 |
|---|---|---|---|
| Windows（`windows-latest`，aqt `--archives qtbase`） | 归档含 `bin/Qt5Test.dll` 与 `lib/cmake/Qt5Test/Qt5TestConfig.cmake`；按同一归档解压出的纯 qtbase 目录链接、构建、6 用例全过 | 含 `plugins/platforms/qoffscreen.dll`，可加载运行 | **已实测**（本机，产物与 CI 同源） |
| Linux（`ubuntu-24.04`，`apt install qtbase5-dev`） | `qtbase5-dev` 声明依赖 `libqt5test5t64`；dev 包内含 `Qt5TestConfig.cmake`，运行包内含 `libQt5Test.so.5` | `platforms/libqoffscreen.so` 由 `libqt5gui5t64` 提供，而它是 `qtbase5-dev` 的依赖 | **元数据可证**（未在 runner 实跑） |

两个阻塞事实（双端 Qt5Test 可用性）都已落定。**但 Windows + offscreen 下 `QMessageBox` 必崩**（§3.2）——
这是判据设计的新约束，不是 Qt5Test 可用性问题。

## 1. Windows：aqt `--archives qtbase` 到底装了什么

取证对象是 aqt 对 `win64_mingw73 / 5.14.2` 的 `qtbase` 归档本体（`scripts/setup_windows_env.ps1:142` 走的就是这条）：

```bash
curl -sSL -o qtbase.7z \
  "https://download.qt.io/online/qtsdkrepository/windows_x86/desktop/qt5_5142/qt.qt5.5142.win64_mingw73/5.14.2-0-202003291224qtbase-Windows-Windows_10-Mingw73-Windows-Windows_10-X86_64.7z"
sha256sum qtbase.7z
# e5dd6df5105343f0d0e2c93e5dad8d12723ad21727c43ad41ba707de5757496a
# 与同目录 .sha256 一致（2026-11-22 拉取）
7z l qtbase.7z | grep -E "Qt5Test|qoffscreen|plugins.platforms"
```

| 项 | 事实 | 证据（URL 或命令） | 判定 |
|---|---|---|---|
| 归档完整性 | 32 667 517 字节，3351 文件 / 301 目录；sha256 与官方 `.sha256` 逐字相同 | `sha256sum qtbase.7z`；上游 `<...>.7z.sha256` | 已实测 |
| `bin/Qt5Test.dll` | 存在，541 520 字节 | `7z l` 输出 | 元数据可证 |
| `lib/cmake/Qt5Test/Qt5TestConfig.cmake` | 存在，8315 字节；同目录另有 `Qt5TestConfigExtras.cmake`、`Qt5TestConfigVersion.cmake` | 同上 | 元数据可证 |
| `plugins/platforms/qoffscreen.dll` | 存在，1 089 400 字节；同目录 `qwindows.dll` / `qminimal.dll` / `qdirect2d.dll` | 同上 | 元数据可证 |
| Debug 版 Qt DLL | **不存在**：`bin/` 下只有 11 个 release DLL（`Qt5Core/Gui/Widgets/Test/...`），无 `Qt5Cored.dll` / `Qt5Testd.dll` | `7z l qtbase.7z \| grep -E "bin.Qt5[^\\]*\.dll"` | 已实测 |
| Debug 配置的测试 exe | CI 用 `-DCMAKE_BUILD_TYPE=Debug`（`scripts/setup_windows_env.ps1:33` 默认值），但对 release-only 的 Qt 配置、链接均成功，exe 导入的是 release DLL | 见下方构建记录 + `objdump -p probe3.exe \| grep "DLL Name"` 输出 `Qt5Test.dll`（无 `Qt5Testd.dll`） | 已实测（本机） |
| 运行期 DLL 集 | 只需 PATH 两段：`<Qt>/bin`（`Qt5Core/Gui/Widgets/Test.dll`）与 `<Qt>/Tools/mingw730_64/bin`（`libgcc_s_seh-1.dll`、`libstdc++-6.dll`）；**不需要 `windeployqt`，不需要手拷 `platforms/`** | `objdump -p` 直接导入表：`Qt5Core/Qt5Gui/Qt5Test/Qt5Widgets/libgcc_s_seh-1/libstdc++-6/KERNEL32/msvcrt` | 已实测（本机） |
| 平台插件发现 | 解压到非标准前缀（`D:/tmp/qta/root/...`）且**不设** `QT_PLUGIN_PATH` 也能找到 `qoffscreen.dll`——Qt 5 在 Windows 上按 `Qt5Core.dll` 所在目录相对定位插件 | 同一 exe 在无 `QT_PLUGIN_PATH` 下 6 用例通过；`qoffscreen.dll` 直接依赖只有 `KERNEL32/msvcrt/libstdc++-6/Qt5Core/Qt5Gui` | 已实测（本机） |
| 现有 PATH 注入点 | `sdk/test/CMakeLists.txt:64-68` 已有 `SOUNDBRIDGE_QT_TEST_BIN_DIR` 钩子，命中时把 Qt bin 拼进 `add_test` 的 `PATH`；Windows workflow 另把 `<Qt>/5.14.2/mingw73_64/bin` 写进 `GITHUB_PATH` | `.github/workflows/sdk-windows.yml`（Export PATH 步骤） | 元数据可证 |

按归档原样解压后构建运行（Debug，与 CI 同配置）：

```bash
# 归档解压到 D:/tmp/qta/root，然后：
cmake -S probe3 -B probe3/build_ci -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_PREFIX_PATH="D:/tmp/qta/root/5.14.2/mingw73_64" ...
cmake --build probe3/build_ci
PATH="D:/tmp/qta/root/5.14.2/mingw73_64/bin;D:/Qt/Tools/mingw730_64/bin" \
  QT_QPA_PLATFORM=offscreen probe3.exe -o ci_ok.txt,txt
# Totals: 6 passed, 0 failed, 0 skipped, 0 blacklisted, 110ms
```

未覆盖：**runner 上的实跑**。本机结论只证明「同一份归档产物可链接、可运行」，不证明 `windows-latest` 的会话/权限下同样成立（见 §5）。

## 2. Linux：`qtbase5-dev` 的连带关系

| 项 | 事实 | 证据（URL 或命令） | 判定 |
|---|---|---|---|
| dev 包内含 QtTest 开发件 | `/usr/lib/x86_64-linux-gnu/cmake/Qt5Test/Qt5TestConfig.cmake`（另有 `ConfigExtras` / `ConfigVersion`）、`libQt5Test.so`、`libQt5Test.prl`、`pkgconfig/Qt5Test.pc` | <https://packages.ubuntu.com/noble/amd64/qtbase5-dev/filelist> | 元数据可证 |
| 运行库归属 | `libqt5test5t64` 提供 `libQt5Test.so.5` → `.5.15` → `.5.15.13` | <https://packages.ubuntu.com/noble/amd64/libqt5test5t64/filelist> | 元数据可证 |
| 运行库是否连带安装 | `qtbase5-dev` 的 `dep:` 段显式列出 `libqt5test5t64 (= 5.15.13+dfsg-1ubuntu1)`，`apt-get install qtbase5-dev` 会拉进来 | <https://packages.ubuntu.com/noble/qtbase5-dev> | 元数据可证 |
| offscreen 插件归属 | `libqt5gui5t64` 提供 `/usr/lib/x86_64-linux-gnu/qt5/plugins/platforms/libqoffscreen.so`（同目录 `libqminimal` / `libqxcb` / `libqlinuxfb` / `libqvnc` / `libqeglfs`）；`qtbase5-dev` 同样 `dep:` 到 `libqt5gui5t64` | <https://packages.ubuntu.com/noble/amd64/libqt5gui5t64/filelist>；`qtbase5-dev` 依赖页 | 元数据可证 |
| 包名形态 | 票正文写的 `libqt5test5` 在 noble 已不存在（`/noble/amd64/libqt5test5` 返回 “No such package”），noble 是 time_t 过渡后的 `*t64` 包名；`libqt5gui5` 同理 | 上述两个非 t64 路径返回错误页 | 已实测 |
| 版本 | noble 的 Qt 是 **5.15.13**，不是 Windows 侧的 5.14.2 | 依赖页 `(= 5.15.13+dfsg-1ubuntu1)` | 元数据可证 |

归属更正：`qoffscreen` 插件**不在** `qtbase5-dev` 里（dev 包只有头 `qoffscreensurface.h`），也不在 `libqt5test5t64`，而在 `libqt5gui5t64`——与票里的提醒一致。

未覆盖：Ubuntu 上的实际安装与运行（本机 Windows，无法代表 apt 闭包的实际落盘结果）。

## 3. offscreen 在 Windows 上的真实行为

### 3.1 能用：窗口、嵌套事件循环、`Qt::Popup`

探针（QtTest，两平台插件对照，Qt 5.14.2 / MinGW 7.3.0）：

| 项 | offscreen 观测 | windows 插件对照 | 判定 |
|---|---|---|---|
| 普通 `QWidget` + `show()` | `visible: true`，`QTest::qWaitForWindowExposed` 返回 true，`windowHandle()->isExposed()` true，`findChild<QLabel*>` 命中 | 同 | 已实测（本机） |
| `QDialog::exec()` 模态 | 进入嵌套事件循环（`QTimer` 在 80 ms 内触发），`findChild` 可定位按钮，`accept()` 后返回 1 | 同 | 已实测（本机） |
| `QMenu`（`Qt::Popup`） | `visible: true`、`isExposed` true、`QApplication::activePopupWidget() == &menu`；`QTest::mouseClick` 命中 action 并触发 `triggered` | 同 | 已实测（本机） |
| `QComboBox::showPopup()` | 视图 `visible: true` 但 `isExposed` **false**；`Key_Down` + `Key_Return` 可改 `currentIndex` | 视图 `visible: false`（无事件循环时未映射），键盘路径同样可用 | 已实测（本机） |
| 字体 | offscreen 在 Windows 上选 `QFreeTypeFontDatabase`，只扫 `<Qt前缀>/lib/fonts`，Qt 5.14 不再随包发字体 → 每次建控件都报 `QFontDatabase: Cannot find font directory .../lib/fonts`；**系统字体不进这个库** | windows 插件用系统字体库，无此告警 | 已实测（本机）+ 源码（`qoffscreenintegration.cpp:117`） |

### 3.2 不能用：`QMessageBox` 在 offscreen / minimal 下必崩

最小复现（`QMessageBox::warning` 同路径：构造 → `show()`）：

```
step1 constructed
This plugin does not support propagateSizeHints()

Thread 1 received signal SIGSEGV, Segmentation fault.
0x000000000106bed7 in qt_getWindowsSystemMenu(QWidget const*) () from ...\Qt5Widgets.dll
#1 QMessageBox::showEvent(QShowEvent*)
#2 QWidget::event(QEvent*)
...
#9 QDialog::setVisible(bool)
#10 main ()
```

| 项 | 事实 | 证据 | 判定 |
|---|---|---|---|
| 崩溃范围 | `QT_QPA_PLATFORM=offscreen` 与 `minimal` 都崩；`windows` 插件正常。Debug 与 Release 都崩 | 三种插件 × 两种构建各跑一次，`msgbox.exe` 退出码 0xC0000005 | 已实测（本机） |
| 根因 | `QMessageBox::showEvent` 在 `Q_OS_WIN` 下调用 `qt_getWindowsSystemMenu(this)`；该函数取 `QGuiApplication::platformNativeInterface()->nativeResourceForWindow("handle", window)`。offscreen/minimal 没有覆写 `nativeInterface()`，基类返回 `0` → 空指针解引用 | 崩溃栈 + 源码：`qmessagebox.cpp:78-84`（helper）、`:1607-1612`（`#if defined(Q_OS_WIN) && !defined(Q_OS_WINRT)` 守卫）、`qplatformintegration.cpp:115-118`（`return 0`）、`qoffscreenintegration.h/cpp`（无 `nativeInterface` 覆写） | 元数据可证（源码）+ 已实测（栈） |
| 是否仅 5.14 | `qmessagebox.cpp` 的 5.15 分支同一段代码逐字相同（`qt_getWindowsSystemMenu` 与调用点） | <https://code.qt.io/cgit/qt/qtbase.git/plain/src/widgets/dialogs/qmessagebox.cpp?h=5.15> 第 78-84 / 1606-1609 行 | 元数据可证 |
| Linux 侧是否受影响 | 不受影响：整段被 `Q_OS_WIN` 包住，Linux 编译不进 | 同上源码 | 元数据可证（**未在 Ubuntu 实跑**） |
| 对本仓库的影响 | `app/mainwindow.cpp:922` 的错误路径就是 `QMessageBox::warning(this, title, message)`——任何触发错误弹窗的 UI 用例在 Windows offscreen 下会直接杀进程，不是断言失败 | `app/mainwindow.cpp:916-922` | 已实测（本机复现同 API） |
| 二手佐证 | 有第三方实践记录称 Qt 5.15 offscreen 下 `QMessageBox::warning()` 段错误并需专门变通；本次一手栈与其描述一致，但未引用其结论 | 仅作旁证，不参与判定 | 未验证（二手，未采信） |

### 3.3 `windows-latest` 无交互会话下的可用性

| 项 | 事实 | 证据 | 判定 |
|---|---|---|---|
| offscreen 是否需要窗口站/桌面 | 插件不创建任何原生窗口：`QOffscreenWindow::winId()` 返回自增整数伪 ID，`setVisible` 只走 `QWindowSystemInterface`；唯一的系统交互是 `QEventDispatcherWin32` 的消息队列与字体目录扫描 | `qoffscreenwindow.cpp:117-151`、`qoffscreenintegration.cpp:142-149` | 元数据可证（源码） |
| runner 上实跑 | 未在 `windows-latest` 上执行过任何 Qt GUI/offscreen 进程 | — | **未验证**（见 §5 最小探针） |

由源码可推「不依赖交互桌面」，但这是推断，不写成结论：**推断，未验证**（runner 会话形态未取证）。

## 4. `SDL_AUDIODRIVER=dummy`

| 项 | 事实 | 证据 | 判定 |
|---|---|---|---|
| 本仓库 SDL 版本 | 内置 SDL 2.28.1，dummy 驱动源码在树内且被编译（`SDL_dummyaudio.c.obj` 在本地构建产物里） | `sdk/3rdparty/SDL-release-2.28.1/src/audio/dummy/SDL_dummyaudio.c`、`sdk/3rdparty/build/SDL-release-2.28.1/CMakeFiles/SDL2.dir/src/audio/dummy/` | 已实测（本机构建产物） |
| 可用性 | `SDL_AUDIODRIVER=dummy` 下 `SDL_Init(SDL_INIT_AUDIO)` 成功，`SDL_GetCurrentAudioDriver()` 返回 `dummy`，能打开 44100/S16/2ch/1024 设备 | 本机探针输出 `audio driver: dummy` / `opened: freq=44100 fmt=8010 ch=2 samples=1024`（默认驱动对照为 `wasapi`） | 已实测（本机） |
| CI 侧 | 两个 workflow 都设了该变量（Windows job 级 `env`、Linux 的 coverage 步骤），近期运行均绿 | run 44926994756（sdk-windows）、44926994752（sdk-coverage） | 已实测（CI 运行记录） |

结论：dummy 可用，且是本图之外已有的既定做法；本图不需要真实播放。

## 5. 仍未被证实的点与最小探针

1. **runner 上的 QtTest 实跑**（Windows / Linux 都还没有）。最小探针（不落进仓库、跑完即删的分支或 `workflow_dispatch` 临时 job）：一个 30 行 QtTest 目标，`find_package(Qt5 COMPONENTS Widgets Test)`，断言 ① `platformName()=="offscreen"` ② `QWidget::show()` 后 `qWaitForWindowExposed` ③ `QTimer` 在 `QDialog::exec()` 嵌套循环里推进；两端各打印 `Config:` 行与 `Totals:` 行。Linux 侧同一份源码，`-DCMAKE_PREFIX_PATH` 换系统 Qt 即可。
2. **`QMessageBox` 崩溃在 runner 上是否复现**：同一份 `qtbase` 二进制、同一段平台代码，崩溃不依赖会话（纯空指针），**推断会复现**——仍属推断、未验证。
3. **Linux offscreen 运行时行为**：`libqoffscreen.so` 存在是元数据事实，但未在 Ubuntu 上跑过任何 Qt 进程（本机无法代表）。
4. **Ubuntu 上 `QMessageBox` 是否真无此路径**：由 `Q_OS_WIN` 条件编译可推，未实跑。

## 6. 对 #248 的含义

- Qt5Test 双端都可获得，**卡点不在链接**：Windows 只需保证 Qt bin + MinGW bin 在 PATH（机制已存在，`SOUNDBRIDGE_QT_TEST_BIN_DIR`），无需 `windeployqt`、无需拷 `platforms/`。
- **Windows 侧的判据设计要绕开 `QMessageBox`**：错误弹窗类判据在 offscreen 下会整进程段错误。可行方向（都需要另立决策，不在本票范围）：断言错误状态而非弹窗、把弹窗收进可注入的展示接口、或仅 Linux 侧覆盖弹窗。
- `Qt::Popup`（本仓库的 `PopupPanel`，`app/popuppanel.cpp:24`）在 offscreen 下可用：可见、`isExposed`、`activePopupWidget`、`QTest::mouseClick` 命中 action 都成立。
- Windows offscreen 无系统字体，文本度量不可信；断言不要依赖像素宽度（与 `app-ui-test-feasibility.md` 的结论一致，且此处给出了机制：`QFreeTypeFontDatabase` + 空字体目录）。
- 双端 Qt 版本不同（5.14.2 / 5.15.13），同一用例需在两端都跑过才算判据成立。
