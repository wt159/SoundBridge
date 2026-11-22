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

// 收藏集合的落盘形态：QStringList。QSet 的迭代序不稳定，先排序再写，避免每次写盘
// 都产生一份"看起来变了"的值（规格 §6.1）。
QStringList favoritesToList(const QSet<QString> &favorites)
{
    QStringList list;
    for (const QString &id : favorites) {
        list.push_back(id);
    }
    list.sort();
    return list;
}

// 三个写盘点共用（规格 §6.1）：QSettings 写失败是静默的，只在 status() 上体现。
// 记 Warning 而非 Error——设置没存住不影响本次会话的播放。
void warnIfSettingsWriteFailed(const QSettings &settings)
{
    if (settings.status() != QSettings::NoError) {
        LogPrintf(LogLevel::Warning, kTag, "settings write failed status=%d",
                  static_cast<int>(settings.status()));
    }
}
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

MainWindow::MainWindow(const AppUiPorts &ports, QWidget *parent)
    : QMainWindow(parent)
{
    /* res.qrc 随 app_core 静态库链接（ADR 0017）；静态库里的资源需显式初始化，
       否则链接器可能丢弃未被引用的 qrc 目标文件。重复调用安全。 */
    Q_INIT_RESOURCE(res);

    /* Initialize UI layout */
    musicLayout();
    applyWindowPolicy();

    /* Initialize media player */
    mediaPlayerInit(ports.player);
    /* 信号源（ADR 0016）：生产 = mController；注入 = 替身的 QObject 面。
       接口本身不是 QObject，无法从 PlayerCommandPort* 交叉转型，由注入方显式给出。 */
    m_signalSource = (ports.player != nullptr) ? ports.playerSignals : mController.get();

    /* 曲库根（ADR 0016）：注入优先，缺省维持现状。 */
    mMusicDirectory = ports.musicDirectory;
    if (mMusicDirectory.isEmpty()) {
        mMusicDirectory = QCoreApplication::applicationDirPath() + "/../../music";
    }

    /* 浮层（ADR 0015）：与触发按钮同窗口，Qt::Popup 自带点外部关闭。 */
    mVolumePopup   = new VolumePopup(mPushButton[6]);
    mPlaylistPopup = new PlaylistPopup(mPushButton[5]);

    /* 音量写盘去抖（规格 §6.1）：滑杆拖动每像素一次 valueChanged，不能逐次写盘。 */
    mVolumeSaveTimer = new QTimer(this);
    mVolumeSaveTimer->setSingleShot(true);
    mVolumeSaveTimer->setInterval(400);
    connect(mVolumeSaveTimer, SIGNAL(timeout()), this, SLOT(persistVolumeState()));

    connect(mVolumePopup, SIGNAL(volumeChanged(int)), this, SLOT(volumeSliderChanged(int)));
    connect(mVolumePopup, SIGNAL(mutedChanged(bool)), this, SLOT(muteToggled(bool)));
    connect(mPlaylistPopup, SIGNAL(trackActivated(int)), this, SLOT(panelTrackActivated(int)));
    connect(mPlaylistPopup, SIGNAL(trackRemoveRequested(int)), this,
            SLOT(panelTrackRemoveRequested(int)));
    connect(mPlaylistPopup, SIGNAL(clearPlaylistRequested()), this,
            SLOT(panelClearPlaylistRequested()));

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
    if (m_signalSource != nullptr) {
        connect(m_signalSource, SIGNAL(viewStateChanged(PlayerViewState)), this,
                SLOT(renderPlayerViewState(PlayerViewState)));
        connect(m_signalSource,
                SIGNAL(errorOccurred(int, QString, int, QString, QString, bool)), this,
                SLOT(handlePlayerError(int, QString, int, QString, QString, bool)));
    }

    /* Remove focus */
    this->setFocus();
    renderPlayerViewState(m_commands->viewState());
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
    mNowPlayingTitle->setObjectName("mNowPlayingTitle");
    mNowPlayingSubtitle->setObjectName("mNowPlayingSubtitle");
    mLabel[2]->setObjectName("mPositionLabel");
    mLabel[3]->setObjectName("mDurationLabel");
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

    /* Set object names */
    mPushButton[0]->setObjectName("btn_previous");
    mPushButton[1]->setObjectName("btn_play");
    mPushButton[2]->setObjectName("btn_next");
    mPushButton[3]->setObjectName("btn_favorite");
    mPushButton[4]->setObjectName("btn_mode");
    mPushButton[5]->setObjectName("btn_menu");
    mPushButton[6]->setObjectName("btn_volume");

    /* Set button state（doc/app-ui-spec.md §8.4–§8.6）：收藏 checkable、菜单可点性由列表驱动、
       音量不做 checkable（静音由浮层里的 btn_mute 表达）。 */
    mPushButton[1]->setCheckable(true);
    mPushButton[3]->setCheckable(true);
    mPushButton[3]->setEnabled(false);
    mPushButton[3]->setToolTip("Favorite");
    mPushButton[3]->setAccessibleDescription("Favorite");
    mPushButton[5]->setToolTip("Playlist");
    mPushButton[5]->setAccessibleDescription("Playlist");
    mPushButton[6]->setToolTip("Volume: 100%");
    mPushButton[6]->setAccessibleDescription("Volume: 100%");

    /* H0 layout */
    /* 两栏只锁宽度；高度交给布局计算，否则 480 的死值会盖掉最小窗口下的实际需求。 */
    mVWidget[0]->setMinimumWidth(310);
    mVWidget[0]->setMaximumWidth(310);
    mVWidget[1]->setMinimumWidth(320);
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
    /* 列表最小高度按最小窗口 960×540 反推：左栏其余控件的硬高度与布局间距合计约 300，
       最小尺寸下需给列表留出可滚动区域，取 150（约 5 行）。列表本身可滚动，
       不承担内容高度，故这是可压缩项。 */
    const int kListMinHeight = 150;
    mListWidget->setMinimumSize(310, kListMinHeight);
    mHWidget[1]->setMinimumSize(310, 80);
    mHWidget[1]->setMaximumHeight(80);
    mLabel[0]->setMinimumSize(310, 95);
    mLabel[0]->setMaximumHeight(95);
    mNowPlayingTitle->setMinimumSize(310, 28);
    mNowPlayingSubtitle->setMinimumSize(310, 18);
    mNowPlayingTitle->setMaximumHeight(28);
    mNowPlayingSubtitle->setMaximumHeight(18);
    QSpacerItem *vSpacer0 = new QSpacerItem(310, 10, QSizePolicy::Minimum, QSizePolicy::Maximum);
    QSpacerItem *vSpacer1 = new QSpacerItem(310, 30, QSizePolicy::Minimum, QSizePolicy::Maximum);
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
    QSpacerItem *vSpacer4 = new QSpacerItem(320, 30, QSizePolicy::Minimum, QSizePolicy::Maximum);
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
    /* 传输行容器内部为 H2(20) + 间隔 10 + H3(60) = 90；此前的 min/max 80 与内容自相
       矛盾，高度不足时先压掉 H2 的时长标签。改为 90，并让 V2 间距归零（间隔由 spacer 提供）。 */
    mVWidget[2]->setMinimumSize(320, 90);
    mVWidget[2]->setMaximumHeight(90);
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
    mVBoxLayout[2]->setSpacing(0);

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
    m_commands->setAutoSkipOnError(autoSkipOnError);
    m_commands->setPlaybackMode(playbackMode);
    updatePlaybackModeButton(playbackMode);

    /* 音量 / 静音（规格 §6.1）：越界钳制并回写修正值，与 playback/mode 的既有先例一致；
       类型错误的脏值按默认值 100 处理，而不是被 toInt() 悄悄读成 0。 */
    bool volumeOk          = false;
    const int storedVolume = settings.value("playback/volume", 100).toInt(&volumeOk);
    // 取值范围的真源在 SDK（Player::setVolume 自己按端点钳制），app 侧这次钳制只负责
    // 修复被外部改坏的存量值，并把修正后的值回写——与 playback/mode 的既有先例一致。
    const int volume = qBound(0, volumeOk ? storedVolume : 100, 100);
    if (volume != storedVolume) {
        settings.setValue("playback/volume", volume);
    }
    const bool muted = settings.value("playback/muted", false).toBool();
    m_commands->setVolume(volume);
    m_commands->setMuted(muted);
    updateVolumeUi();

    /* 收藏集合（规格 §6.1 / §8.4）：空串丢弃。悬挂条目保留，不做启动期 GC。 */
    mFavorites.clear();
    const QStringList favoriteIds = settings.value("library/favorites").toStringList();
    for (const QString &id : favoriteIds) {
        if (!id.isEmpty()) {
            mFavorites.insert(id);
        }
    }
    updateFavoriteButton();

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
    settings.setValue("playback/auto_skip_on_error", m_commands->autoSkipOnError());
    settings.setValue("playback/mode", static_cast<int>(m_commands->playbackMode()));
    settings.setValue("playback/volume", m_commands->volume());
    settings.setValue("playback/muted", m_commands->muted());
    settings.setValue("library/favorites", favoritesToList(mFavorites));
    warnIfSettingsWriteFailed(settings);
}

