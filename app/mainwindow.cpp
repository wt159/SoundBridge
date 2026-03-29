/******************************************************************
Copyright (c) Deng Zhimao Co., Ltd. 1990-2021. All rights reserved.
* @projectName   14_musicplayer
* @brief         mainwindow.cpp
* @author        Deng Zhimao
* @email         1252699831@qq.com
* @net           www.openedv.com
* @date          2021-04-20
*******************************************************************/
#include "mainwindow.h"
#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfoList>
#include <QGuiApplication>
#include <QMessageBox>
#include <QPainter>
#include <QScreen>
#include <QSettings>
#include <QStyledItemDelegate>
#include <QThread>

namespace {
constexpr const char *kTag = "MainWindow";
constexpr int kRolePlaying = Qt::UserRole + 1;

PlaybackMode sanitizePlaybackMode(int rawMode)
{
    switch (static_cast<PlaybackMode>(rawMode)) {
    case PlaybackMode::Sequential:
    case PlaybackMode::Loop:
    case PlaybackMode::Random:
    case PlaybackMode::SingleOnce:
    case PlaybackMode::SingleLoop:
        return static_cast<PlaybackMode>(rawMode);
    }
    return PlaybackMode::Sequential;
}

class MusicItemDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override
    {
        QStyleOptionViewItem opt(option);
        initStyleOption(&opt, index);

        const bool isSelected = (opt.state & QStyle::State_Selected);
        const bool isPlaying  = index.data(kRolePlaying).toBool();

        painter->save();

        if (isSelected || isPlaying) {
            if (isPlaying) {
                const QColor bg = isSelected ? MainWindow::kPlayBgSelected : MainWindow::kPlayBg;
                QRect bar(opt.rect.left(), opt.rect.top(), 3, opt.rect.height());
                painter->fillRect(bar, MainWindow::kPlayBar);
                painter->fillRect(opt.rect, bg);
            } else {
                painter->fillRect(opt.rect, MainWindow::kSelectBg);
            }
        }

        opt.state           &= ~QStyle::State_Selected;
        opt.backgroundBrush  = Qt::NoBrush;

        QStyle *style = opt.widget ? opt.widget->style() : QApplication::style();
        style->drawControl(QStyle::CE_ItemViewItem, &opt, painter, opt.widget);

        painter->restore();
    }
};
}

QString MainWindow::playbackModeText(PlaybackMode mode) const
{
    switch (mode) {
    case PlaybackMode::Sequential:
        return "Sequential";
    case PlaybackMode::Loop:
        return "Loop";
    case PlaybackMode::Random:
        return "Random";
    case PlaybackMode::SingleOnce:
        return "Single Once";
    case PlaybackMode::SingleLoop:
        return "Single Loop";
    }
    return "Sequential";
}

void MainWindow::updatePlaybackModeButton(PlaybackMode mode)
{
    const QString modeText = playbackModeText(mode);
    if (mPushButton[4] != nullptr) {
        const QString tip = "Playback mode: " + modeText;
        mPushButton[4]->setToolTip(tip);
        mPushButton[4]->setStatusTip(tip);
        mPushButton[4]->setAccessibleDescription(tip);
    }
}

