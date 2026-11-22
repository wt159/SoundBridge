#include "popuppanel.h"

#include <QAbstractItemView>
#include <QColor>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QItemSelectionModel>
#include <QKeyEvent>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QScreen>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QSlider>
#include <QVBoxLayout>
#include <QFont>

namespace {
// 浮层与触发按钮之间的垂直间隙（规格 §8.7）。
constexpr int kAnchorGap = 8;
// 可用高度不足时允许压缩到的下限；音量浮层本身比它小，取两者较小值。
constexpr int kMinPanelHeight = 120;
}

PopupPanel::PopupPanel(const QString &panelObjectName, QWidget *anchor)
    : QWidget(anchor != nullptr ? anchor->window() : nullptr, Qt::Popup)
    , m_anchor(anchor)
    , m_preferredWidth(300)
    , m_preferredHeight(260)
{
    setObjectName(panelObjectName);
    setFocusPolicy(Qt::StrongFocus);
    // Qt::Popup 自带「点外部关闭」的 grab；这里只需要保证它不是独立窗口的装饰外观。
    setWindowFlags(Qt::Popup | Qt::FramelessWindowHint);
}

PopupPanel::~PopupPanel() = default;

void PopupPanel::setPreferredSize(int width, int height)
{
    m_preferredWidth  = width;
    m_preferredHeight = height;
}

QWidget *PopupPanel::anchor() const
{
    return m_anchor;
}

void PopupPanel::popup()
{
    if (m_anchor == nullptr) {
        show();
        return;
    }

    QScreen *screen = m_anchor->screen();
    const QRect area
        = screen != nullptr ? screen->availableGeometry() : QRect(0, 0, 1920, 1080);
    const QPoint anchorTopLeft = m_anchor->mapToGlobal(QPoint(0, 0));
    const int above = anchorTopLeft.y() - area.top() - kAnchorGap;
    const int below = area.bottom() - (anchorTopLeft.y() + m_anchor->height()) - kAnchorGap;
    const int usable    = qMax(qMax(above, below), 0);
    const int floorSize = qMin(kMinPanelHeight, m_preferredHeight);
    const int height    = qMin(m_preferredHeight, qMax(usable, floorSize));

    resize(m_preferredWidth, height);
    positionNearAnchor();
    show();
    raise();
    setFocus(Qt::PopupFocusReason);
}

void PopupPanel::positionNearAnchor()
{
    if (m_anchor == nullptr) {
        return;
    }

    QScreen *screen = m_anchor->screen();
    const QRect area
        = screen != nullptr ? screen->availableGeometry() : QRect(0, 0, 1920, 1080);
    const QPoint anchorTopLeft = m_anchor->mapToGlobal(QPoint(0, 0));

    int x = anchorTopLeft.x() + (m_anchor->width() - width()) / 2;
    int y = anchorTopLeft.y() - height() - kAnchorGap;
    if (y < area.top()) {
        // 上方放不下就翻到按钮下方（规格 §8.7）。
        y = anchorTopLeft.y() + m_anchor->height() + kAnchorGap;
    }

    if (x + width() > area.right()) {
        x = area.right() - width();
    }
    if (x < area.left()) {
        x = area.left();
    }
    if (y + height() > area.bottom()) {
        y = area.bottom() - height();
    }
    if (y < area.top()) {
        y = area.top();
    }
    move(x, y);
}

void PopupPanel::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Escape) {
        hide();
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}

void PopupPanel::hideEvent(QHideEvent *event)
{
    QWidget::hideEvent(event);
    // 焦点回到触发按钮，键盘用户不会「掉到窗口外」（规格 §8.7）。
    if (m_anchor != nullptr) {
        m_anchor->setFocus(Qt::OtherFocusReason);
    }
}

