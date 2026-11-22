#pragma once

#include <QStringList>
#include <QWidget>

class QLabel;
class QListWidget;
class QListWidgetItem;
class QPushButton;
class QSlider;

// 浮层底座（ADR 0015）：自绘 QWidget + Qt::Popup。
//
// - 点外部关闭由 popup grab 提供；Esc 显式处理；关闭后焦点回到触发按钮。
// - 定位见规格 doc/app-ui-spec.md §8.7：水平居中于触发按钮，垂直优先在上方，
//   上方空间不足时翻转到下方，并钳制到屏幕可用区域。
class PopupPanel : public QWidget {
    Q_OBJECT

public:
    PopupPanel(const QString &panelObjectName, QWidget *anchor);
    ~PopupPanel() override;

    // 按可用空间定尺寸、定位、显示并取焦点。
    void popup();

protected:
    void keyPressEvent(QKeyEvent *event) override;
    void hideEvent(QHideEvent *event) override;

    void setPreferredSize(int width, int height);
    QWidget *anchor() const;

private:
    void positionNearAnchor();

    QWidget *m_anchor;
    int m_preferredWidth;
    int m_preferredHeight;
};

// 音量浮层：滑杆 0–100 + 独立静音开关（规格 §8.5）。
class VolumePopup : public PopupPanel {
    Q_OBJECT

public:
    explicit VolumePopup(QWidget *anchor);

    // 从当前状态回填控件；不回发信号（避免渲染触发写盘）。
    void syncFromState(int volume, bool muted);

signals:
    void volumeChanged(int volume);
    void mutedChanged(bool muted);

protected:
    void showEvent(QShowEvent *event) override;

private slots:
    void sliderValueChanged(int value);
    void muteButtonToggled(bool checked);

private:
    QSlider *m_slider;
    QPushButton *m_muteButton;
};

// 播放列表面板：主列表的第二个只读视图（规格 §8.6），并承载列表编辑的两个入口
// （§8.9：面板内 Delete、CLEAR + 内嵌确认条）。
class PlaylistPopup : public PopupPanel {
    Q_OBJECT

public:
    explicit PlaylistPopup(QWidget *anchor);

    /* preferredRow ≥ 0 时把面板选中行放到 min(preferredRow, n-1)：列表编辑命令之后的
       第一次全量重建用（§8.9 选中落点），其余刷新传 -1 = 跟播放行。 */
    void setTracks(const QStringList &names, int currentIndex, int preferredRow = -1);
    void updateCurrentIndex(int index);

signals:
    void trackActivated(int index);
    /* 面板内 Delete：删单条，面板不关闭（可以接着连删）。 */
    void trackRemoveRequested(int index);
    /* 确认条点确认：一条命令，面板随即关闭。 */
    void clearPlaylistRequested();

protected:
    void keyPressEvent(QKeyEvent *event) override;
    void hideEvent(QHideEvent *event) override;
    void showEvent(QShowEvent *event) override;

private slots:
    void listItemClicked(QListWidgetItem *item);
    void clearButtonClicked();
    void confirmClearClicked();
    void cancelClearClicked();

private:
    void setConfirmVisible(bool visible);
    void updateConfirmText();

private:
    QListWidget *m_list;
    /* 常态入口与确认条占同一格，同时只显示一个（§8.9）。 */
    QPushButton *m_clearButton;
    QWidget *m_confirm;
    /* 确认条提示文案：§1.3 不登记锚点，故不设 objectName（UI-A1 的集合比对）。 */
    QLabel *m_confirmText;
    QPushButton *m_confirmButton;
    QPushButton *m_cancelButton;
};
