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
#include "MusicPlayer.h"
#include <QDebug>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMainWindow>
#include <QCloseEvent>
#include <QPushButton>
#include <QSlider>
#include <QSpacerItem>
#include <QVBoxLayout>
#include <memory>

using namespace sdk;

/* Media information struct. */
struct MediaObjectInfo {
    /* Stores the song file name. */
    QString fileName;
    /* Stores the song file path. */
    QString filePath;
};

class MainWindow : public QMainWindow, public MusicPlayerListener {
    Q_OBJECT

public:
    MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

private:
    /* Music player instance. */
    std::shared_ptr<MusicPlayer> mMusicPlayer;

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

    /* Mask for list overlay. */
    QWidget *mListMask;

    /* Media info storage. */
    QVector<MediaObjectInfo> mMediaObjectInfo;

    std::string mAppDir;
    std::string mLogDir;
    MusicPlayerState mState;

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

protected:
    /* Media player state changed. */
    virtual void onMusicPlayerStateChanged(MusicPlayerState state);

    /* Current playlist index changed. */
    virtual void onMusicPlayerListCurrentIndexChanged(int index);

    /* Total duration changed. */
    virtual void onMusicPlayerDurationChanged(uint64_t duration);

    /* Playback position changed. */
    virtual void onMusicPlayerPositionChanged(uint64_t position);

    /* Playlist changed. */
    virtual void onMusicPlayerMusicListChanged(std::list<MusicIndex> list);

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
};
#endif // MAINWINDOW_H
