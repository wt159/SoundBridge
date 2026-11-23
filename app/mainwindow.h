/******************************************************************
Copyright (c) Deng Zhimao Co., Ltd. 1990-2021. All rights reserved.
* @projectName   14_musicplayer
* @brief         mainwindow.h
* @author        Deng Zhimao
* @email         1252699831@qq.com
* @net           www.openedv.com
* @date          2021-04-20
*******************************************************************/
#ifndef MAINWINDOW_H
#define MAINWINDOW_H
#include "playercontroller.h"
#include "popuppanel.h"
#include "soundbridge/sdk.h"
#include <QCheckBox>
#include <QCloseEvent>
#include <QColor>
#include <QDebug>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QListWidget>
#include <QMainWindow>
#include <QPushButton>
#include <QSet>
#include <QSlider>
#include <QSpacerItem>
#include <QTimer>
#include <QVBoxLayout>
#include <memory>
using namespace soundbridge;

/* Media information struct. */
struct MediaObjectInfo {
    /* Stores the song file name. */
    QString fileName;
    /* Stores the song file path. */
    QString filePath;
};

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    /* 构造注入（ADR 0016）：缺省 AppUiPorts = 生产现状（自建 PlayerController、
       曲库取 applicationDirPath()/../../music）。测试传替身，进程内零 SDL。 */
    MainWindow(const AppUiPorts &ports = AppUiPorts(), QWidget *parent = nullptr);
    ~MainWindow();

    static const QColor kSelectBg;
    static const QColor kPlayBg;
    static const QColor kPlayBgSelected;
    static const QColor kPlayBar;

public slots:
    /* 渲染入口（ADR 0016 双缝之二）：由 private slots 提升为 public slots，
       不改名不改签名——既有 SLOT() 连接不变，测试直接调用注入一帧状态。 */
    void renderPlayerViewState(PlayerViewState viewState);

public:
    /* 错误弹窗的六个字段组装（UI-E2 文案半边）：从 handlePlayerError 提取为
       纯函数，呈现（QMessageBox）永久人工（规格 §9.7.5）。 */
    static QString buildErrorMessageBoxText(int codeValue, const QString &detail, int trackIndex,
                                            const QString &path, const QString &traceId);

private:
    /* 命令口（ADR 0016）：生产指向 mController.get()，注入时指向替身。
       mController 仅在生产路径持有（唯一构造点不变）。 */
    std::unique_ptr<PlayerController> mController;
    PlayerCommandPort *m_commands    = nullptr;
    QObject *m_signalSource = nullptr;
    QString mMusicDirectory;
    QStringList mRenderedPlaylist;
    int mRenderedTrackIndex = -1;
    /* 列表编辑命令之后的选中落点（§8.9）：第一次全量重建把 currentRow 放到
       min(删掉的行号, n-1)，用后清零；其余刷新仍取播放行。
       `mPendingSelectCount` 是删除后的预期条数，只有「恰好少一条」的重建才消费锚点。 */
    int mPendingSelectRow   = -1;
    int mPendingSelectCount = -1;

    /* Music list. */
    QListWidget *mListWidget;

    /* Playback progress slider. */
    QSlider *mDurationSlider;

    /* Player control buttons. */
    QPushButton *mPushButton[7];

    /* Vertical layouts. */
    QVBoxLayout *mVBoxLayout[3];

    /* Horizontal layouts. */
    QHBoxLayout *mHBoxLayout[4];

    /* Vertical containers. */
    QWidget *mVWidget[3];

    /* Horizontal containers. */
    QWidget *mHWidget[4];

    /* Label text. */
    QLabel *mLabel[4];
    QLabel *mNowPlayingTitle;
    QLabel *mNowPlayingSubtitle;
    QCheckBox *mAutoSkipCheck;

    /* 浮层（ADR 0015）。mListMask 已删除。 */
    VolumePopup *mVolumePopup;
    PlaylistPopup *mPlaylistPopup;

    /* 收藏（本地偏好，ADR 0014）。 */
    QSet<QString> mFavorites;
    QString mCurrentTrackId;

    /* 音量写盘去抖（规格 §6.1）。 */
    QTimer *mVolumeSaveTimer;

    /* 切歌等待的「切换中」呈现（地图 #285 / #289）：切歌命令提交起计时，
       超过 150 ms 未收到换曲结果才上屏 `SWITCHING`；撤回三条路——
       `trackSwitchSettled` 信号（TC 反映，含同曲重播）、错误信号（错误优先）、
       渲染帧发现列表清空。等待期 `PlayerState` 保持原值（SDK 契约），
       本计时纯 UI 层，不引入 SDK 事件。 */
    QTimer *mSwitchGuardTimer = nullptr;
    bool mSwitchingActive     = false;
    int mSwitchFromIndex      = -1;

    std::string mAppDir;
    std::string mLogDir;

