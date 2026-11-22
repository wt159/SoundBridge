# 播放列表编辑交互的 offscreen 可测性取证（票 #261）

> 本文件是**时点快照**（取证 2026-11-22，`master @ 8b2b72c9a`），不是规格承诺。
> 环境：Windows 10 Pro，Qt **5.14.2** `mingw73_64`（release-only），MinGW **7.3.0** 64-bit，`QT_QPA_PLATFORM=offscreen`。
> 只读调查：未改 `sdk/`、`app/`、`doc/`、`docs/adr/`；探针工程在仓库外 `%TEMP%\sb-probe-playlist-edit\`（单文件 `main.cpp`，编译命令见 §7）。
> 判定三分：**已实测**（本次亲手跑出输出）/ **源码或头文件可证**（Qt 5.14.2 官方源码、随二进制发布的 QtTest 头文件）/ **未取证**。

## 0. 结论速览

| # | 问题 | 结论一句话 | 判定 |
|---|---|---|---|
| 1 | `QMenu::popup()` / `exec()` 在 offscreen | `popup()` 不阻塞且确实建出 `Qt::Popup` 顶层窗口（QPA 窗口存在、无 Win32 原生菜单）；`exec()` 内部跑 `QEventLoop` 会阻塞；菜单 action 可被 `QTest::mouseClick` 命中，也可 `trigger()` 直呼；但**右键手势本身不产生 `QContextMenuEvent`** | 已实测 + 源码可证 |
| 2 | `QTest::keyClick(listWidget, Key_Delete)` 投递链路 | 走 `qApp->notify(widget, …)` **直投**，窗口从未 `show()`、无焦点也照样送达；`nullptr` / `QWindow*` 重载才依赖焦点。`QShortcut` / `QAction` 快捷键仍会被消费（实测触发），`processEvents()` 不需要 | 已实测 + 头文件可证 |
| 3 | `QMessageBox` 的替代形态 | offscreen 下 `QMessageBox` 在 `show()` 必段错误（gdb 栈落在 `qt_getWindowsSystemMenu`）；内嵌确认条 / 撤销提示 / 无确认**都可被机器判定**（点击、可见性、信号均成立），普通 `QDialog` 也能跑，但受 ADR 0015 约束 | 已实测 + 源码可证 |
| 4 | `takeItem()` / `clear()` 后的 current/selection | `takeItem(当前行)` 后 Qt 把 current 移到**下一条**（末行则退到上一条，全删则 `-1`）并各发一次 `currentRowChanged`/`itemSelectionChanged`/`currentItemChanged`；`clear()` 把三者清空且发同样三个信号（空表时不发） | 已实测 + 源码可证 |
| 5 | 1000 项 `clear()+addItem()` 耗时 | 两轮各 3 次测得中位数 **1–2 ms**（冷启首填 6–13 ms），远低于 §8.6「数十毫秒」预算 | 已实测 |

一句话选型含义：**右键菜单能进判据，但「右键」这个手势不能**——判据要么直接调动作槽 / `trigger()`，要么把 `QContextMenuEvent` 投给 `viewport()`；这使「按钮触发」在自动化上比「右键菜单」少一层模拟。

## 1. `QMenu::popup()` / `exec()` 在 offscreen 下的行为

### 1.1 `popup()`：不阻塞，建出 `Qt::Popup` 顶层窗口

结论：`popup()` 只做布局、定位、`show()`，**不进入事件循环**，调用立即返回；顶层窗口是 Qt 自绘的 `Qt::Popup`（QPA 窗口对象存在，offscreen 下无原生句柄），不是 Win32 原生菜单。

实测（`probe.exe menu`，offscreen）：

```
[menu] popup: visible=1 activePopupIsMenu=1 hasWindowHandle=1 hasPlatformWindow=1 size=164x50
[menu] window: type=9 flags=0x8800f009 visible=1
```

`type=9` 即 `Qt::Popup`；`hasPlatformWindow=1` 说明 offscreen 插件也真的创建了 QPA 窗口（不是 `WA_DontShowOnScreen` 的假 show）。两个 action、每个 25 px 高 ⇒ 164×50。

源码：`QMenu::popup()` 全函数（`src/widgets/widgets/qmenu.cpp:2319`）只在末尾 `show()`，全程没有 `QEventLoop`。

### 1.2 `exec()`：阻塞，必须由外部关闭

结论：`exec()` **同步阻塞**，直到菜单被关闭（选中、Esc、`close()`）才返回；返回值是触发的 `QAction*`，未触发为 `nullptr`。自动化里直接调 `exec()` 会挂住，必须配 `QTimer::singleShot` 或改用 `popup()`。

实测：定时器 150 ms 后 `close()`，`exec()` 在 150 ms 返回 `nullptr`：

```
[menu] timer fired inside exec at 150 ms; closing
[menu] exec() returned rc=0000000000000000 after 150 ms (blocking=1)
```

源码（`qmenu.cpp:2633-2651`）：

```cpp
QAction *QMenu::exec(const QPoint &p, QAction *action)
{
    ...
    QEventLoop eventLoop;
    d->eventLoop = &eventLoop;
    popup(p, action);
    QPointer<QObject> guard = this;
    (void) eventLoop.exec();      // 阻塞点
    ...
}
```

### 1.3 菜单 action 能被点中，也能直呼

结论：两条模拟路径都能触发 action——① `QTest::mouseClick(&menu, …, actionGeometry(a).center())`（QtTest 控件重载，直投）；② `QTest::mouseClick(menu.windowHandle(), …, 窗口坐标)`（真 QPA 路径，经 `QWidgetWindow` 命中测试）；③ `actions().at(i)->trigger()` 连菜单都没 `show()` 也能触发。

实测：

```
[menu] actionGeometry(a0)=0,0 164x25
[menu] after QTest::mouseClick(menu, actionPos): t0=1 t1=0 menuVisible=0
[menu] after QTest::mouseClick(menu.windowHandle(), actionPos): t0=1 t1=1
[menu] after actions().at(0)->trigger(): t0=2 t1=1
[menu] bare(never shown) trigger(): tb=1
```

注：offscreen 下 `QCursor::pos()` 恒为 (0,0) 一类，`snapToMouse`/屏幕钳制分支不可信，不要断言菜单绝对坐标；断言「哪个 action 被触发」是稳的。

### 1.4 关键陷阱：右键点击**不**产生上下文菜单事件

结论：`QTest::mouseClick(widget, Qt::RightButton)` **不会**合成 `QContextMenuEvent`，因此 `customContextMenuRequested` 不会发出、`contextMenuEvent()` 不会被调。原因是本机 Qt 5.14.2 的 QtTest **没有**定义 `QTEST_QPA_MOUSE_HANDLING`（`qtestmouse.h:190` 的 `#ifdef` 分支不成立，头文件与 mkspecs 里全局搜不到该宏），于是走 `#else` 分支：`qApp->notify(widget, &me)` 直投（`qtestmouse.h:242`），绕过了 `QWidgetWindow` 里「右键释放 ⇒ 生成 ContextMenu」的合成逻辑。