void MainWindow::persistVolumeState()
{
    QSettings settings;
    settings.setValue("playback/volume", m_commands->volume());
    settings.setValue("playback/muted", m_commands->muted());
    warnIfSettingsWriteFailed(settings);
}

void MainWindow::updateVolumeUi()
{
    const int volume = m_commands->volume();
    const bool muted = m_commands->muted();
    const QString tip = muted ? QStringLiteral("Volume: muted")
                              : QStringLiteral("Volume: %1%").arg(volume);
    if (mPushButton[6] != nullptr) {
        mPushButton[6]->setToolTip(tip);
        mPushButton[6]->setAccessibleDescription(tip);
    }
    if (mVolumePopup != nullptr) {
        mVolumePopup->syncFromState(volume, muted);
    }
}

void MainWindow::updateFavoriteButton()
{
    if (mPushButton[3] == nullptr) {
        return;
    }
    const bool hasTrack  = !mCurrentTrackId.isEmpty();
    const bool favorited = hasTrack && mFavorites.contains(mCurrentTrackId);
    mPushButton[3]->setEnabled(hasTrack);
    mPushButton[3]->setChecked(favorited);
    const QString tip = favorited ? QStringLiteral("Favorited") : QStringLiteral("Favorite");
    mPushButton[3]->setToolTip(tip);
    mPushButton[3]->setAccessibleDescription(tip);
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    persistWindowState();
    QMainWindow::closeEvent(event);
}