const QColor MainWindow::kSelectBg(94, 220, 243, 48);
const QColor MainWindow::kPlayBg(94, 220, 243, 70);
const QColor MainWindow::kPlayBgSelected(94, 220, 243, 90);
const QColor MainWindow::kPlayBar(94, 220, 243, 200);

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    /* Initialize UI layout */
    musicLayout();
    applyWindowPolicy();

    /* Initialize media player */
    mediaPlayerInit();

    restoreWindowState();

    /* Scan songs */
    scanSongs();

    /* Connect button signals */
    connect(mPushButton[0], SIGNAL(clicked()), this, SLOT(btn_previous_clicked()));
    connect(mPushButton[1], SIGNAL(clicked()), this, SLOT(btn_play_clicked()));
    connect(mPushButton[2], SIGNAL(clicked()), this, SLOT(btn_next_clicked()));
    connect(mPushButton[3], SIGNAL(clicked()), this, SLOT(btn_favorite_clicked()));
    connect(mPushButton[4], SIGNAL(clicked()), this, SLOT(btn_playMode_clicked()));
    connect(mPushButton[5], SIGNAL(clicked()), this, SLOT(btn_playList_clicked()));
    connect(mPushButton[6], SIGNAL(clicked()), this, SLOT(btn_volume_clicked()));

    /* List signals */
    connect(mListWidget, SIGNAL(itemClicked(QListWidgetItem *)), this,
            SLOT(listWidgetCliked(QListWidgetItem *)));

    /* Slider signals */
    connect(mDurationSlider, SIGNAL(sliderReleased()), this, SLOT(durationSliderReleased()));
    connect(mAutoSkipCheck, SIGNAL(toggled(bool)), this, SLOT(autoSkipToggled(bool)));
    connect(mController.get(), SIGNAL(viewStateChanged(PlayerViewState)), this,
            SLOT(renderPlayerViewState(PlayerViewState)));
    connect(mController.get(), SIGNAL(errorOccurred(int, QString, int, QString, QString, bool)),
            this, SLOT(handlePlayerError(int, QString, int, QString, QString, bool)));

    /* Remove focus */
    this->setFocus();
    renderPlayerViewState(mController->viewState());
}