实测：

```
[menu] right-click via QTest::mouseClick(list): customContextMenuRequested=0 listContextMenuEvent=0
[menu] after manual QContextMenuEvent: customContextMenuRequested=0 listContextMenuEvent=0   ← 投给了 QListWidget 本身
[menu2] sendEvent(viewport): ctx=1 pos=147,7                                                  ← 投给 viewport 才成功
[menu2] QTest right-click viewport: ctx=1
[menu2] QTest right-click list: ctx=1
[menu2] DefaultContextMenu sendEvent(list2): contextEvents=0
[menu2] DefaultContextMenu sendEvent(viewport): contextEvents=1
```

第二行是第二个陷阱：`QContextMenuEvent` 直投给 `QListWidget`（`QAbstractScrollArea` 子类）会被吞掉——`qabstractscrollarea.cpp:959-963`：

```cpp
case QEvent::ContextMenu:
    if (static_cast<QContextMenuEvent *>(e)->reason() == QContextMenuEvent::Keyboard)
        return QFrame::event(e);
    e->ignore();
    break;
```

鼠标来源（`reason()==Mouse`）被 `ignore()`，既不转发也不发信号；真实用户右键时事件是由 `QWidgetWindow` 命中到 **viewport** 上的。所以自动化要么 `QApplication::sendEvent(list->viewport(), &ce)`，要么干脆不进「手势」层。

