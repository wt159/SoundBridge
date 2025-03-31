/******************************************************************
Copyright © Deng Zhimao Co., Ltd. 1990-2021. All rights reserved.
* @projectName   14_musicplayer
* @brief         mainwindow.cpp
* @author        Deng Zhimao
* @email         1252699831@qq.com
* @net           www.openedv.com
* @date          2021-04-20
*******************************************************************/
#include "mainwindow.h"
#include <QCoreApplication>
#include <QDir>
#include <QFileInfoList>

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , mState(MusicPlayerState::StoppedState)
{
    /* 布局初始化 */
    musicLayout();

    /* 媒体播放器初始化 */
    mediaPlayerInit();

    /* 扫描歌曲 */
    scanSongs();

    /* 按钮信号槽连接 */
    connect(mPushButton[0], SIGNAL(clicked()), this, SLOT(btn_previous_clicked()));
    connect(mPushButton[1], SIGNAL(clicked()), this, SLOT(btn_play_clicked()));
    connect(mPushButton[2], SIGNAL(clicked()), this, SLOT(btn_next_clicked()));
    connect(mPushButton[3], SIGNAL(clicked()), this, SLOT(btn_favorite_clicked()));
    connect(mPushButton[4], SIGNAL(clicked()), this, SLOT(btn_playMode_clicked()));
    connect(mPushButton[5], SIGNAL(clicked()), this, SLOT(btn_playList_clicked()));
    connect(mPushButton[6], SIGNAL(clicked()), this, SLOT(btn_volume_clicked()));
    /* 媒体信号槽连接 */
    // connect(mMusicPlayer, SIGNAL(stateChanged(QMediaPlayer::State)), this,
    //         SLOT(mediaPlayerStateChanged(QMediaPlayer::State)));
    // connect(mediaPlaylist, SIGNAL(currentIndexChanged(int)), this,
    //         SLOT(mediaPlaylistCurrentIndexChanged(int)));
    // connect(mMusicPlayer, SIGNAL(durationChanged(qint64)), this,
    //         SLOT(musicPlayerDurationChanged(qint64)));
    // connect(mMusicPlayer, SIGNAL(positionChanged(qint64)), this,
    //         SLOT(mediaPlayerPositionChanged(qint64)));

    /* 列表信号槽连接 */
    connect(mListWidget, SIGNAL(itemClicked(QListWidgetItem *)), this,
            SLOT(listWidgetCliked(QListWidgetItem *)));

    /* slider信号槽连接 */
    connect(mDurationSlider, SIGNAL(sliderReleased()), this, SLOT(durationSliderReleased()));

    /* 失去焦点 */
    this->setFocus();
}

