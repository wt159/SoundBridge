/******************************************************************
Copyright © Deng Zhimao Co., Ltd. 1990-2021. All rights reserved.
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
#include <QPushButton>
#include <QSlider>
#include <QSpacerItem>
#include <QVBoxLayout>
#include <memory>

using namespace sdk;

/* 媒体信息结构体 */
struct MediaObjectInfo {
    /* 用于保存歌曲文件名 */
    QString fileName;
    /* 用于保存歌曲文件路径 */
    QString filePath;
};

class MainWindow : public QMainWindow, public MusicPlayerListener {
    Q_OBJECT

public:
    MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

private:
    /* 媒体播放器，用于播放音乐 */
    std::shared_ptr<MusicPlayer> musicPlayer;

    /* 音乐列表 */
    QListWidget *listWidget;

    /* 播放进度条 */
    QSlider *durationSlider;

    /* 音乐播放器按钮 */
    QPushButton *pushButton[7];

    /* 垂直布局 */
    QVBoxLayout *vBoxLayout[3];

    /* 水平布局 */
    QHBoxLayout *hBoxLayout[4];

    /* 垂直容器 */
    QWidget *vWidget[3];

    /* 水平容器 */
    QWidget *hWidget[4];

    /* 标签文本 */
    QLabel *label[4];

    /* 用于遮罩 */
    QWidget *listMask;

    /* 媒体信息存储 */
    QVector<MediaObjectInfo> mediaObjectInfo;

    std::string appDir;
    std::string logDir;
    MusicPlayerState m_state;

protected:
    /* 音乐布局函数 */
    void musicLayout();

    /* 主窗体大小重设大小函数重写 */
    void resizeEvent(QResizeEvent *event);

    /* 扫描歌曲 */
    void scanSongs();

    /* 媒体播放器类初始化 */
    void mediaPlayerInit();

protected:
    /* 媒体状态改变 */
    virtual void onMusicPlayerStateChanged(MusicPlayerState state);

    /* 媒体列表项改变 */
    virtual void onMusicPlayerListCurrentIndexChanged(int index);

    /* 媒体总长度改变 */
    virtual void onMusicPlayerDurationChanged(uint64_t duration);

    /* 媒体播放位置改变 */
    virtual void onMusicPlayerPositionChanged(uint64_t position);

private slots:
    /* 播放按钮点击 */
    void btn_play_clicked();

    /* 下一曲按钮点击*/
    void btn_next_clicked();

    /* 上一曲按钮点击 */
    void btn_previous_clicked();

    /* 收藏按钮点击 */
    void btn_favorite_clicked();

    /* 播放模式按钮点击 */
    void btn_playMode_clicked();

    /* 播放列表按钮点击 */
    void btn_playList_clicked();

    /* 播放音量按钮点击 */
    void btn_volume_clicked();

    /* 列表单击 */
    void listWidgetCliked(QListWidgetItem *);

    /* 播放进度条松开 */
    void durationSliderReleased();
};
#endif // MAINWINDOW_H