void MainWindow::musicLayout()
{
    QPalette pal;

    /* Buttons */
    for (int i = 0; i < 7; i++)
        mPushButton[i] = new QPushButton();

    /* Labels */
    for (int i = 0; i < 4; i++)
        mLabel[i] = new QLabel();
    mNowPlayingTitle    = new QLabel();
    mNowPlayingSubtitle = new QLabel();
    mAutoSkipCheck      = new QCheckBox("Auto skip on error");
    mAutoSkipCheck->setObjectName("mAutoSkipCheck");

    for (int i = 0; i < 3; i++) {
        /* Vertical containers */
        mVWidget[i] = new QWidget();
        mVWidget[i]->setAutoFillBackground(true);
        /* Vertical layouts */
        mVBoxLayout[i] = new QVBoxLayout();
    }

    for (int i = 0; i < 4; i++) {
        /* Horizontal containers */
        mHWidget[i] = new QWidget();
        mHWidget[i]->setAutoFillBackground(true);
        /* Horizontal layouts */
        mHBoxLayout[i] = new QHBoxLayout();
    }

    /* Playback progress */
    mDurationSlider = new QSlider(Qt::Horizontal);
    mDurationSlider->setMinimumSize(300, 15);
    mDurationSlider->setMaximumHeight(15);
    mDurationSlider->setObjectName("mDurationSlider");

    /* Music list */
    mListWidget = new QListWidget();
    mListWidget->setObjectName("mListWidget");
    mListWidget->resize(310, 265);
    mListWidget->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    mListWidget->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    mListWidget->setSelectionMode(QAbstractItemView::SingleSelection);
    mListWidget->setSelectionBehavior(QAbstractItemView::SelectRows);
    mListWidget->setItemDelegate(new MusicItemDelegate(mListWidget));
    {
        QFont listFont = mListWidget->font();
        listFont.setPixelSize(16);
        listFont.setWeight(QFont::Medium);
        mListWidget->setFont(listFont);
        mListWidget->setSpacing(0);
        mListWidget->setUniformItemSizes(true);
        mListWidget->setTextElideMode(Qt::ElideRight);
        const int rowHeight = QFontMetrics(listFont).height() + 10;
        mListWidget->setGridSize(QSize(0, rowHeight));
        mListWidget->setStyleSheet(
            "QListWidget{background:transparent;border:1px solid rgba(255,255,255,32);}"
            "QListWidget::item{border-right:none;}"
            "QListWidget::item{padding:4px 8px;border-bottom:1px solid rgba(255,255,255,16);}"
            "QListWidget::item:hover{background:rgba(255,255,255,8);}"
            "QScrollBar:vertical{background:transparent;width:6px;margin:2px 2px 2px 0;}"
            "QScrollBar::handle:vertical{background:rgba(255,255,255,60);border-radius:3px;min-"
            "height:24px;}"
            "QScrollBar::add-line:vertical,QScrollBar::sub-line:vertical{height:0px;}"
            "QScrollBar::add-page:vertical,QScrollBar::sub-page:vertical{background:transparent;}");
    }

    /* List mask overlay */
    mListMask = new QWidget(mListWidget->viewport());
    mListMask->setMinimumSize(310, 50);
    mListMask->setMinimumHeight(50);
    mListMask->setObjectName("mListMask");
    mListMask->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    mListMask->hide();
    const int maskInset  = 1;
    const int maskHeight = 0;
    mListMask->setGeometry(maskInset, mListWidget->viewport()->height() - maskInset,
                           mListWidget->viewport()->width() - maskInset * 2, maskHeight);

    /* Set object names */
    mPushButton[0]->setObjectName("btn_previous");
    mPushButton[1]->setObjectName("btn_play");
    mPushButton[2]->setObjectName("btn_next");
    mPushButton[3]->setObjectName("btn_favorite");
    mPushButton[4]->setObjectName("btn_mode");
    mPushButton[5]->setObjectName("btn_menu");
    mPushButton[6]->setObjectName("btn_volume");

    /* Set button state */
    mPushButton[1]->setCheckable(true);
    mPushButton[3]->setCheckable(true);

    /* H0 layout */
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

    /* V0 layout */
    mListWidget->setMinimumSize(310, 265);
    mHWidget[1]->setMinimumSize(310, 80);
    mHWidget[1]->setMaximumHeight(80);
    mLabel[0]->setMinimumSize(310, 95);
    mLabel[0]->setMaximumHeight(95);
    mNowPlayingTitle->setMinimumSize(310, 28);
    mNowPlayingSubtitle->setMinimumSize(310, 18);
    mNowPlayingTitle->setMaximumHeight(28);
    mNowPlayingSubtitle->setMaximumHeight(18);
    QSpacerItem *vSpacer0 = new QSpacerItem(310, 10, QSizePolicy::Minimum, QSizePolicy::Maximum);
    QSpacerItem *vSpacer1 = new QSpacerItem(310, 30, QSizePolicy::Minimum, QSizePolicy::Minimum);
    mVBoxLayout[0]->addWidget(mLabel[0]);
    mVBoxLayout[0]->addWidget(mNowPlayingSubtitle);
    mVBoxLayout[0]->addWidget(mNowPlayingTitle);
    mVBoxLayout[0]->addWidget(mListWidget);
    mVBoxLayout[0]->addSpacerItem(vSpacer0);
    mVBoxLayout[0]->addWidget(mAutoSkipCheck);
    mVBoxLayout[0]->addWidget(mHWidget[1]);
    mVBoxLayout[0]->addSpacerItem(vSpacer1);
    mVBoxLayout[0]->setContentsMargins(0, 0, 0, 0);

    mVWidget[0]->setLayout(mVBoxLayout[0]);

    /* H1 layout */
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

    /* V1 layout */
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

    /* V2 layout */
    QSpacerItem *vSpacer5 = new QSpacerItem(300, 10, QSizePolicy::Minimum, QSizePolicy::Maximum);
    mHWidget[2]->setMinimumSize(320, 20);
    mHWidget[3]->setMinimumSize(320, 60);
    mVBoxLayout[2]->addWidget(mHWidget[2]);
    mVBoxLayout[2]->addSpacerItem(vSpacer5);
    mVBoxLayout[2]->addWidget(mHWidget[3]);
    mVBoxLayout[2]->setContentsMargins(0, 0, 0, 0);

    mVWidget[2]->setLayout(mVBoxLayout[2]);

    /* H2 layout */
    QFont font;

    font.setPixelSize(10);

    /* Set label text */
    mLabel[0]->setText("SoundBridge");
    mNowPlayingSubtitle->setText("NOW PLAYING");
    mNowPlayingTitle->setText("—");
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
    mNowPlayingTitle->setPalette(pal);
    QPalette subPal = pal;
    subPal.setColor(QPalette::WindowText, QColor(255, 255, 255, 160));
    mNowPlayingSubtitle->setPalette(subPal);

    QFont headerFont;
    headerFont.setPixelSize(12);
    headerFont.setWeight(QFont::Medium);
    headerFont.setLetterSpacing(QFont::AbsoluteSpacing, 0.6);
    mLabel[0]->setFont(headerFont);
    mLabel[0]->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    mLabel[0]->setContentsMargins(0, 0, 0, 6);

    QFont nowTitleFont;
    nowTitleFont.setPixelSize(18);
    nowTitleFont.setWeight(QFont::DemiBold);
    mNowPlayingTitle->setFont(nowTitleFont);
    QFont nowSubFont;
    nowSubFont.setPixelSize(10);
    nowSubFont.setWeight(QFont::Medium);
    mNowPlayingSubtitle->setFont(nowSubFont);
    mNowPlayingSubtitle->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    mNowPlayingTitle->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    mNowPlayingSubtitle->setContentsMargins(0, 2, 0, 0);
    mNowPlayingTitle->setContentsMargins(0, 0, 0, 8);

    QFont autoSkipFont = mAutoSkipCheck->font();
    autoSkipFont.setPixelSize(10);
    mAutoSkipCheck->setFont(autoSkipFont);
    mAutoSkipCheck->setChecked(true);
    mAutoSkipCheck->setStyleSheet("QCheckBox{color:rgba(255,255,255,180);}");
    updatePlaybackModeButton(PlaybackMode::Sequential);

    mHBoxLayout[2]->addWidget(mLabel[2]);
    mHBoxLayout[2]->addWidget(mLabel[3]);

    mHBoxLayout[2]->setContentsMargins(0, 0, 0, 0);
    mHWidget[2]->setLayout(mHBoxLayout[2]);

    /* H3 layout */
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

    QTimer::singleShot(0, this, [this]() {
        const int maskInset  = 1;
        const int maskHeight = 0;
        mListMask->setGeometry(maskInset, mListWidget->viewport()->height() - maskInset,
                               mListWidget->viewport()->width() - maskInset * 2, maskHeight);
        mListWidget->doItemsLayout();
        mListWidget->viewport()->update();
    });
}

