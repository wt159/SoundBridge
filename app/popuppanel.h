#pragma once

#include <QStringList>
#include <QWidget>

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

// 播放列表面板：主列表的第二个只读视图（规格 §8.6）。
class PlaylistPopup : public PopupPanel {
    Q_OBJECT

public:
    explicit PlaylistPopup(QWidget *anchor);

    void setTracks(const QStringList &names, int currentIndex);
    void updateCurrentIndex(int index);

signals:
    void trackActivated(int index);

protected:
    void showEvent(QShowEvent *event) override;

private slots:
    void listItemClicked(QListWidgetItem *item);

private:
    QListWidget *m_list;
};