### 1.5 对「右键菜单能否进判据」的回答

能，但要拆开：

- **菜单展示与动作选择**：可判。`popup()` 后 `isVisible()` / `QApplication::activePopupWidget()` / `actionGeometry()` 都可用；点 action 或 `trigger()` 都能观测到「哪个动作被选中」。
- **右键手势**：不可判（Qt 5.14.2 + offscreen，本机二进制的 QtTest 直投模型下）。想覆盖只能自己送 `QContextMenuEvent` 到 `viewport()`，那时测的是「处理函数」，不是「用户手势」。
- **`exec()`**：不适合自动化（阻塞），除非同时挂定时器。

## 2. `QTest::keyClick(listWidget, Qt::Key_Delete)` 的投递链路

### 2.1 头文件实现（随二进制发布，即本机实际编译进去的代码）

`QTest::keyClick(QWidget*, …)` → `sendKeyEvent`（`qtestkeyboard.h:201`）→ `simulateEvent`（`:181`）：

```cpp
QKeyEvent a(press ? QEvent::KeyPress : QEvent::KeyRelease, code, modifier, text, repeat);
QSpontaneKeyEvent::setSpontaneous(&a);
if (press && qt_sendShortcutOverrideEvent(widget, a.timestamp(), code, modifier, text, repeat))
    return;
if (!qApp->notify(widget, &a))        // ← 直投目标控件，不经 QPA、不看焦点
    QTest::qWarn("Keyboard event not accepted by receiving widget");
```

只有目标为 `nullptr` 时才按 `keyboardGrabber → activePopupWidget → focusWindow → focusWidget → activeWindow` 去找（`:206-223`），找不到就 `QTEST_ASSERT`（`qFail`，**不是** abort——实测静默返回）。

### 2.2 实测矩阵（offscreen）

| 场景 | 前置 | 送达 `keyPressEvent` |
|---|---|---|
| 控件重载，窗口从未 `show()`、`activeWindow=null`、`focusWidget=null` | 无 | **1**（`[key2] hidden, widget-overload: delivered=1`） |
| 控件重载，`clearFocus()` + `setActiveWindow(nullptr)` | 无 | **1**（`[key2] no-focus, widget-overload: delivered=1`） |
| `handleKeyEvent(window, …)`（QPA 路径），无焦点 | 无 | **0**（`[key2] no-focus, QWSI window path: delivered=0`） |
| `keyClick(window, …)`，窗口已激活、`list->setFocus()` | 需焦点 | **1**（`[key] D window-overload with focus: delivered=1`） |
| `keyClick(nullptr)`，全局无焦点 | — | 0（静默 `qFail`，进程正常退出） |

所以：**不需要 `activateWindow()` / `setFocus()` / `qApp->processEvents()` 前置**——`notify` 是同步调用，事件在 `keyClick` 返回时已经处理完。

### 2.3 但快捷键会截胡（实测）

窗口上有 `QShortcut(QKeySequence(Qt::Key_Delete))` 和 `QAction`（Backspace）时：

```
[key3] keyClick(list, Delete): QShortcut activated=1 listKeyPresses=0
[key3] keyClick(list, Backspace): QAction shortcut triggered=1 listKeyPresses=0
[key3] keyClick(window, Delete): QShortcut activated=2 listKeyPresses=0
```

