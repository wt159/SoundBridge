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
#include "soundbridge/sdk.h"
#include <QCheckBox>
#include <QCloseEvent>
#include <QColor>
#include <QDebug>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMainWindow>
#include <QPushButton>
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
    MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

    static const QColor kSelectBg;
    static const QColor kPlayBg;
    static const QColor kPlayBgSelected;
    static const QColor kPlayBar;

private:
    std::unique_ptr<PlayerController> mController;
    QStringList mRenderedPlaylist;
    int mRenderedTrackIndex = -1;

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

    /* Mask for list overlay. */
    QWidget *mListMask;

    std::string mAppDir;
    std::string mLogDir;

protected:
    /* Build the music UI layout. */
    void musicLayout();

    /* Handle window resize. */
    void resizeEvent(QResizeEvent *event);
    void closeEvent(QCloseEvent *event);

    /* Scan local songs. */
    void scanSongs();

    /* Initialize media player. */
    void mediaPlayerInit();
    void applyWindowPolicy();
    void restoreWindowState();
    void persistWindowState();
    void updatePlaybackModeButton(PlaybackMode mode);
    QString playbackModeText(PlaybackMode mode) const;

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

    void renderPlayerViewState(PlayerViewState viewState);
    void handlePlayerError(int code, const QString &detail, int trackIndex, const QString &path,
                           const QString &traceId, bool autoSkipEnabled);
};
#endif // MAINWINDOW_H