VolumePopup::VolumePopup(QWidget *anchor)
    : PopupPanel("mVolumePopup", anchor)
    , m_slider(nullptr)
    , m_muteButton(nullptr)
{
    setPreferredSize(200, 56);

    QVBoxLayout *layout = new QVBoxLayout(this);
    layout->setContentsMargins(10, 8, 10, 8);
    layout->setSpacing(4);

    m_slider = new QSlider(Qt::Horizontal, this);
    m_slider->setObjectName("mVolumeSlider");
    m_slider->setRange(0, 100);
    m_slider->setSingleStep(1);
    m_slider->setPageStep(10);
    layout->addWidget(m_slider);

    m_muteButton = new QPushButton("MUTE", this);
    m_muteButton->setObjectName("btn_mute");
    m_muteButton->setCheckable(true);
    m_muteButton->setFixedHeight(16);
    m_muteButton->setToolTip("MUTE");
    QFont muteFont = m_muteButton->font();
    muteFont.setPixelSize(9);
    m_muteButton->setFont(muteFont);
    layout->addWidget(m_muteButton);

    connect(m_slider, SIGNAL(valueChanged(int)), this, SLOT(sliderValueChanged(int)));
    connect(m_muteButton, SIGNAL(toggled(bool)), this, SLOT(muteButtonToggled(bool)));
}

void VolumePopup::sliderValueChanged(int value)
{
    // 先让外部把新取值落到播放器与界面状态，再解除静音。反过来（先解除静音）
    // 会走一次 syncFromState，把滑杆打回解除静音前的旧音量，拖动时可见回弹。
    emit volumeChanged(value);
    if (m_muteButton->isChecked()) {
        m_muteButton->setChecked(false);
    }
}

void VolumePopup::muteButtonToggled(bool checked)
{
    emit mutedChanged(checked);
}

void VolumePopup::showEvent(QShowEvent *event)
{
    PopupPanel::showEvent(event);
    // 打开时把焦点交给滑杆：Esc 之外，键盘左右键也能直接调音量（规格 §8.7）。
    if (m_slider != nullptr) {
        m_slider->setFocus(Qt::PopupFocusReason);
    }
}

void VolumePopup::syncFromState(int volume, bool muted)
{
    if (m_slider == nullptr || m_muteButton == nullptr) {
        return;
    }
    // 回填不能回发信号：否则每次渲染都会被当成一次用户操作，写盘与下发都翻倍。
    const QSignalBlocker sliderBlocker(m_slider);
    const QSignalBlocker muteBlocker(m_muteButton);
    m_slider->setValue(volume);
    m_muteButton->setChecked(muted);
}

PlaylistPopup::PlaylistPopup(QWidget *anchor)
    : PopupPanel("mPlaylistPopup", anchor)
    , m_list(nullptr)
    , m_clearButton(nullptr)
    , m_confirm(nullptr)
    , m_confirmText(nullptr)
    , m_confirmButton(nullptr)
    , m_cancelButton(nullptr)
{
    setPreferredSize(300, 260);

    QVBoxLayout *layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 6, 6, 6);
    layout->setSpacing(0);

    m_list = new QListWidget(this);
    m_list->setObjectName("mPanelList");
    m_list->setUniformItemSizes(true);
    m_list->setSelectionMode(QAbstractItemView::SingleSelection);
    m_list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_list->setTextElideMode(Qt::ElideRight);
    layout->addWidget(m_list);

    /* 常态入口（§8.9）：单条删除没有确认，清空必须过确认条。 */
    m_clearButton = new QPushButton("CLEAR", this);
    m_clearButton->setObjectName("btn_clear");
    layout->addWidget(m_clearButton);

    /* 内嵌确认条：PlaylistPopup 内部的一行 QWidget，不新增浮层机制（ADR 0015）。 */
    m_confirm = new QWidget(this);
    m_confirm->setObjectName("mPanelConfirm");
    QHBoxLayout *confirmLayout = new QHBoxLayout(m_confirm);
    confirmLayout->setContentsMargins(0, 0, 0, 0);
    confirmLayout->setSpacing(4);

    m_confirmText = new QLabel(m_confirm);
    m_confirmText->setTextFormat(Qt::PlainText);
    confirmLayout->addWidget(m_confirmText, 1);

    m_confirmButton = new QPushButton("CLEAR", m_confirm);
    m_confirmButton->setObjectName("btn_confirmClear");
    confirmLayout->addWidget(m_confirmButton);

    m_cancelButton = new QPushButton("CANCEL", m_confirm);
    m_cancelButton->setObjectName("btn_cancelClear");
    confirmLayout->addWidget(m_cancelButton);

    layout->addWidget(m_confirm);
    setConfirmVisible(false);

    connect(m_list, SIGNAL(itemClicked(QListWidgetItem *)), this,
            SLOT(listItemClicked(QListWidgetItem *)));
    connect(m_clearButton, SIGNAL(clicked()), this, SLOT(clearButtonClicked()));
    connect(m_confirmButton, SIGNAL(clicked()), this, SLOT(confirmClearClicked()));
    connect(m_cancelButton, SIGNAL(clicked()), this, SLOT(cancelClearClicked()));
}