MainWindow::~MainWindow() { }
void MainWindow::applyWindowPolicy()
{
    setWindowFlags(Qt::Window);
    setWindowTitle("SoundBridge");
    setMinimumSize(960, 540);
    resize(1200, 720);
}

void MainWindow::restoreWindowState()
{
    QSettings settings;
    const QByteArray geometry       = settings.value("window/geometry").toByteArray();
    const bool maximized            = settings.value("window/maximized", false).toBool();
    const bool autoSkipOnError      = settings.value("playback/auto_skip_on_error", true).toBool();
    const PlaybackMode playbackMode = sanitizePlaybackMode(
        settings.value("playback/mode", static_cast<int>(PlaybackMode::Sequential)).toInt());
    settings.setValue("playback/mode", static_cast<int>(playbackMode));
    if (mAutoSkipCheck != nullptr) {
        mAutoSkipCheck->setChecked(autoSkipOnError);
    }
    mController->setAutoSkipOnError(autoSkipOnError);
    mController->setPlaybackMode(playbackMode);
    updatePlaybackModeButton(playbackMode);

    if (!geometry.isEmpty()) {
        restoreGeometry(geometry);
    } else {
        QScreen *screen = QGuiApplication::primaryScreen();
        if (screen != nullptr) {
            const QRect area = screen->availableGeometry();
            move(area.center() - rect().center());
        }
    }

    if (maximized) {
        setWindowState(windowState() | Qt::WindowMaximized);
    }
}

void MainWindow::persistWindowState()
{
    QSettings settings;
    settings.setValue("window/geometry", saveGeometry());
    settings.setValue("window/maximized", isMaximized());
    settings.setValue("playback/auto_skip_on_error", mController->autoSkipOnError());
    settings.setValue("playback/mode", static_cast<int>(mController->playbackMode()));
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    persistWindowState();
    QMainWindow::closeEvent(event);
}

void MainWindow::btn_play_clicked()
{
    mController->playPause();
}

void MainWindow::btn_next_clicked()
{
    mController->nextTrack();
}

void MainWindow::btn_previous_clicked()
{
    mController->previousTrack();
}

void MainWindow::btn_favorite_clicked()
{
    // TODO:
}