直投路径仍会走 ShortcutOverride + `QApplication::notify` 的快捷键分发，**被 shortcut 消费后控件的 `keyPressEvent` 收不到**。这其实是好消息：判据挂在 `QShortcut` / `QAction` 上也能被判（断言触发次数即可），不必依赖控件子类。

### 2.4 边界（未取证/需注意）

- 直投证明「事件能进控件」，**不**证明真实键盘输入在 offscreen 下能到达（真实输入要先有焦点窗口，本机未构造该场景）。
- `QListWidget` 默认对 Delete **无任何行为**（`[key] F … count=5 currentRow=1`）——删除语义必须由本仓库自己实现，测试只能验证「自己实现的处理器被调到」。

## 3. `QMessageBox` 的替代形态

### 3.1 崩溃机制（源码 + gdb 栈，双证据）

`qmessagebox.cpp:77-85`：

```cpp
#if defined(Q_OS_WIN) && !defined(Q_OS_WINRT)
HMENU qt_getWindowsSystemMenu(const QWidget *w)
{
    if (QWindow *window = QApplicationPrivate::windowForWidget(w))
        if (void *handle = QGuiApplication::platformNativeInterface()->nativeResourceForWindow("handle", window))
            return GetSystemMenu(reinterpret_cast<HWND>(handle), false);
    return 0;
}
#endif
```

`showEvent`（`:1592-1613`）无条件调用它。offscreen 插件没有 native interface ⇒ `platformNativeInterface()` 返回 `nullptr` ⇒ 空指针解引用。gdb 实测栈：

```
Thread 1 received signal SIGSEGV, Segmentation fault.
0x…bed7 in qt_getWindowsSystemMenu(QWidget const*) () from …\Qt5Widgets.dll
#1 0x…0814 in QMessageBox::showEvent(QShowEvent*) () from …\Qt5Widgets.dll
#2 0x…5068 in QWidget::event(QEvent*) ()
#3 0x…d311 in QMessageBox::event(QEvent*) ()
#7 0x…15aa in QWidgetPrivate::show_helper ()
#8 0x…4cb4 in QWidgetPrivate::setVisible(bool)
#9 0x…1fd4 in QDialog::setVisible(bool)
```

注意栈上没有 `qwindows.dll`/`qoffscreen.dll` 帧——纯 `Qt5Widgets` 内部空指针，与平台插件实现无关，**静态 `QMessageBox::warning()` 同路**（底层同样 `show_new_message_box`）。

### 3.2 替代形态实测对照

| 形态 | offscreen 可运行 | 可被机器判定 | 实测输出 |
|---|---|---|---|
| `QMessageBox`（任意按钮组合） | ❌ 段错误 | 不可能 | 构造成功、`show()` 即崩 |
| 内嵌确认条（`QWidget` + 两个 `QPushButton`，行内） | ✅ | 可见性 / 文本 / 点击 / 信号 | `[dialog] inline bar visible=1 undoVisible=1 undoGeom=270,9 121x14`；`after click UNDO: clicks=1` |
| 删除撤销提示（同上，文案 + `UNDO`） | ✅ | 同上（断 `undoClicks` 与提示文本） | 同上（同一次实测里既测了点击也测了可见性） |
| 无确认（直接删） | ✅ | 列表行数 / `count()` / 重填后条目 | §4 的 `takeItem` 实测即该路径 |
| 普通 `QDialog`（非 QMessageBox） | ✅ | `result()` / 可见性 / 按钮点击 | `[dialog] QDialog visible=1`；点 DELETE 后 `result=1 visible=0` |

普通 `QDialog` 能跑是因为崩溃只发生在 `QMessageBox::showEvent` 的 Windows 专有块，`QDialog` 不碰这段。但 `docs/adr/0015` 已否决 `QDialog` 作为浮层形态（`PopupPanel` 是唯一底座），所以可选项仍是「行内条 / 撤销 / 无确认」三种。

### 3.3 结论