void MainWindow::btn_play_clicked()
{
    m_commands->playPause();
}

void MainWindow::btn_next_clicked()
{
    m_commands->nextTrack();
}

void MainWindow::btn_previous_clicked()
{
    m_commands->previousTrack();
}

void MainWindow::btn_favorite_clicked()
{
    if (mCurrentTrackId.isEmpty()) {
        return;
    }
    if (mFavorites.contains(mCurrentTrackId)) {
        mFavorites.remove(mCurrentTrackId);
    } else {
        mFavorites.insert(mCurrentTrackId);
    }
    // 收藏低频，且用户预期"点了就存住"，不走去抖（规格 §6.1）。
    QSettings settings;
    settings.setValue("library/favorites", favoritesToList(mFavorites));
    warnIfSettingsWriteFailed(settings);
    updateFavoriteButton();
}

void MainWindow::btn_playMode_clicked()
{
    PlaybackMode nextMode = m_commands->playbackMode();
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
    m_commands->setPlaybackMode(nextMode);
}

void MainWindow::btn_playList_clicked()
{
    const PlayerViewState &viewState = m_commands->viewState();
    LogPrintf(LogLevel::Debug, kTag, "btn_playList_clicked count=%d visible=%d",
              viewState.playlist.size(), mPlaylistPopup->isVisible() ? 1 : 0);
    if (viewState.playlist.isEmpty()) {
        return;
    }
    if (mPlaylistPopup->isVisible()) {
        mPlaylistPopup->hide();
        return;
    }
    mPlaylistPopup->setTracks(viewState.playlist, viewState.currentTrackIndex, -1);
    mPlaylistPopup->popup();
    LogPrintf(LogLevel::Debug, kTag, "panel popup visible=%d size=%dx%d pos=%d,%d",
              mPlaylistPopup->isVisible() ? 1 : 0, mPlaylistPopup->width(),
              mPlaylistPopup->height(), mPlaylistPopup->x(), mPlaylistPopup->y());
}

void MainWindow::btn_volume_clicked()
{
    LogPrintf(LogLevel::Debug, kTag, "btn_volume_clicked visible=%d",
              mVolumePopup->isVisible() ? 1 : 0);
    if (mVolumePopup->isVisible()) {
        mVolumePopup->hide();
        return;
    }
    updateVolumeUi();
    mVolumePopup->popup();
    LogPrintf(LogLevel::Debug, kTag, "volume popup visible=%d size=%dx%d pos=%d,%d",
              mVolumePopup->isVisible() ? 1 : 0, mVolumePopup->width(), mVolumePopup->height(),
              mVolumePopup->x(), mVolumePopup->y());
}