void MainWindow::btn_playMode_clicked()
{
    PlaybackMode nextMode = mController->playbackMode();
    switch (nextMode) {
    case PlaybackMode::Sequential:
        nextMode = PlaybackMode::Loop;
        break;
    case PlaybackMode::Loop:
        nextMode = PlaybackMode::Random;
        break;
    case PlaybackMode::Random:
        nextMode = PlaybackMode::SingleOnce;
        break;
    case PlaybackMode::SingleOnce:
        nextMode = PlaybackMode::SingleLoop;
        break;
    case PlaybackMode::SingleLoop:
        nextMode = PlaybackMode::Sequential;
        break;
    }
    mController->setPlaybackMode(nextMode);
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
    const int clickedIndex = mListWidget->row(item);
    int selectedCount      = 0;
    if (mListWidget && mListWidget->selectionModel()) {
        selectedCount = mListWidget->selectionModel()->selectedIndexes().size();
    }
    LogPrintf(LogLevel::Debug, kTag, "thread listWidgetCliked cur=%p ui=%p",
              QThread::currentThread(), qApp ? qApp->thread() : nullptr);
    LogPrintf(LogLevel::Debug, kTag,
              "listWidgetCliked idx=%d playing=%d selectedCount=%d itemSelected=%d itemPlaying=%d",
              clickedIndex, mController->currentTrackIndex(), selectedCount,
              item ? item->isSelected() : -1, item ? item->data(kRolePlaying).toBool() : -1);
    mController->selectTrack(clickedIndex);
}

void MainWindow::resizeEvent(QResizeEvent *event)
{
    Q_UNUSED(event);
    const int maskInset  = 1;
    const int maskHeight = 0;
    mListMask->setGeometry(maskInset, mListWidget->viewport()->height() - maskInset,
                           mListWidget->viewport()->width() - maskInset * 2, maskHeight);
}

void MainWindow::durationSliderReleased()
{
    /* Seek playback to slider position */
    LogPrintf(LogLevel::Debug, kTag, "durationSliderReleased: %d", mDurationSlider->value());
    mController->seekSeconds(mDurationSlider->value());
}

void MainWindow::autoSkipToggled(bool checked)
{
    mController->setAutoSkipOnError(checked);
}

void MainWindow::scanSongs()
{
    QDir dir(QCoreApplication::applicationDirPath() + "/../../music");
    QDir dirbsolutePath(dir.absolutePath());
    if (dirbsolutePath.exists()) {
        mController->addMusicDirectory(dirbsolutePath.absolutePath().toUtf8().constData());
    } else {
        LogMessage(LogLevel::Warning, kTag, "dir not exist");
        LogPrintf(LogLevel::Warning, kTag, "dir is %s",
                  QCoreApplication::applicationDirPath().toUtf8().constData());
    }
}

void MainWindow::mediaPlayerInit()
{
    mAppDir = QCoreApplication::applicationDirPath().toStdString();
#ifdef _WIN32
    mLogDir = mAppDir + "/log";
#else
#define TONAME1(x) #x
#define TONAME(x)  TONAME1(x)
    mLogDir  = "/var/log/";
    mLogDir += TONAME(EXE_NAME);
#endif // _WIN32
    QDir dir(mLogDir.c_str());
    if (!dir.exists())
        dir.mkdir(mLogDir.c_str());

    PlayerConfig config;
    config.logDirectory    = mLogDir;
    config.autoSkipOnError = true;
    mController.reset(new PlayerController(config));
    updatePlaybackModeButton(mController->playbackMode());
}