**三种替代形态都能被机器判定**，判定点分别是：内嵌条（按钮 `clicked` 信号 + `isVisible()` + 文案）、撤销提示（`undoClicks` + 提示文本 `1 track deleted` 类）、无确认（列表 `count()` 与内容）。`QMessageBox` 在 Windows+offscreen 下没有任何可用形态，判据里不得出现它（这与 `.workbuddy/memory/2026-11-22.md` 记录的既有事实一致，本票补上了源码位置与崩溃栈）。

## 4. `takeItem()` 与 `clear()` 语义（Qt 5.14）

### 4.1 源码位置

- `QListWidget::takeItem`（`qlistwidget.cpp`）：边界检查后 `d->listModel()->take(row)`；`QListModel::take` 用 `beginRemoveRows/endRemoveRows` 包裹单行移除，**不 delete**（返回给调用方）。
- `QListWidget::clear`：`selectionModel()->clear(); listModel()->clear();`；`QListModel::clear` 是 `beginResetModel()` … `endResetModel()`（`qlistwidget.cpp:72-84`），并在循环里 `delete` 每个 item。
- 当前行的搬移不在 `QListWidget`，在 `QAbstractItemView::rowsAboutToBeRemoved`（`qabstractitemview.cpp:3376-3410`）：

```cpp
QModelIndex current = currentIndex();
if (d->selectionMode == SingleSelection && current.isValid()
    && current.row() >= start && current.row() <= end && current.parent() == parent) {
    int totalToRemove = end - start + 1;
    if (d->model->rowCount(parent) <= totalToRemove) {   // 全删光
        ... setCurrentIndex(parent);                     // ⇒ 无效索引 ⇒ currentRow -1
    } else {
        int row = end + 1;                               // 优先下一条
        do { next = d->model->index(row++, current.column(), current.parent()); }
        while (next.isValid() && (isIndexHidden(next) || !d->isIndexEnabled(next)));
        if (row > d->model->rowCount(parent)) {           // 没有下一条 ⇒ 退到上一条
            row = start - 1;
            do { next = d->model->index(row--, current.column(), current.parent()); }
            while (next.isValid() && (isIndexHidden(next) || !d->isIndexEnabled(next)));
        }
        setCurrentIndex(next);
    }
}
```

### 4.2 实测（5 条列表，`probe.exe list` / `list2`）

| 操作 | count | currentRow | currentItem | 信号（row/sel/item） |
|---|---|---|---|---|
| 初始 current=2 | 5 | 2 | `track 2` | — |
| `takeItem(2)`（当前中间行） | 4 | **2**（同一行号，内容变 `track 3`） | 下一条 | 1 / 1 / 1 |
| `takeItem(0)`（当前行） | 4 | **0**（变 `track 1`） | 下一条 | 1 / — |
| `takeItem(末行)`（current=3/4） | 3 | **2**（退到上一条 `track 3`） | 上一条 | 1 / — |
| `takeItem(唯一一条)` | 0 | **-1** | `nullptr` | 1 / 1 / 1 |
| `takeItem(0)` 而 current=3（删**上方**行） | 4 | **2**（同一 item，索引静默位移） | `y3` 不变 | **0 / 0 / 0** |
| `clear()`（非空） | 0 | **-1** | `nullptr` | 1 / 1 / 1 |
| `clear()`（已空） | 0 | -1 | `nullptr` | 0 / 0 / 0 |

`ExtendedSelection` 下同样的删除（首行/末行）可见结果一致（`currentRow` 同样落到下一条/上一条）——所以**别把「自动选下一行」当成 `SingleSelection` 专属行为**；源码里那段显式搬移确实只在 `SingleSelection` 生效，但 `QItemSelectionModel` 自己也会修复无效 current，最终可见结果相同。

### 4.3 两个设计陷阱

