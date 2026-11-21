# 0015 浮层底座用自绘 Qt::Popup，顺带删除 mListMask

- 状态：已接受
- 日期：2026-11-21
- 来源：[地图：占位控件转正](https://github.com/wt159/SoundBridge/issues/235)、[app 层浮层底座选型](https://github.com/wt159/SoundBridge/issues/239)、[播放列表面板的内容与只读语义](https://github.com/wt159/SoundBridge/issues/241)。规格落点 [`doc/app-ui-spec.md`](../../doc/app-ui-spec.md) §8.6 / §8.7。

## 背景

音量浮层与播放列表面板都需要一个"从按钮旁边弹出来、点外部就消失"的容器。`app/` 此前是**零浮层**：没有 `QMenu`、没有 `QDialog`，唯一的近似物是 `mListMask`——一个挂在 `QListWidget::viewport()` 上的 `QWidget`，`hide()` 后从未 `show()`，几何高度被 `setMinimumHeight(50)` 钳制，`WA_TransparentForMouseEvents` 让它不拦鼠标。它没有任何消费方，是死代码，但占着"列表遮罩"这个位置，容易让后来者以为存在一套浮层机制。

四个候选：

- **(a) `QMenu`**：定位、Esc、点外关闭、键盘导航全部现成；qss 只能部分定制；音量滑杆要 `QWidgetAction` 承载。
- **(b) 非模态 `QDialog`**：完全可样式化；定位、关闭、焦点全部自管；带窗口装饰，任务栏可能多一个条目。
- **(c) 自绘 `QWidget` + `Qt::Popup`**：最可控；关闭、焦点、Esc 需要显式处理。
- **(d) 两个控件各用不同机制。**

## 决策

**两控件共用一个自绘 `Qt::Popup` 底座（`app/popuppanel.h` / `app/popuppanel.cpp` 的 `PopupPanel`），并删除 `mListMask`。**

- **`Qt::Popup` 提供的行为**：popup grab 让"点击浮层外部"自动关闭，且不需要额外的事件过滤器；窗口无装饰、不进任务栏、不改变主窗口激活状态。
- **显式补三件事**：Esc 关闭（`keyPressEvent`）、打开时取焦点（Esc 与键盘操作的前提）、关闭后 `setFocus()` 回到触发按钮。
- **定位统一**：水平居中于触发按钮，垂直在按钮上方 8 px；上方空间不足时翻转到下方；左右钳制到 `QScreen::availableGeometry()`。两控件复用同一段几何计算，避免各写一套。
- **控件各管内容**：音量浮层是 `mVolumeSlider` + `btn_mute`；面板是 `mPanelList`（`QListWidget`，`setUniformItemSizes(true)`）。
- **删除 `mListMask`**：它没有消费方，且与新浮层同属"列表上方叠东西"的语义，留着会让判据 `UI-V8` 永远描述一个不存在的东西。随删除一并作废 `UI-V8` 与 §1.3 的 `mListMask` 锚点行。

## 考虑过的替代方案

**(a) `QMenu`。** 行为最省事。否决：音量滑杆要 `QWidgetAction` 包一层 `QSlider`，qss 对 `QMenu` 内嵌 widget 的定制能力有限（本项目样式全靠 `style.qss`）；更关键的是千曲面板用菜单项渲染既慢又不像列表（没有滚动条、没有 `scrollToItem` 居中）。

**(b) 非模态 `QDialog`。** 完全可样式化。否决：需要 `Qt::Tool` / `Qt::FramelessWindowHint` 组合才能去掉装饰，而 `Qt::FramelessWindowHint` 的窗口在 Windows 上仍会抢焦点、进任务栏；"点外部关闭"要自己装事件过滤器，跨平台行为还得逐个验证。相比 `Qt::Popup`，多写不少代码却换不到收益。

**(d) 两控件各用不同机制**（例如音量 `QMenu` + 面板 `QDialog`）。否决：两套关闭语义、两套定位、两套判据；用户在同一行按钮上点两个相邻控件会得到不一致的行为。

**(e) 保留 `mListMask`，新浮层绕开它。** 否决：死代码留着就要在规格里养一条永远无法验证的判据，而且它占据 `viewport()` 的子控件位置——将来谁想给列表加真正的覆盖层，会先踩到它。

**(f) 用 `QToolTip` 承载音量滑杆。** 否决：`QToolTip` 不接收鼠标事件，滑杆拖不动。

## 后果

- `app/` 首次有浮层机制与对应锚点（`mVolumePopup` / `mVolumeSlider` / `btn_mute` / `mPlaylistPopup` / `mPanelList`），规格 §8.7 锁定行为，判据 `UI-M5` / `UI-V9` / `UI-V10` 覆盖。
- `APP_SOURCES` 增加一对文件；`MainWindow` 增加两个成员，`resizeEvent` 不再需要处理遮罩几何（随之简化）。
- 删除 `mListMask` 使 `UI-V8` 与 §1.3 该行作废；`style.qss` 的 `#mListMask` 规则一并删除——否则 `UI-V6` 的"选择器只能指向锚点"会立刻红。
- 不引入动画、阴影或圆角之外的装饰；`Qt::Popup` 在部分 Linux 窗管下没有阴影，这是可接受的降级。