void MainWindow::renderPlayerViewState(PlayerViewState viewState)
{
    const PlayerState state = static_cast<PlayerState>(viewState.playerState);
    const PlaybackMode mode = static_cast<PlaybackMode>(viewState.playbackMode);
    const int index         = viewState.currentTrackIndex;

    LogPrintf(LogLevel::Debug, kTag, "thread renderPlayerViewState cur=%p ui=%p",
              QThread::currentThread(), qApp ? qApp->thread() : nullptr);
    LogPrintf(LogLevel::Debug, kTag, "render state=%d playing=%d", static_cast<int>(state), index);

    switch (state) {
    case PlayerState::Stopped:
        mPushButton[1]->setChecked(false);
        break;
    case PlayerState::Playing:
        mPushButton[1]->setChecked(true);
        break;
    case PlayerState::Paused:
        mPushButton[1]->setChecked(false);
        break;
    }

    updatePlaybackModeButton(mode);
    if (mAutoSkipCheck != nullptr && mAutoSkipCheck->isChecked() != viewState.autoSkipOnError) {
        mAutoSkipCheck->blockSignals(true);
        mAutoSkipCheck->setChecked(viewState.autoSkipOnError);
        mAutoSkipCheck->blockSignals(false);
    }
    mNowPlayingSubtitle->setText(viewState.subtitle);

    const bool playlistChanged = (mRenderedPlaylist != viewState.playlist);
    const bool trackChanged    = (mRenderedTrackIndex != index);
    if (playlistChanged) {
        mListWidget->setUpdatesEnabled(false);
        mListWidget->clear();
        for (int i = 0; i < viewState.playlist.size(); ++i) {
            const QString &name = viewState.playlist.at(i);
            mListWidget->addItem(name);
            LogPrintf(LogLevel::Debug, kTag, "onPlaylist: %d %s", i, name.toUtf8().constData());
        }
        mRenderedPlaylist = viewState.playlist;
    }

    if (playlistChanged || trackChanged) {
        for (int i = 0; i < mListWidget->count(); ++i) {
            QListWidgetItem *playlistItem = mListWidget->item(i);
            if (playlistItem != nullptr) {
                playlistItem->setData(kRolePlaying, false);
            }
        }
        if (mListWidget->selectionModel() != nullptr) {
            mListWidget->selectionModel()->clearSelection();
        }
        if (index >= 0 && index < mListWidget->count()) {
            QListWidgetItem *item = mListWidget->item(index);
            if (item != nullptr) {
                item->setData(kRolePlaying, true);
                mListWidget->setCurrentItem(item);
                item->setSelected(true);
                if (trackChanged) {
                    mListWidget->scrollToItem(item, QAbstractItemView::PositionAtCenter);
                }
                mNowPlayingTitle->setText(viewState.title);
            }
            mListWidget->setCurrentRow(index, QItemSelectionModel::ClearAndSelect);
        }
        mRenderedTrackIndex = index;
    }

    if (playlistChanged) {
        mListWidget->setUpdatesEnabled(true);
        mListWidget->doItemsLayout();
        mListWidget->viewport()->update();
    } else if (trackChanged) {
        mListWidget->viewport()->update();
    }

    if (viewState.playlist.isEmpty() || index < 0 || index >= mListWidget->count()) {
        mNowPlayingTitle->setText(viewState.title);
    }

    const qulonglong durationMs = viewState.durationMs;
    mDurationSlider->setRange(0, durationMs / 1000);
    mLabel[3]->setText(viewState.formattedDuration);

    const qulonglong positionMs = viewState.positionMs;
    if (!mDurationSlider->isSliderDown()) {
        mDurationSlider->setValue(positionMs / 1000);
    }
    mLabel[2]->setText(viewState.formattedPosition);
}

void MainWindow::handlePlayerError(int codeValue, const QString &detail, int trackIndex,
                                   const QString &path, const QString &traceId,
                                   bool autoSkipEnabled)
{
    const ErrorCode code  = static_cast<ErrorCode>(codeValue);
    const ErrorInfo &info = GetErrorInfo(code);
    QString title         = "Playback Error";
    QString message;
    message += "Message: "
        + QString::fromUtf8(FormatError(code, std::string(detail.toUtf8().constData())).c_str())
        + "\n";
    message += "Module: " + QString::fromUtf8(ToString(info.module)) + "\n";
    message += "Severity: " + QString::fromUtf8(ToString(info.severity)) + "\n";
    message += "Action: " + QString::fromUtf8(ToString(info.action)) + "\n";
    if (trackIndex >= 0) {
        message += "Index: " + QString::number(trackIndex) + "\n";
    }
    if (!path.isEmpty()) {
        message += "File: " + path + "\n";
    }
    if (!traceId.isEmpty()) {
        message += "Trace: " + traceId + "\n";
    }

    if (autoSkipEnabled) {
        LogPrintf(LogLevel::Warning, kTag, "auto-skip error trace=%s: %s",
                  traceId.toUtf8().constData(), message.trimmed().toUtf8().constData());
        return;
    }

    QMessageBox::warning(this, title, message.trimmed());
}