1. **删除当前行不会保留「同一 item」，只保留「同一行号」**：`takeItem(2)` 后 `currentRow()` 仍是 2，但指向的是原来的第 3 条。判据若断言 `currentRow` 不变就通过，断言「选中项是同一条曲目」就会失败。
2. **批量删除上方行时 `currentRow` 会静默左移且不发信号**（3→2）。所以「按下标循环 `takeItem`」必须**倒序**，或每轮重新用 `currentItem()` 取指针；判据不能在循环中间缓存行号。

### 4.4 结论

删当前行后 Qt 会自动给出一个合法 current（下一条优先、末行退上一条、删光为 `-1`），并各发一次三类信号；`clear()` 在非空时同样发一次三类信号、空表时沉默。两者都不会「把 current 留在越界位置」，所以 UI 不需要额外兜底，但需要接受「currentRow 是行号语义」而不是「曲目身份语义」。

## 5. 1000 项全量重填耗时

被测形态与 `app/popuppanel.cpp:233` `PlaylistPopup::setTracks()` 一致：`clear()` + 循环 `addItem()` + `setCurrentRow(0)` + `processEvents()`；`setUniformItemSizes(true)`，窗口 600×480 可见，offscreen。

两次独立运行、各 3 轮：

```
# 运行 A
[perf] round 0: clear+addItem(1000)+processEvents = 13 ms
[perf] round 1: … = 2 ms
[perf] round 2: … = 1 ms
[perf] median = 2 ms (count=1000)

# 运行 B（拆分计时）
[perf] round 0: clear=0 ms, addItem x1000=1 ms, total=6 ms
[perf] round 1: clear=0 ms, addItem x1000=0 ms, total=1 ms
[perf] round 2: clear=0 ms, addItem x1000=0 ms, total=1 ms
[perf] median = 1 ms (count=1000)
```

结论：中位数 **1–2 ms**，冷启首填 **6–13 ms**，全部落在 §8.6「千曲量级约数十毫秒可接受」的预算内（余量一个数量级）。

口径提醒：offscreen 无系统字体（`QFontDatabase: Cannot find font directory …`）、绘制近似空转，整轮耗时里剩下的那几毫秒主要是 `processEvents()` 的布局/虚拟绘制。真机 `windows` 插件的首帧绘制会更贵，但 `addItem` 本身是摊还 O(1)、`setUniformItemSizes` 会跳过逐项测量，**数量级不会变**。若将来要卡这条预算，应测「重填 + 首帧呈现」而不是只测 `addItem`。

## 6. 对「右键菜单 vs 按钮」选型的影响

| 维度 | 右键上下文菜单 | 按钮 / 行内控件 |
|---|---|---|
| 弹出可判 | 可（`popup()` + `isVisible()` + `actionGeometry()`） | 可 |
| 手势可判 | **不可**（右键不合成 ContextMenu，需手工送事件到 `viewport()`） | 可（`QTest::mouseClick` 直投或 QPA 路径都命中） |
| 选中哪个动作可判 | 可（`trigger()` 或点 action） | 可（`clicked` 信号） |
| 阻塞风险 | `exec()` 阻塞，需定时器；`popup()` 无风险 | 无 |
| 与既有底座 | 需新建 `QMenu`，绕开 `PopupPanel`（ADR 0015） | 可复用 `PopupPanel` / 行内控件 |

**最可能改变选型的一条是 §1.4**：右键手势在 Qt 5.14.2 的 QtTest 直投模型下不产生 `QContextMenuEvent`，判据只能测「处理函数」而非「用户动作」，且必须知道「事件要投给 `viewport()` 而不是 `QListWidget`」这个非直觉细节。相比之下按钮路径的点击语义与用户操作一一对应，判据不需要特例。若选型只看「能否自动化」，右键菜单仍可留；若要求「判据等价于用户动作」，按钮占优。

## 7. 复跑方式

探针源码：`%TEMP%\sb-probe-playlist-edit\main.cpp`（仓库外，未跟踪），`platforms/` 从 `D:/Qt/5.14.2/mingw73_64/plugins/` 整目录拷到 exe 旁（缺 `qoffscreen.dll` 会卡在致命对话框、无输出）。