void PlaylistPopup::listItemClicked(QListWidgetItem *item)
{
    if (item == nullptr) {
        return;
    }
    const int index = m_list->row(item);
    hide();
    emit trackActivated(index);
}

void PlaylistPopup::keyPressEvent(QKeyEvent *event)
{
    /* 面板内 Delete 必须在 Esc 分支之前拦下（§8.9）：删完面板不关，可接着连删。
       无选中时同样 accept——否则事件会冒泡到 MainWindow，删掉的是主列表那一行。 */
    if (event->key() == Qt::Key_Delete) {
        const int row = m_list != nullptr ? m_list->currentRow() : -1;
        if (row >= 0) {
            emit trackRemoveRequested(row);
        }
        event->accept();
        return;
    }
    PopupPanel::keyPressEvent(event);
}

void PlaylistPopup::hideEvent(QHideEvent *event)
{
    /* 复位规则（§8.9）：面板一关就回到常态，下次打开不存在残留确认态。 */
    setConfirmVisible(false);
    PopupPanel::hideEvent(event);
}

void PlaylistPopup::clearButtonClicked()
{
    setConfirmVisible(true);
}

void PlaylistPopup::confirmClearClicked()
{
    /* 只发一条命令，然后关面板；关面板经 hideEvent 复位确认态。 */
    emit clearPlaylistRequested();
    hide();
}

void PlaylistPopup::cancelClearClicked()
{
    setConfirmVisible(false);
}

void PlaylistPopup::setConfirmVisible(bool visible)
{
    if (m_clearButton == nullptr || m_confirm == nullptr) {
        return;
    }
    m_clearButton->setVisible(!visible);
    m_confirm->setVisible(visible);
    if (visible) {
        updateConfirmText();
    }
}

void PlaylistPopup::updateConfirmText()
{
    if (m_confirmText == nullptr || m_list == nullptr) {
        return;
    }
    /* 文案逐字取 §1.4：`Clear all <n> tracks?`，<n> 无前导零。 */
    m_confirmText->setText(QString("Clear all %1 tracks?").arg(m_list->count()));
}

void PlaylistPopup::showEvent(QShowEvent *event)
{
    PopupPanel::showEvent(event);
    // 焦点进列表，方向键可直接换行（规格 §8.7）；Esc 由 QListWidget 上抛给底座。
    if (m_list != nullptr) {
        m_list->setFocus(Qt::PopupFocusReason);
    }
}

void PlaylistPopup::setTracks(const QStringList &names, int currentIndex, int preferredRow)
{
    if (m_list == nullptr) {
        return;
    }
    m_list->clear();
    for (const QString &name : names) {
        m_list->addItem(name);
    }
    updateCurrentIndex(currentIndex);
    if (preferredRow >= 0 && m_list->count() > 0) {
        m_list->setCurrentRow(qMin(preferredRow, m_list->count() - 1),
                              QItemSelectionModel::ClearAndSelect);
    }
    /* 确认态在面板开着时列表被改（例如刚用 Delete 删了一条）：条数要跟着变。 */
    if (m_confirm != nullptr && m_confirm->isVisible()) {
        updateConfirmText();
    }
}

void PlaylistPopup::updateCurrentIndex(int index)
{
    if (m_list == nullptr) {
        return;
    }
    for (int i = 0; i < m_list->count(); ++i) {
        QListWidgetItem *item = m_list->item(i);
        if (item == nullptr) {
            continue;
        }
        const bool playing = (i == index);
        QFont font         = item->font();
        font.setBold(playing);
        item->setFont(font);
        item->setForeground(playing ? QColor(94, 220, 243) : QColor(Qt::white));
    }
    if (index >= 0 && index < m_list->count()) {
        m_list->setCurrentRow(index);
        m_list->scrollToItem(m_list->item(index), QAbstractItemView::PositionAtCenter);
    }
}