void MainWindow::volumeSliderChanged(int volume)
{
    m_commands->setVolume(volume);
    updateVolumeUi();
    mVolumeSaveTimer->start();
}

void MainWindow::muteToggled(bool muted)
{
    m_commands->setMuted(muted);
    updateVolumeUi();
    // 静音是离散事件，直接写，不等去抖。
    persistVolumeState();
}

void MainWindow::panelTrackActivated(int index)
{
    m_commands->selectTrack(index);
}

void MainWindow::panelTrackRemoveRequested(int index)
{
    requestRemoveTrack(index);
}

void MainWindow::panelClearPlaylistRequested()
{
    m_commands->clearPlaylist();
}

void MainWindow::requestRemoveTrack(int row)
{
    /* 行号来自某次渲染的快照，命令执行时刻列表可能已经变短（面板上一次重建之后又删过）：
       越界直接丢弃，也不记锚点——否则锚点会劫持之后某次重扫的 currentRow。 */
    if (row < 0 || row >= m_commands->trackCount()) {
        return;
    }
    /* 先记账再下发：命令返回时 SDK 已把列表改短，渲染帧随之到达，届时按这一行定选中。
       同时记下预期条数——只有「恰好少一条」的重建才消费锚点；命令被丢弃（条数没变）
       或期间发生了重扫（条数任意变）都让锚点作废，不做错误落点。 */
    mPendingSelectRow   = row;
    mPendingSelectCount = m_commands->trackCount() - 1;
    m_commands->removeTrack(row);
}

void MainWindow::keyPressEvent(QKeyEvent *event)
{
    /* 主列表的 Delete（§8.9）：`QListWidget` 默认不处理该键，事件自然上抛到这里。
       不用 `QShortcut`——它依赖窗口激活，offscreen 下判据会退化成「测快捷键有没有注册」。

       只有焦点在主列表（或其 viewport）时才生效：§8.9 的「作用视图」只有两处列表，
       焦点在播放键 / 复选框 / 进度条上时按 Delete 不该删掉一条曲目。 */
    if (event->key() == Qt::Key_Delete) {
        QWidget *focused     = QApplication::focusWidget();
        const bool listFocus = mListWidget != nullptr
            && (focused == mListWidget || focused == mListWidget->viewport());
        if (!listFocus) {
            QMainWindow::keyPressEvent(event);
            return;
        }
        const int row = mListWidget->currentRow();
        if (row >= 0) {
            requestRemoveTrack(row);
        }
        event->accept();
        return;
    }
    QMainWindow::keyPressEvent(event);
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
              clickedIndex, m_commands->currentTrackIndex(), selectedCount,
              item ? item->isSelected() : -1, item ? item->data(kRolePlaying).toBool() : -1);
    m_commands->selectTrack(clickedIndex);
}

void MainWindow::durationSliderReleased()
{
    /* Seek playback to slider position */
    LogPrintf(LogLevel::Debug, kTag, "durationSliderReleased: %d", mDurationSlider->value());
    m_commands->seekSeconds(mDurationSlider->value());
}

void MainWindow::autoSkipToggled(bool checked)
{
    m_commands->setAutoSkipOnError(checked);
}

void MainWindow::scanSongs()
{
    QDir dir(mMusicDirectory);
    QDir dirbsolutePath(dir.absolutePath());
    if (dirbsolutePath.exists()) {
        m_commands->addMusicDirectory(dirbsolutePath.absolutePath().toUtf8().constData());
    } else {
        LogMessage(LogLevel::Warning, kTag, "dir not exist");
        LogPrintf(LogLevel::Warning, kTag, "dir is %s",
                  mMusicDirectory.toUtf8().constData());
    }
}