void MainWindow::musicLayout()
{
    /* 设置位置与大小,这里固定为800, 480 */
    this->setGeometry(0, 0, 800, 480);
    QPalette pal;

    /* 按钮 */
    for (int i = 0; i < 7; i++)
        mPushButton[i] = new QPushButton();

    /* 标签 */
    for (int i = 0; i < 4; i++)
        mLabel[i] = new QLabel();

    for (int i = 0; i < 3; i++) {
        /* 垂直容器 */
        mVWidget[i] = new QWidget();
        mVWidget[i]->setAutoFillBackground(true);
        /* 垂直布局 */
        mVBoxLayout[i] = new QVBoxLayout();
    }

    for (int i = 0; i < 4; i++) {
        /* 水平容器 */
        mHWidget[i] = new QWidget();
        mHWidget[i]->setAutoFillBackground(true);
        /* 水平布局 */
        mHBoxLayout[i] = new QHBoxLayout();
    }

    /* 播放进度条 */
    mDurationSlider = new QSlider(Qt::Horizontal);
    mDurationSlider->setMinimumSize(300, 15);
    mDurationSlider->setMaximumHeight(15);
    mDurationSlider->setObjectName("mDurationSlider");

    /* 音乐列表 */
    mListWidget = new QListWidget();
    mListWidget->setObjectName("mListWidget");
    mListWidget->resize(310, 265);
    mListWidget->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    mListWidget->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    /* 列表遮罩 */
    mListMask = new QWidget(mListWidget);
    mListMask->setMinimumSize(310, 50);
    mListMask->setMinimumHeight(50);
    mListMask->setObjectName("mListMask");
    mListMask->setGeometry(0, mListWidget->height() - 50, 310, 50);

    /* 设置对象名称 */
    mPushButton[0]->setObjectName("btn_previous");
    mPushButton[1]->setObjectName("btn_play");
    mPushButton[2]->setObjectName("btn_next");
    mPushButton[3]->setObjectName("btn_favorite");
    mPushButton[4]->setObjectName("btn_mode");
    mPushButton[5]->setObjectName("btn_menu");
    mPushButton[6]->setObjectName("btn_volume");

    /* 设置按钮属性 */
    mPushButton[1]->setCheckable(true);
    mPushButton[3]->setCheckable(true);

    /* H0布局 */
    mVWidget[0]->setMinimumSize(310, 480);
    mVWidget[0]->setMaximumWidth(310);
    mVWidget[1]->setMinimumSize(320, 480);
    QSpacerItem *hSpacer0 = new QSpacerItem(70, 480, QSizePolicy::Minimum, QSizePolicy::Maximum);

    QSpacerItem *hSpacer1 = new QSpacerItem(65, 480, QSizePolicy::Minimum, QSizePolicy::Maximum);

    QSpacerItem *hSpacer2 = new QSpacerItem(60, 480, QSizePolicy::Minimum, QSizePolicy::Maximum);

    mHBoxLayout[0]->addSpacerItem(hSpacer0);
    mHBoxLayout[0]->addWidget(mVWidget[0]);
    mHBoxLayout[0]->addSpacerItem(hSpacer1);
    mHBoxLayout[0]->addWidget(mVWidget[1]);
    mHBoxLayout[0]->addSpacerItem(hSpacer2);
    mHBoxLayout[0]->setContentsMargins(0, 0, 0, 0);

    mHWidget[0]->setLayout(mHBoxLayout[0]);
    setCentralWidget(mHWidget[0]);

    /* V0布局 */
    mListWidget->setMinimumSize(310, 265);
    mListWidget->setFont(QFont("ANSI"));
    mHWidget[1]->setMinimumSize(310, 80);
    mHWidget[1]->setMaximumHeight(80);
    mLabel[0]->setMinimumSize(310, 95);
    mLabel[0]->setMaximumHeight(95);
    QSpacerItem *vSpacer0 = new QSpacerItem(310, 10, QSizePolicy::Minimum, QSizePolicy::Maximum);
    QSpacerItem *vSpacer1 = new QSpacerItem(310, 30, QSizePolicy::Minimum, QSizePolicy::Minimum);
    mVBoxLayout[0]->addWidget(mLabel[0]);
    mVBoxLayout[0]->addWidget(mListWidget);
    mVBoxLayout[0]->addSpacerItem(vSpacer0);
    mVBoxLayout[0]->addWidget(mHWidget[1]);
    mVBoxLayout[0]->addSpacerItem(vSpacer1);
    mVBoxLayout[0]->setContentsMargins(0, 0, 0, 0);

    mVWidget[0]->setLayout(mVBoxLayout[0]);

    /* H1布局 */
    for (int i = 0; i < 3; i++) {
        mPushButton[i]->setMinimumSize(80, 80);
    }
    QSpacerItem *hSpacer3 = new QSpacerItem(40, 80, QSizePolicy::Expanding, QSizePolicy::Expanding);
    QSpacerItem *hSpacer4 = new QSpacerItem(40, 80, QSizePolicy::Expanding, QSizePolicy::Expanding);
    mHBoxLayout[1]->addWidget(mPushButton[0]);
    mHBoxLayout[1]->addSpacerItem(hSpacer3);
    mHBoxLayout[1]->addWidget(mPushButton[1]);
    mHBoxLayout[1]->addSpacerItem(hSpacer4);
    mHBoxLayout[1]->addWidget(mPushButton[2]);
    mHBoxLayout[1]->setContentsMargins(0, 0, 0, 0);

    mHWidget[1]->setLayout(mHBoxLayout[1]);

    /* V1布局 */
    QSpacerItem *vSpacer2 = new QSpacerItem(320, 40, QSizePolicy::Minimum, QSizePolicy::Maximum);
    QSpacerItem *vSpacer3 = new QSpacerItem(320, 20, QSizePolicy::Minimum, QSizePolicy::Maximum);
    QSpacerItem *vSpacer4 = new QSpacerItem(320, 30, QSizePolicy::Minimum, QSizePolicy::Minimum);
    mLabel[1]->setMinimumSize(320, 320);
    QImage Image;
    Image.load(":/images/cd.png");
    QPixmap pixmap = QPixmap::fromImage(Image);
    int with       = 320;
    int height     = 320;
    QPixmap fitpixmap
        = pixmap.scaled(with, height, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    mLabel[1]->setPixmap(fitpixmap);
    mLabel[1]->setAlignment(Qt::AlignCenter);
    mVWidget[2]->setMinimumSize(300, 80);
    mVWidget[2]->setMaximumHeight(80);
    mVBoxLayout[1]->addSpacerItem(vSpacer2);
    mVBoxLayout[1]->addWidget(mLabel[1]);
    mVBoxLayout[1]->addSpacerItem(vSpacer3);
    mVBoxLayout[1]->addWidget(mDurationSlider);
    mVBoxLayout[1]->addWidget(mVWidget[2]);
    mVBoxLayout[1]->addSpacerItem(vSpacer4);
    mVBoxLayout[1]->setContentsMargins(0, 0, 0, 0);

    mVWidget[1]->setLayout(mVBoxLayout[1]);

    /* V2布局 */
    QSpacerItem *vSpacer5 = new QSpacerItem(300, 10, QSizePolicy::Minimum, QSizePolicy::Maximum);
    mHWidget[2]->setMinimumSize(320, 20);
    mHWidget[3]->setMinimumSize(320, 60);
    mVBoxLayout[2]->addWidget(mHWidget[2]);
    mVBoxLayout[2]->addSpacerItem(vSpacer5);
    mVBoxLayout[2]->addWidget(mHWidget[3]);
    mVBoxLayout[2]->setContentsMargins(0, 0, 0, 0);

    mVWidget[2]->setLayout(mVBoxLayout[2]);

    /* H2布局 */
    QFont font;

    font.setPixelSize(10);

    /* 设置标签文本 */
    mLabel[0]->setText("Q Music，Enjoy it！");
    mLabel[2]->setText("00:00");
    mLabel[3]->setText("00:00");
    mLabel[2]->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    mLabel[3]->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    mLabel[3]->setAlignment(Qt::AlignRight);
    mLabel[2]->setAlignment(Qt::AlignLeft);
    mLabel[2]->setFont(font);
    mLabel[3]->setFont(font);

    pal.setColor(QPalette::WindowText, Qt::white);
    mLabel[0]->setPalette(pal);
    mLabel[2]->setPalette(pal);
    mLabel[3]->setPalette(pal);

    mHBoxLayout[2]->addWidget(mLabel[2]);
    mHBoxLayout[2]->addWidget(mLabel[3]);

    mHBoxLayout[2]->setContentsMargins(0, 0, 0, 0);
    mHWidget[2]->setLayout(mHBoxLayout[2]);

    /* H3布局 */
    QSpacerItem *hSpacer5 = new QSpacerItem(0, 60, QSizePolicy::Minimum, QSizePolicy::Maximum);
    QSpacerItem *hSpacer6 = new QSpacerItem(80, 60, QSizePolicy::Maximum, QSizePolicy::Maximum);
    QSpacerItem *hSpacer7 = new QSpacerItem(80, 60, QSizePolicy::Maximum, QSizePolicy::Maximum);
    QSpacerItem *hSpacer8 = new QSpacerItem(80, 60, QSizePolicy::Maximum, QSizePolicy::Maximum);
    QSpacerItem *hSpacer9 = new QSpacerItem(0, 60, QSizePolicy::Minimum, QSizePolicy::Maximum);

    for (int i = 3; i < 7; i++) {
        mPushButton[i]->setMinimumSize(25, 25);
        mPushButton[i]->setMaximumSize(25, 25);
    }

    mHBoxLayout[3]->addSpacerItem(hSpacer5);
    mHBoxLayout[3]->addWidget(mPushButton[3]);
    mHBoxLayout[3]->addSpacerItem(hSpacer6);
    mHBoxLayout[3]->addWidget(mPushButton[4]);
    mHBoxLayout[3]->addSpacerItem(hSpacer7);
    mHBoxLayout[3]->addWidget(mPushButton[5]);
    mHBoxLayout[3]->addSpacerItem(hSpacer8);
    mHBoxLayout[3]->addWidget(mPushButton[6]);
    mHBoxLayout[3]->addSpacerItem(hSpacer9);
    mHBoxLayout[3]->setContentsMargins(0, 0, 0, 0);
    mHBoxLayout[3]->setAlignment(Qt::AlignHCenter);

    mHWidget[3]->setLayout(mHBoxLayout[3]);

    // mHWidget[0]->setStyleSheet("background-color:red");
    // mHWidget[1]->setStyleSheet("background-color:#ff5599");
    // mHWidget[2]->setStyleSheet("background-color:#ff55ff");
    // mHWidget[3]->setStyleSheet("background-color:black");
    // mVWidget[0]->setStyleSheet("background-color:#555555");
    // mVWidget[1]->setStyleSheet("background-color:green");
    // mVWidget[2]->setStyleSheet("background-color:gray");
}

MainWindow::~MainWindow() { }

void MainWindow::btn_play_clicked()
{
    MusicPlayerState state = mMusicPlayer->state();
    switch (state) {
    case MusicPlayerState::StoppedState:
        /* 媒体播放 */
        mMusicPlayer->play();
        break;

    case MusicPlayerState::PlayingState:
        /* 媒体暂停 */
        mMusicPlayer->pause();
        break;

    case MusicPlayerState::PausedState:
        mMusicPlayer->play();
        break;
    }
}

void MainWindow::btn_next_clicked()
{
    mMusicPlayer->stop();
    int count = mMusicPlayer->getMusicCount();
    if (0 == count)
        return;

    /* 列表下一个 */
    mMusicPlayer->next();
    mMusicPlayer->play();
}

void MainWindow::btn_previous_clicked()
{
    mMusicPlayer->stop();
    int count = mMusicPlayer->getMusicCount();
    if (0 == count)
        return;

    /* 列表上一个 */
    mMusicPlayer->previous();
    mMusicPlayer->play();
}

void MainWindow::btn_favorite_clicked()
{
    // TODO:
}

void MainWindow::btn_playMode_clicked()
{
    // TODO:
}

void MainWindow::btn_playList_clicked()
{
    // TODO:
}

void MainWindow::btn_volume_clicked()
{
    // TODO:
}

void MainWindow::listWidgetCliked(QListWidgetItem *item)
{
    qDebug() << "listWidgetCliked:" << mListWidget->row(item) << endl;
    mMusicPlayer->stop();
    mMusicPlayer->setCurrentIndex(mListWidget->row(item));
    mMusicPlayer->play();
}

void MainWindow::resizeEvent(QResizeEvent *event)
{
    Q_UNUSED(event);
    mListMask->setGeometry(0, mListWidget->height() - 50, 310, 50);
}

void MainWindow::durationSliderReleased()
{
    /* 设置媒体播放的位置 */
    qDebug() << "durationSliderReleased:" << mDurationSlider->value() << endl;
    mMusicPlayer->setPosition(mDurationSlider->value() * 1000);
}

void MainWindow::scanSongs()
{
    QDir dir(QCoreApplication::applicationDirPath() + "/../../music");
    QDir dirbsolutePath(dir.absolutePath());
    /* 如果目录存在 */
    if (dirbsolutePath.exists()) {
        mMusicPlayer->addMusicDir(dirbsolutePath.absolutePath().toStdString());
    } else {
        qDebug() << "dir not exist" << endl;
        qDebug() << "dir is" << QCoreApplication::applicationDirPath() << endl;
    }
}

void MainWindow::mediaPlayerInit()
{
    mAppDir = QCoreApplication::applicationDirPath().toStdString();
#ifdef _WIN32
    mLogDir = mAppDir + "/log";
#else
#define TONAME1(x) #x
#define TONAME(x) TONAME1(x)
    mLogDir = "/var/log/";
    mLogDir += TONAME(EXE_NAME);
#endif // _WIN32
    QDir dir(mLogDir.c_str());
    if (!dir.exists())
        dir.mkdir(mLogDir.c_str());
    mMusicPlayer = std::make_shared<MusicPlayer>(this, mLogDir);
    /* 确保列表是空的 */
    // mediaPlaylist->clear();
    /* 设置音乐播放器的列表为mediaPlaylist */
    // mMusicPlayer->setPlaylist(mediaPlaylist);
    /* 设置播放模式，Loop是列循环 */
    // mMusicPlayer->setPlaybackMode(QMediaPlaylist::Loop);
}

void MainWindow::onMusicPlayerStateChanged(MusicPlayerState state)
{
    if (mState == state)
        return;
    switch (state) {
    case sdk::MusicPlayerState::StoppedState:
        mPushButton[1]->setChecked(false);
        break;

    case sdk::MusicPlayerState::PlayingState:
        mPushButton[1]->setChecked(true);
        break;

    case sdk::MusicPlayerState::PausedState:
        mPushButton[1]->setChecked(false);
        break;
    }
    mState = state;
}

void MainWindow::onMusicPlayerListCurrentIndexChanged(int index)
{
    if (-1 == index)
        return;
    qDebug() << "onListCurIndex:" << index << endl;
    /* 设置列表正在播放的项 */
    mListWidget->setCurrentRow(index);
}

void MainWindow::onMusicPlayerDurationChanged(uint64_t duration)
{
    qDebug() << "duration" << duration << endl;
    mDurationSlider->setRange(0, duration / 1000);
    int second  = duration / 1000;
    int minute  = second / 60;
    second     %= 60;

    QString mediaDuration;
    mediaDuration.clear();

    if (minute >= 10)
        mediaDuration = QString::number(minute, 10);
    else
        mediaDuration = "0" + QString::number(minute, 10);

    if (second >= 10)
        mediaDuration = mediaDuration + ":" + QString::number(second, 10);
    else
        mediaDuration = mediaDuration + ":0" + QString::number(second, 10);

    /* 显示媒体总长度时间 */
    mLabel[3]->setText(mediaDuration);
}

void MainWindow::onMusicPlayerPositionChanged(uint64_t position)
{
    if (!mDurationSlider->isSliderDown())
        mDurationSlider->setValue(position / 1000);

    int second  = position / 1000;
    int minute  = second / 60;
    second     %= 60;

    QString mediaPosition;
    mediaPosition.clear();

    if (minute >= 10)
        mediaPosition = QString::number(minute, 10);
    else
        mediaPosition = "0" + QString::number(minute, 10);

    if (second >= 10)
        mediaPosition = mediaPosition + ":" + QString::number(second, 10);
    else
        mediaPosition = mediaPosition + ":0" + QString::number(second, 10);

    /* 显示现在播放的时间 */
    mLabel[2]->setText(mediaPosition);
}

void MainWindow::onMusicPlayerMusicListChanged(std::list<MusicIndex> list)
{
    mListWidget->clear();
    for (auto &index : list) {
        std::string name = QString::fromLocal8Bit(index.name.data()).toUtf8().data();
        mListWidget->addItem(QString::fromStdString(name));
        qDebug() << "onMusicList: " << index.index << " " << name.c_str();
    }
}