```bash
export PATH="/d/Qt/Tools/mingw730_64/bin:/d/Qt/5.14.2/mingw73_64/bin:$PATH"
cd "$TEMP/sb-probe-playlist-edit"
g++ -std=c++11 -O2 -DQT_WIDGETS_LIB -DQT_GUI_LIB -DQT_CORE_LIB main.cpp -o probe.exe \
  -I d:/Qt/5.14.2/mingw73_64/include \
  -I d:/Qt/5.14.2/mingw73_64/include/QtCore \
  -I d:/Qt/5.14.2/mingw73_64/include/QtGui \
  -I d:/Qt/5.14.2/mingw73_64/include/QtGui/5.14.2 \
  -I d:/Qt/5.14.2/mingw73_64/include/QtWidgets \
  -I d:/Qt/5.14.2/mingw73_64/include/QtTest \
  -L d:/Qt/5.14.2/mingw73_64/lib \
  -lQt5Test -lQt5Widgets -lQt5Gui -lQt5Core

QT_QPA_PLATFORM=offscreen ./probe.exe menu     # §1
QT_QPA_PLATFORM=offscreen ./probe.exe menu2    # §1.4
QT_QPA_PLATFORM=offscreen ./probe.exe key      # §2
QT_QPA_PLATFORM=offscreen ./probe.exe key2     # §2.2
QT_QPA_PLATFORM=offscreen ./probe.exe key3     # §2.3
QT_QPA_PLATFORM=offscreen ./probe.exe list     # §4.2
QT_QPA_PLATFORM=offscreen ./probe.exe list2    # §4.2 / §4.3
QT_QPA_PLATFORM=offscreen ./probe.exe perf     # §5
QT_QPA_PLATFORM=offscreen ./probe.exe dialog   # §3.2
QT_QPA_PLATFORM=offscreen ./probe.exe msgbox   # §3.1，预期 exit 139
```

`msgbox` 崩溃栈复跑：

```bash
QT_QPA_PLATFORM=offscreen gdb -batch -ex run -ex "bt 12" --args ./probe.exe msgbox
```

源码引用来源：本机 `D:/Qt/5.14.2/Src` **不存在**，`qmenu.cpp` / `qlistwidget.cpp` / `qabstractitemview.cpp` / `qabstractscrollarea.cpp` / `qmessagebox.cpp` 取自官方镜像 `raw.githubusercontent.com/qt/qtbase/v5.14.2`（`curl` 下载到 `/tmp` 后本地 `grep`，行号为该 tag 文件内行号）；QtTest 头文件引用的是 `D:/Qt/5.14.2/mingw73_64/include/QtTest/` 随二进制发布的版本，即本机实际编译进探针的代码。

## 8. 未能取证

1. **真机 `windows` 插件下的菜单/首帧耗时**：本票全程 offscreen，未跑可见窗口；§5 的预算结论只覆盖 offscreen 数量级。
2. **无焦点时窗口级 `QShortcut` 是否触发**：§2.3 的快捷键实测都在 `setFocus()` 之后；`activeWindow=null` 情形未测。因此「判据挂在 shortcut 上」建议仍显式 `setFocus()`（成本低、无副作用）。
3. **真实键盘输入（QPA 注入 + 焦点窗口）在 offscreen 是否可达**：只测了 `handleKeyEvent(window)` 有焦点时送达（`delivered=1`）、无焦点时丢弃（`0`），未测窗口激活序列（`activateWindow` + 事件循环）能否让 offscreen 建立 focusWindow。
4. **Linux offscreen**：未在任何 Linux 上运行本探针；§3.1 的崩溃由 `Q_OS_WIN` 条件编译可推为 Windows 专有，但未实跑。
5. **`QMenu::popup()` 在 offscreen 的 grab 行为**（点外部关闭）：未测；本票只测可见性与命中。