void MainWindow::mediaPlayerInit(PlayerCommandPort *injectedPort)
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

    if (injectedPort != nullptr) {
        /* 注入路径（ADR 0016）：不建 Player、不拉起 SDL、不建日志目录。
           信号源由构造函数按 AppUiPorts::playerSignals 回填。 */
        m_commands = injectedPort;
        return;
    }

    QDir dir(mLogDir.c_str());
    if (!dir.exists())
        dir.mkdir(mLogDir.c_str());

    PlayerConfig config;
    config.logDirectory    = mLogDir;
    config.autoSkipOnError = true;
    mController.reset(new PlayerController(config));
    m_commands     = mController.get();
    m_signalSource = mController.get();
    updatePlaybackModeButton(m_commands->playbackMode());
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

    /* 收藏态（§8.4）：键来自 TrackInfo::id；切曲瞬间允许一帧残留，下一次渲染校正。 */
    mCurrentTrackId.clear();
    if (index >= 0 && index < viewState.trackIds.size()) {
        mCurrentTrackId = viewState.trackIds.at(index);
    }
    updateFavoriteButton();
    if (mRenderedTrackIndex != index) {
        // 身份键链路的现场读数（SDK deriveMusicId → TrackInfo::id → viewState.trackIds）。
        // 空键显式写 `(none)`：首帧（列表尚未到达）与「派生失败」是两件事，别混在同一个
        // `track key= index=` 的空格后面。
        const QByteArray key
            = mCurrentTrackId.isEmpty() ? QByteArrayLiteral("(none)") : mCurrentTrackId.toUtf8();
        LogPrintf(LogLevel::Debug, kTag, "track key=%s index=%d", key.constData(), index);
    }

    /* 列表为空时面板没有内容可显示，按钮直接禁用（§8.6）。 */
    if (mPushButton[5] != nullptr) {
        mPushButton[5]->setEnabled(!viewState.playlist.isEmpty());
    }

    /* 空列表不变量（§8.9）：只禁用按钮会留下一个空面板，必须顺手关掉。 */
    if (mPlaylistPopup != nullptr && viewState.playlist.isEmpty()
        && mPlaylistPopup->isVisible()) {
        mPlaylistPopup->hide();
    }

    const bool playlistChanged = (mRenderedPlaylist != viewState.playlist);
    const bool trackChanged    = (mRenderedTrackIndex != index);
    /* 本帧的列表编辑落点（两个视图共用，§8.9）：-1 = 本帧不是「删完那一帧」。
       值是未钳制的删除行，各视图按自己的条数取 min(p, n-1)。 */
    int editAnchorRow = -1;
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

        /* 列表编辑后的选中落点（§8.9）：只有「删完那一帧」按 min(删掉的行号, n-1)，
           其余刷新仍按播放行。播放标记与加粗不受影响——它们由 kRolePlaying 与播放
           下标决定，与 currentRow 解耦，连删才不会退化成「删当前播放行」。 */
        if (playlistChanged && mPendingSelectRow >= 0) {
            const int pendingRow = mPendingSelectRow;
            const int expected   = mPendingSelectCount;
            mPendingSelectRow   = -1;
            mPendingSelectCount = -1;
            /* 条数不是「恰好少一条」⇒ 这次重建不是本命令的结果（命令被丢弃或期间重扫），
               不做落点，避免选中跳到与编辑无关的行。 */
            if (mListWidget->count() == expected) {
                editAnchorRow = pendingRow;
                if (pendingRow >= 0 && mListWidget->count() > 0) {
                    mListWidget->setCurrentRow(qMin(pendingRow, mListWidget->count() - 1),
                                               QItemSelectionModel::ClearAndSelect);
                }
            }
        }
    }

    if (playlistChanged) {
        mListWidget->setUpdatesEnabled(true);
        mListWidget->doItemsLayout();
        mListWidget->viewport()->update();
    } else if (trackChanged) {
        mListWidget->viewport()->update();
    }

    /* 面板可见时跟随（§8.6）：重建走全量、仅换曲走高亮 + 居中。
       删完那一帧把同一个落点交给面板，连删才删的是邻居而不是播放行（§8.9）。 */
    if (mPlaylistPopup != nullptr && mPlaylistPopup->isVisible()) {
        if (playlistChanged) {
            mPlaylistPopup->setTracks(viewState.playlist, index, editAnchorRow);
        } else if (trackChanged) {
            mPlaylistPopup->updateCurrentIndex(index);
        }
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

QString MainWindow::buildErrorMessageBoxText(int codeValue, const QString &detail, int trackIndex,
                                             const QString &path, const QString &traceId)
{
    const ErrorCode code  = static_cast<ErrorCode>(codeValue);
    const ErrorInfo &info = GetErrorInfo(code);
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
    return message.trimmed();
}

void MainWindow::handlePlayerError(int codeValue, const QString &detail, int trackIndex,
                                   const QString &path, const QString &traceId,
                                   bool autoSkipEnabled)
{
    const QString message = buildErrorMessageBoxText(codeValue, detail, trackIndex, path, traceId);
    const QString title   = "Playback Error";

    if (autoSkipEnabled) {
        LogPrintf(LogLevel::Warning, kTag, "auto-skip error trace=%s: %s",
                  traceId.toUtf8().constData(), message.toUtf8().constData());
        return;
    }

    QMessageBox::warning(this, title, message);
}