protected:
    /* Build the music UI layout. */
    void musicLayout();

    void closeEvent(QCloseEvent *event);

    /* 主列表的 Delete（§8.9）：QListWidget 不处理该键，事件自然上抛到这里。 */
    void keyPressEvent(QKeyEvent *event) override;

    /* Scan local songs. */
    void scanSongs();

    /* Initialize media player. */
    void mediaPlayerInit(PlayerCommandPort *injectedPort);
    void applyWindowPolicy();
    void restoreWindowState();
    void persistWindowState();
    void updatePlaybackModeButton(PlaybackMode mode);
    QString playbackModeText(PlaybackMode mode) const;
    /* 音量与收藏的可见状态回填（规格 §8.4 / §8.5）。 */
    void updateFavoriteButton();
    void updateVolumeUi();

    /* 列表编辑的统一下发点（主列表 Delete 与面板 Delete 共用）。 */
    void requestRemoveTrack(int row);

    /* 「切换中」守卫（#289）：四个切歌命令槽的公共启动点 / 撤回公共尾部。 */
    void beginSwitchGuard();
    void clearSwitchGuard();

protected:
private slots:
    /* Play button clicked. */
    void btn_play_clicked();

    /* Next track button clicked. */
    void btn_next_clicked();

    /* Previous track button clicked. */
    void btn_previous_clicked();

    /* Favorite button clicked. */
    void btn_favorite_clicked();

    /* Play mode button clicked. */
    void btn_playMode_clicked();

    /* Playlist button clicked. */
    void btn_playList_clicked();

    /* Volume button clicked. */
    void btn_volume_clicked();

    /* List item clicked. */
    void listWidgetCliked(QListWidgetItem *);

    /* Duration slider released. */
    void durationSliderReleased();

    /* Auto skip checkbox toggled. */
    void autoSkipToggled(bool checked);

    /* 音量写盘（去抖计时器与 closeEvent 都走它，故须为槽）。 */
    void persistVolumeState();

    /* 音量滑杆变化（拖动即改）。 */
    void volumeSliderChanged(int volume);

    /* 静音开关切换。 */
    void muteToggled(bool muted);

    /* 面板内条目被点击（= selectTrack + 关闭面板）。 */
    void panelTrackActivated(int index);

    /* 面板内 Delete（删单条，面板不关闭）与确认条确认（清空）。 */
    void panelTrackRemoveRequested(int index);
    void panelClearPlaylistRequested();

    void handlePlayerError(int code, const QString &detail, int trackIndex, const QString &path,
                           const QString &traceId, bool autoSkipEnabled);

    /* 切歌计时器超窗（#289）：上屏 `SWITCHING`。 */
    void switchGuardTimeout();

    /* SDK 换曲结果到达（TC 反映，PlayerController 转发的 Qt 信号半边）：撤回。 */
    void playerTrackSwitchSettled();
};
#endif // MAINWINDOW_H
