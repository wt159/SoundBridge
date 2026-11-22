// AppUiTests —— app 层 UI 判据自动化（地图 #248 / 执行票 #257）。
//
// 判据真源：doc/app-ui-spec.md §9.7.2（在册 57 条：自动 50 / 半自动 4 / 人工 3）。
// 本文件按判据编号命名用例；`UI-VOL1` / `UI-VOL2` 由既有 MusicPlayerVolume +
// PcmGain 单测承担，本目标不重复（§9.7.2）；永久人工三条（UI-E2 呈现半边 /
// UI-V3 / UI-P4）不写用例（§9.7.5）。
//
// 环境纪律（规格 §9.7.3）：
// - offscreen 平台插件由 CMake POST_BUILD 部署到 exe 旁 platforms/；
// - 日志目录与 QSettings 路径全部 pid 化（ctest -j4 的前提，见 TestProcess.hpp）；
// - 不依赖像素宽度/文本像素度量；不做任何人耳/目测判断；
// - 任何会弹 QMessageBox 的路径不得在 offscreen 下执行（Windows 必崩）——
//   错误呈现走纯函数 buildErrorMessageBoxText 断言（UI-E2 文案半边）。

#include "FakeCommandPort.hpp"
#include "mainwindow.h"

#include "soundbridge/logging.h"

#include <QtTest>

#include <QApplication>
#include <QCheckBox>
#include <QDeadlineTimer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QLabel>
#include <QListWidget>
#include <QPointer>
#include <QPushButton>
#include <QRegularExpression>
#include <QScreen>
#include <QScrollBar>
#include <QSettings>
#include <QSlider>
#include <QXmlStreamReader>

#include <memory>

#include "TestProcess.hpp"

namespace {

// mainwindow.cpp 匿名命名空间 kRolePlaying 的对齐值（Qt::UserRole + 1）。
constexpr int kPlayingRole = Qt::UserRole + 1;

// 规格 §1.3 锚点集（mListMask 随死代码删除作废，§7 更正四，不入集）。
const QStringList kAnchors = {
    "mListWidget",     "mDurationSlider", "mAutoSkipCheck",     "btn_previous",
    "btn_play",        "btn_next",        "btn_favorite",       "btn_mode",
    "btn_menu",        "btn_volume",      "mNowPlayingTitle",   "mNowPlayingSubtitle",
    "mPositionLabel",  "mDurationLabel",  "mVolumePopup",       "mVolumeSlider",
    "btn_mute",        "mPlaylistPopup",  "mPanelList",
};

// UI-E6 表驱动：六个文件级原因文案，与 PlayerController::errorReasonText 逐字一致
// （app/playercontroller.cpp，措辞归 app 层、禁止共用）。
const QMap<int, QString> kReasonTexts = {
    {static_cast<int>(soundbridge::ErrorCode::ExtractorUnsupported), "FORMAT NOT SUPPORTED"},
    {static_cast<int>(soundbridge::ErrorCode::ExtractorInitFailed), "FILE IS DAMAGED - CANNOT PLAY"},
    {static_cast<int>(soundbridge::ErrorCode::FileOpenFailed), "FILE CANNOT BE OPENED"},
    {static_cast<int>(soundbridge::ErrorCode::DecodeInitFailed), "FILE CANNOT BE DECODED"},
    {static_cast<int>(soundbridge::ErrorCode::DataCorrupted), "FILE IS CORRUPTED"},
    {static_cast<int>(soundbridge::ErrorCode::DataTruncated), "FILE MAY BE INCOMPLETE"},
};

// 四个不可恢复错误码（ErrorInfo.recoverable == false，UI-E4）。
const QList<int> kUnrecoverableCodes = {
    static_cast<int>(soundbridge::ErrorCode::FileOpenFailed),
    static_cast<int>(soundbridge::ErrorCode::ExtractorUnsupported),
    static_cast<int>(soundbridge::ErrorCode::ExtractorInitFailed),
    static_cast<int>(soundbridge::ErrorCode::DecodeInitFailed),
};

QString g_musicDir;    // 存在的空曲库根（scanSongs 会下发 addMusicDirectory）
QString g_missingDir;  // 不存在的曲库根（UI-L6）

QPushButton *btnOf(MainWindow *w, const char *name)
{
    return w->findChild<QPushButton *>(name);
}

MainWindow *makeWindow(FakeCommandPort *port, const QString &musicDir = g_musicDir)
{
    AppUiPorts ports;
    ports.player = port;
    ports.playerSignals = port;
    ports.musicDirectory = musicDir;
    return new MainWindow(ports);
}

void inject(MainWindow *w, FakeCommandPort *port) { w->renderPlayerViewState(port->state); }

void seedIni(const QString &key, const QVariant &value)
{
    QSettings settings;
    settings.setValue(key, value);
    settings.sync();
}

int topLevelCount() { return QApplication::topLevelWidgets().size(); }

QString pidLogRoot()
{
    return QCoreApplication::applicationDirPath()
        + QString("/app_ui_tests_log/%1").arg(sb_test::processId());
}

/* g3log 经异步 worker 落盘：轮询 pid 日志目录直到命中文本或超时。 */
bool waitForLogText(const QString &needle, int timeoutMs = 7000)
{
    const QByteArray needleUtf8 = needle.toUtf8();
    const QDeadlineTimer deadline(timeoutMs);
    while (!deadline.hasExpired()) {
        const QDir dir(pidLogRoot());
        const QFileInfoList logs = dir.entryInfoList({"*.log"}, QDir::Files);
        for (const QFileInfo &fi : logs) {
            QFile file(fi.absoluteFilePath());
            if (file.open(QIODevice::ReadOnly | QIODevice::Text)
                && file.readAll().contains(needleUtf8)) {
                return true;
            }
        }
        QTest::qWait(200);
    }
    return false;
}

QString qssText()
{
    QFile file(":/style.qss");
    if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return QString::fromUtf8(file.readAll());
    }
    return QString();
}

} // namespace

class AppUiTests : public QObject {
    Q_OBJECT

private slots:
    // ---- S 区（状态，8 条）----
    void s1_play_pause_stop_checked_and_subtitle();
    void s2_stopped_keeps_title_and_duration();
    void s3_error_subtitle_clears_on_track_change();
    void s4_duration_65s_label_and_slider();
    void s5_zero_duration_floor();
    void s6_dragging_slider_freezes_position_value();
    void s7_release_seeks_and_position_updates();
    void s8_empty_list_defaults();

    // ---- L 区（列表，7 条，含半自动 UI-L6）----
    void l1_list_rows_match_injected_playlist();
    void l2_second_row_selected_and_playing_marker();
    void l3_click_current_row_issues_select_track();
    void l4_list_click_forwards_index_without_side_effects();
    void l5_far_row_scrolled_into_view();
    void l6_missing_music_dir_no_scan_no_modal();
    void l7_main_list_anchor_unique();

    // ---- T 区（传输，7 条）----
    void t1_play_click_sends_playpause();
    void t2_pause_click_unchecks_but_keeps_subtitle();
    void t3_empty_list_transport_clicks_no_change();
    void t4_three_next_clicks_recorded();
    void t5_mode_button_tooltip_cycle();
    void t6_mode_and_autoskip_survive_restart();
    void t7_autoskip_single_call_discipline();

    // ---- E 区（错误，7 条）----
    void e1_autoskip_error_logs_trace_and_no_modal();
    void e2_message_box_fields_and_stopped_subtitle();
    void e3_recoverable_error_carries_position();
    void e4_unrecoverable_codes_never_carry_position();
    void e5_batch_summary_is_uppercase_detail_only();
    void e6_six_reason_texts_are_distinct_and_exact();
    void e7_error_subtitle_restored_after_next_track();

    // ---- P 区（持久化，5 条；UI-P4 永久人工）----
    void p1_first_run_window_size_and_centering();
    void p2_geometry_mode_autoskip_roundtrip();
    void p3_illegal_mode_falls_back_and_rewrites();
    void p5_settings_key_set_is_exact();
    void p6_minimum_size_no_overlap_and_scrollable();

    // ---- V 区（视觉，9 条；UI-V3 永久人工，UI-V4 半自动）----
    void v1_two_columns_ordered_and_visible();
    void v2_minimum_size_scrollbar_and_bounds();
    void v4_state_colors_pairwise_distinct();
    void v5_every_transport_button_has_qss_icon();
    void v6_qss_selectors_stay_within_anchors();
    void v7_qrc_files_exist_on_disk();
    void v9_popup_geometry_within_bounds_and_off_anchor();
    void v10_popup_flags_esc_and_focus_return();

    // ---- F 区（收藏，4 条）----
    void f1_favorite_checkable_and_two_qss_branches();
    void f2_favorite_survives_restart();
    void f3_favorite_marker_follows_track_switch();
    void f4_empty_id_disables_and_does_not_write();

    // ---- VOL 区（音量，2 条；VOL1/VOL2 由既有单测承担）----
    void vol3_volume_persisted_and_restored();
    void vol4_mute_toggle_keeps_volume_value();

    // ---- M 区（播放列表面板，5 条）----
    void m1_menu_enabled_follows_playlist();
    void m2_panel_rows_and_bold_current();
    void m3_panel_row_click_selects_and_closes();
    void m4_panel_highlight_follows_next();
    void m5_both_popups_are_qt_popup_and_esc_closes();

    // ---- 生命周期 ----
    void initTestCase();
    void init();
    void cleanup();

private:
    QPointer<MainWindow> m_lastWindow;
};

void AppUiTests::initTestCase()
{
    const QString pid = QString::number(sb_test::processId());
    g_musicDir = QDir::tempPath() + "/app_ui_tests_music_" + pid;
    QDir().mkpath(g_musicDir);
    g_missingDir = g_musicDir + "_does_not_exist";
}

void AppUiTests::init()
{
    /* 每用例清空 QSettings（IniFormat 已在 main() 指到 pid 化目录）。 */
    QSettings settings;
    settings.clear();
    settings.sync();
    QFile::remove(settings.fileName());
}

void AppUiTests::cleanup()
{
    if (m_lastWindow != nullptr) {
        delete m_lastWindow;
    }
}

// ============ S 区 ============

void AppUiTests::s1_play_pause_stop_checked_and_subtitle()
{
    FakeCommandPort fake;
    std::unique_ptr<MainWindow> w(makeWindow(&fake));
    m_lastWindow = w.get();

    fake.state.playerState = static_cast<int>(soundbridge::PlayerState::Playing);
    fake.state.subtitle = "NOW PLAYING";
    inject(w.get(), &fake);
    QCOMPARE(btnOf(w.get(), "btn_play")->isChecked(), true);
    QCOMPARE(w->findChild<QLabel *>("mNowPlayingSubtitle")->text(), QString("NOW PLAYING"));

    fake.state.playerState = static_cast<int>(soundbridge::PlayerState::Paused);
    inject(w.get(), &fake);
    QCOMPARE(btnOf(w.get(), "btn_play")->isChecked(), false);
    QCOMPARE(w->findChild<QLabel *>("mNowPlayingSubtitle")->text(), QString("NOW PLAYING"));

    fake.state.playerState = static_cast<int>(soundbridge::PlayerState::Stopped);
    fake.state.subtitle = "IDLE";
    inject(w.get(), &fake);
    QCOMPARE(btnOf(w.get(), "btn_play")->isChecked(), false);
    QCOMPARE(w->findChild<QLabel *>("mNowPlayingSubtitle")->text(), QString("IDLE"));
}

void AppUiTests::s2_stopped_keeps_title_and_duration()
{
    FakeCommandPort fake;
    std::unique_ptr<MainWindow> w(makeWindow(&fake));
    m_lastWindow = w.get();

    fake.state.playlist = (QStringList() << "Song A");
    fake.state.trackIds = (QStringList() << "id-a");
    fake.state.currentTrackIndex = 0;
    fake.state.title = "Song A";
    fake.state.durationMs = 120000;
    fake.state.formattedDuration = "02:00";
    fake.state.playerState = static_cast<int>(soundbridge::PlayerState::Playing);
    fake.state.subtitle = "NOW PLAYING";
    inject(w.get(), &fake);
    QCOMPARE(w->findChild<QLabel *>("mNowPlayingTitle")->text(), QString("Song A"));
    QCOMPARE(w->findChild<QLabel *>("mDurationLabel")->text(), QString("02:00"));

    /* 停止：标题与时长保持上一首的值（规格 UI-S2），只有状态与副标题变。 */
    fake.state.playerState = static_cast<int>(soundbridge::PlayerState::Stopped);
    fake.state.subtitle = "IDLE";
    inject(w.get(), &fake);
    QCOMPARE(btnOf(w.get(), "btn_play")->isChecked(), false);
    QCOMPARE(w->findChild<QLabel *>("mNowPlayingTitle")->text(), QString("Song A"));
    QCOMPARE(w->findChild<QLabel *>("mDurationLabel")->text(), QString("02:00"));
}

void AppUiTests::s3_error_subtitle_clears_on_track_change()
{
    FakeCommandPort fake;
    std::unique_ptr<MainWindow> w(makeWindow(&fake));
    m_lastWindow = w.get();

    fake.state.playlist = (QStringList() << "Bad" << "Good");
    fake.state.trackIds = (QStringList() << "id-bad" << "id-good");
    fake.state.currentTrackIndex = 0;
    fake.state.title = "Bad";
    fake.state.subtitle = "FILE IS DAMAGED - CANNOT PLAY - SKIPPED";
    inject(w.get(), &fake);
    QCOMPARE(w->findChild<QLabel *>("mNowPlayingSubtitle")->text(),
             QString("FILE IS DAMAGED - CANNOT PLAY - SKIPPED"));

    fake.state.currentTrackIndex = 1;
    fake.state.title = "Good";
    fake.state.subtitle = "NOW PLAYING";
    fake.state.playerState = static_cast<int>(soundbridge::PlayerState::Playing);
    inject(w.get(), &fake);
    QCOMPARE(w->findChild<QLabel *>("mNowPlayingSubtitle")->text(), QString("NOW PLAYING"));
}

void AppUiTests::s4_duration_65s_label_and_slider()
{
    FakeCommandPort fake;
    std::unique_ptr<MainWindow> w(makeWindow(&fake));
    m_lastWindow = w.get();

    fake.state.durationMs = 65000;
    fake.state.formattedDuration = "01:05";
    inject(w.get(), &fake);
    QCOMPARE(w->findChild<QLabel *>("mDurationLabel")->text(), QString("01:05"));
    QCOMPARE(w->findChild<QSlider *>("mDurationSlider")->maximum(), 65);
}

void AppUiTests::s5_zero_duration_floor()
{
    FakeCommandPort fake;
    std::unique_ptr<MainWindow> w(makeWindow(&fake));
    m_lastWindow = w.get();

    inject(w.get(), &fake);
    QCOMPARE(w->findChild<QLabel *>("mDurationLabel")->text(), QString("00:00"));
    QCOMPARE(w->findChild<QLabel *>("mPositionLabel")->text(), QString("00:00"));
    QSlider *slider = w->findChild<QSlider *>("mDurationSlider");
    QCOMPARE(slider->maximum(), 0);
    QCOMPARE(slider->value(), 0);
}

void AppUiTests::s6_dragging_slider_freezes_position_value()
{
    FakeCommandPort fake;
    std::unique_ptr<MainWindow> w(makeWindow(&fake));
    m_lastWindow = w.get();

    fake.state.durationMs = 100000;
    fake.state.formattedDuration = "01:40";
    fake.state.positionMs = 40000;
    fake.state.formattedPosition = "00:40";
    inject(w.get(), &fake);

    QSlider *slider = w->findChild<QSlider *>("mDurationSlider");
    slider->setSliderDown(true);
    fake.state.positionMs = 60000;
    fake.state.formattedPosition = "01:00";
    inject(w.get(), &fake);
    /* 按住期间：滑块不跟随位置回调（渲染守卫 isSliderDown），位置标签仍更新。 */
    QCOMPARE(slider->value(), 40);
    QCOMPARE(w->findChild<QLabel *>("mPositionLabel")->text(), QString("01:00"));
    slider->setSliderDown(false);
}

void AppUiTests::s7_release_seeks_and_position_updates()
{
    FakeCommandPort fake;
    std::unique_ptr<MainWindow> w(makeWindow(&fake));
    m_lastWindow = w.get();

    fake.state.durationMs = 100000;
    fake.state.formattedDuration = "01:40";
    inject(w.get(), &fake);

    QSlider *slider = w->findChild<QSlider *>("mDurationSlider");
    slider->setValue(30);
    QVERIFY(QMetaObject::invokeMethod(w.get(), "durationSliderReleased"));
    QCOMPARE(fake.countOf("seekSeconds"), 1);
    QCOMPARE(fake.lastArgOf("seekSeconds"), QString("30"));

    /* 位置回调随后到达：标签更新（UI-S7 后半）。 */
    fake.state.positionMs = 30000;
    fake.state.formattedPosition = "00:30";
    inject(w.get(), &fake);
    QCOMPARE(w->findChild<QLabel *>("mPositionLabel")->text(), QString("00:30"));
}

void AppUiTests::s8_empty_list_defaults()
{
    FakeCommandPort fake;
    std::unique_ptr<MainWindow> w(makeWindow(&fake));
    m_lastWindow = w.get();

    /* 曲库根存在：构造即下发一次扫描（UI-L1 的启动半边）。 */
    QCOMPARE(fake.countOf("addMusicDirectory"), 1);

    inject(w.get(), &fake);
    QCOMPARE(w->findChild<QLabel *>("mNowPlayingTitle")->text(), QString("—"));
    QCOMPARE(w->findChild<QLabel *>("mNowPlayingSubtitle")->text(), QString("IDLE"));
    QCOMPARE(w->findChild<QLabel *>("mDurationLabel")->text(), QString("00:00"));
    QCOMPARE(w->findChild<QLabel *>("mPositionLabel")->text(), QString("00:00"));
    QCOMPARE(btnOf(w.get(), "btn_play")->isChecked(), false);
}

// ============ L 区 ============

void AppUiTests::l1_list_rows_match_injected_playlist()
{
    FakeCommandPort fake;
    std::unique_ptr<MainWindow> w(makeWindow(&fake));
    m_lastWindow = w.get();

    fake.state.playlist = (QStringList() << "a.mp3" << "sub dir/b.flac" << "c.wav");
    inject(w.get(), &fake);
    QListWidget *list = w->findChild<QListWidget *>("mListWidget");
    QCOMPARE(list->count(), 3);
    QCOMPARE(list->item(0)->text(), QString("a.mp3"));
    QCOMPARE(list->item(1)->text(), QString("sub dir/b.flac"));
    QCOMPARE(list->item(2)->text(), QString("c.wav"));
}

void AppUiTests::l2_second_row_selected_and_playing_marker()
{
    FakeCommandPort fake;
    std::unique_ptr<MainWindow> w(makeWindow(&fake));
    m_lastWindow = w.get();

    fake.state.playlist = (QStringList() << "Row 0" << "Row 1" << "Row 2");
    fake.state.currentTrackIndex = 1;
    inject(w.get(), &fake);
    QListWidget *list = w->findChild<QListWidget *>("mListWidget");
    QCOMPARE(list->item(1)->data(kPlayingRole).toBool(), true);
    QCOMPARE(list->item(1)->isSelected(), true);
    QCOMPARE(list->item(0)->data(kPlayingRole).toBool(), false);
    QCOMPARE(list->currentRow(), 1);
}

void AppUiTests::l3_click_current_row_issues_select_track()
{
    FakeCommandPort fake;
    std::unique_ptr<MainWindow> w(makeWindow(&fake));
    m_lastWindow = w.get();

    fake.state.playlist = (QStringList() << "Row 0" << "Row 1");
    fake.state.currentTrackIndex = 0;
    inject(w.get(), &fake);
    QListWidget *list = w->findChild<QListWidget *>("mListWidget");
    QVERIFY(QMetaObject::invokeMethod(w.get(), "listWidgetCliked",
                                      Q_ARG(QListWidgetItem *, list->item(0))));
    QCOMPARE(fake.countOf("selectTrack"), 1);
    QCOMPARE(fake.lastArgOf("selectTrack"), QString("0"));
}

void AppUiTests::l4_list_click_forwards_index_without_side_effects()
{
    FakeCommandPort fake;
    std::unique_ptr<MainWindow> w(makeWindow(&fake));
    m_lastWindow = w.get();

    fake.state.playlist = (QStringList() << "Row 0" << "Row 1" << "Row 2");
    fake.state.currentTrackIndex = 0;
    inject(w.get(), &fake);
    QListWidget *list = w->findChild<QListWidget *>("mListWidget");
    const int baselineTopLevel = topLevelCount();

    QVERIFY(QMetaObject::invokeMethod(w.get(), "listWidgetCliked",
                                      Q_ARG(QListWidgetItem *, list->item(1))));
    QCOMPARE(fake.countOf("selectTrack"), 1);
    QCOMPARE(fake.lastArgOf("selectTrack"), QString("1"));
    /* 未注入新状态：界面无可见变化、无弹窗。 */
    QCOMPARE(w->findChild<QLabel *>("mNowPlayingTitle")->text(), QString("—"));
    QCOMPARE(topLevelCount(), baselineTopLevel);
}

void AppUiTests::l5_far_row_scrolled_into_view()
{
    FakeCommandPort fake;
    std::unique_ptr<MainWindow> w(makeWindow(&fake));
    m_lastWindow = w.get();

    QStringList rows;
    for (int i = 0; i < 100; ++i) {
        rows.append(QString("Track %1").arg(i, 3, 10, QLatin1Char('0')));
    }
    fake.state.playlist = rows;
    fake.state.currentTrackIndex = 90;
    inject(w.get(), &fake);

    QListWidget *list = w->findChild<QListWidget *>("mListWidget");
    w->show();
    QTest::qWait(50);
    QVERIFY(list->visualItemRect(list->item(90)).intersects(list->viewport()->rect()));
    QCOMPARE(list->item(0)->data(kPlayingRole).toBool(), false);
    QCOMPARE(list->item(90)->data(kPlayingRole).toBool(), true);
}

void AppUiTests::l6_missing_music_dir_no_scan_no_modal()
{
    FakeCommandPort fake;
    const int baselineTopLevel = topLevelCount();
    std::unique_ptr<MainWindow> w(makeWindow(&fake, g_missingDir));
    m_lastWindow = w.get();

    /* 目录不存在：不下发扫描、列表为空、无弹窗（Warning 见日志断言）。
       基线在窗口建完后重取，排除 QApplication 惰性顶层件。 */
    QCOMPARE(fake.countOf("addMusicDirectory"), 0);
    QCOMPARE(w->findChild<QListWidget *>("mListWidget")->count(), 0);
    const int afterWindowTopLevel = topLevelCount();
    QVERIFY(waitForLogText("dir not exist"));
    QCOMPARE(topLevelCount(), afterWindowTopLevel);
    QVERIFY(baselineTopLevel <= afterWindowTopLevel);
}

void AppUiTests::l7_main_list_anchor_unique()
{
    FakeCommandPort fake;
    std::unique_ptr<MainWindow> w(makeWindow(&fake));
    m_lastWindow = w.get();

    QCOMPARE(w->findChildren<QListWidget *>("mListWidget").size(), 1);
    QVERIFY(w->findChild<QListWidget *>("mListWidget") != nullptr);
}

// ============ T 区 ============

void AppUiTests::t1_play_click_sends_playpause()
{
    FakeCommandPort fake;
    std::unique_ptr<MainWindow> w(makeWindow(&fake));
    m_lastWindow = w.get();
    w->show();
    QTest::qWait(30);

    QTest::mouseClick(btnOf(w.get(), "btn_play"), Qt::LeftButton);
    QCOMPARE(fake.countOf("playPause"), 1);

    fake.state.playerState = static_cast<int>(soundbridge::PlayerState::Playing);
    fake.state.subtitle = "NOW PLAYING";
    inject(w.get(), &fake);
    QCOMPARE(btnOf(w.get(), "btn_play")->isChecked(), true);
}

void AppUiTests::t2_pause_click_unchecks_but_keeps_subtitle()
{
    FakeCommandPort fake;
    std::unique_ptr<MainWindow> w(makeWindow(&fake));
    m_lastWindow = w.get();
    w->show();
    QTest::qWait(30);

    fake.state.playerState = static_cast<int>(soundbridge::PlayerState::Playing);
    fake.state.subtitle = "NOW PLAYING";
    inject(w.get(), &fake);
    QTest::mouseClick(btnOf(w.get(), "btn_play"), Qt::LeftButton);
    QCOMPARE(fake.countOf("playPause"), 1);

    fake.state.playerState = static_cast<int>(soundbridge::PlayerState::Paused);
    inject(w.get(), &fake);
    QCOMPARE(btnOf(w.get(), "btn_play")->isChecked(), false);
    QCOMPARE(w->findChild<QLabel *>("mNowPlayingSubtitle")->text(), QString("NOW PLAYING"));
}

void AppUiTests::t3_empty_list_transport_clicks_no_change()
{
    FakeCommandPort fake;
    std::unique_ptr<MainWindow> w(makeWindow(&fake));
    m_lastWindow = w.get();
    const int baselineTopLevel = topLevelCount();

    QTest::mouseClick(btnOf(w.get(), "btn_previous"), Qt::LeftButton);
    QTest::mouseClick(btnOf(w.get(), "btn_next"), Qt::LeftButton);
    QTest::mouseClick(btnOf(w.get(), "btn_play"), Qt::LeftButton);
    QCOMPARE(fake.countOf("previousTrack"), 1);
    QCOMPARE(fake.countOf("nextTrack"), 1);
    QCOMPARE(fake.countOf("playPause"), 1);
    /* 命令已下发但状态未被替身改变：无曲目变化、无弹窗。 */
    QCOMPARE(fake.state.currentTrackIndex, -1);
    QCOMPARE(w->findChild<QLabel *>("mNowPlayingTitle")->text(), QString("—"));
    QCOMPARE(topLevelCount(), baselineTopLevel);
}

void AppUiTests::t4_three_next_clicks_recorded()
{
    FakeCommandPort fake;
    std::unique_ptr<MainWindow> w(makeWindow(&fake));
    m_lastWindow = w.get();
    w->show();
    QTest::qWait(30);

    for (int i = 0; i < 3; ++i) {
        QTest::mouseClick(btnOf(w.get(), "btn_next"), Qt::LeftButton);
    }
    QCOMPARE(fake.countOf("nextTrack"), 3);
    QVERIFY(w->isVisible()); /* 进程存活、窗口在（不崩溃） */
}

void AppUiTests::t5_mode_button_tooltip_cycle()
{
    FakeCommandPort fake;
    std::unique_ptr<MainWindow> w(makeWindow(&fake));
    m_lastWindow = w.get();
    w->show();
    QTest::qWait(30);

    const QStringList expected = {"Playback mode: Loop", "Playback mode: Random",
        "Playback mode: Single Once", "Playback mode: Single Loop",
        "Playback mode: Sequential"};
    QPushButton *mode = btnOf(w.get(), "btn_mode");
    for (const QString &tip : expected) {
        QTest::mouseClick(mode, Qt::LeftButton);
        inject(w.get(), &fake);
        QCOMPARE(mode->toolTip(), tip);
    }
    /* 5 次点击回到起点（§4.4 顺序）。 */
    QCOMPARE(fake.state.playbackMode, static_cast<int>(soundbridge::PlaybackMode::Sequential));
}

void AppUiTests::t6_mode_and_autoskip_survive_restart()
{
    FakeCommandPort fake1;
    {
        std::unique_ptr<MainWindow> w1(makeWindow(&fake1));
        w1->show();
        QTest::qWait(30);
        QTest::mouseClick(btnOf(w1.get(), "btn_mode"), Qt::LeftButton);
        QTest::mouseClick(w1->findChild<QCheckBox *>("mAutoSkipCheck"), Qt::LeftButton);
        QCOMPARE(fake1.state.playbackMode,
                 static_cast<int>(soundbridge::PlaybackMode::Loop));
        QCOMPARE(fake1.state.autoSkipOnError, false);
        w1->close(); /* closeEvent 落盘（§6.1） */
    }

    FakeCommandPort fake2;
    std::unique_ptr<MainWindow> w2(makeWindow(&fake2));
    m_lastWindow = w2.get();
    QCOMPARE(fake2.countOf("setPlaybackMode"), 1);
    QCOMPARE(fake2.lastArgOf("setPlaybackMode"),
             QString::number(static_cast<int>(soundbridge::PlaybackMode::Loop)));
    QCOMPARE(fake2.countOf("setAutoSkipOnError"), 1);
    QCOMPARE(fake2.state.autoSkipOnError, false);
    QCOMPARE(w2->findChild<QCheckBox *>("mAutoSkipCheck")->isChecked(), false);
}

void AppUiTests::t7_autoskip_single_call_discipline()
{
    FakeCommandPort fake;
    std::unique_ptr<MainWindow> w(makeWindow(&fake));
    m_lastWindow = w.get();
    /* 构造期 restoreWindowState 调过一次（ini 默认 true）。 */
    QCOMPARE(fake.countOf("setAutoSkipOnError"), 1);

    w->show();
    QTest::qWait(30);
    QTest::mouseClick(w->findChild<QCheckBox *>("mAutoSkipCheck"), Qt::LeftButton);
    QCOMPARE(fake.countOf("setAutoSkipOnError"), 2);

    /* SDK 回写同值（blockSignals 守卫）：不产生第二次 setAutoSkipOnError。 */
    inject(w.get(), &fake);
    QCOMPARE(fake.countOf("setAutoSkipOnError"), 2);
}

// ============ E 区 ============

void AppUiTests::e1_autoskip_error_logs_trace_and_no_modal()
{
    FakeCommandPort fake;
    std::unique_ptr<MainWindow> w(makeWindow(&fake));
    m_lastWindow = w.get();
    const int baselineTopLevel = topLevelCount();

    fake.emitError(static_cast<int>(soundbridge::ErrorCode::ExtractorInitFailed), "detail",
                   0, "bad.wav", "trace-e1", true);
    QCOMPARE(topLevelCount(), baselineTopLevel); /* autoSkip 开：无弹窗 */
    QVERIFY(waitForLogText("auto-skip error trace=trace-e1"));
}

void AppUiTests::e2_message_box_fields_and_stopped_subtitle()
{
    FakeCommandPort fake;
    std::unique_ptr<MainWindow> w(makeWindow(&fake));
    m_lastWindow = w.get();

    /* 文案半边（纯函数）：六个字段齐全。呈现半边永久人工（§9.7.5）。 */
    const QString text = MainWindow::buildErrorMessageBoxText(
        static_cast<int>(soundbridge::ErrorCode::FileOpenFailed), "detail-x", 7,
        "C:/music/x.wav", "t-9");
    QVERIFY2(text.startsWith("Message: "), qPrintable(text));
    QVERIFY(text.contains("Module: "));
    QVERIFY(text.contains("Severity: "));
    QVERIFY(text.contains("Action: "));
    QVERIFY(text.contains("Index: 7"));
    QVERIFY(text.contains("File: C:/music/x.wav"));
    QVERIFY(text.contains("Trace: t-9"));

    /* subtitle 半边：autoSkip 关的后缀文案。 */
    fake.state.subtitle = "FILE IS DAMAGED - CANNOT PLAY - STOPPED";
    inject(w.get(), &fake);
    QCOMPARE(w->findChild<QLabel *>("mNowPlayingSubtitle")->text(),
             QString("FILE IS DAMAGED - CANNOT PLAY - STOPPED"));
}

void AppUiTests::e3_recoverable_error_carries_position()
{
    FakeCommandPort fake;
    std::unique_ptr<MainWindow> w(makeWindow(&fake));
    m_lastWindow = w.get();

    const QString subtitle = PlayerController::composeErrorSubtitle(
        soundbridge::ErrorCode::DataTruncated, true, 3000, 0, QString());
    QCOMPARE(subtitle, QString("FILE MAY BE INCOMPLETE AT 00:03 - SKIPPED"));

    fake.state.subtitle = subtitle;
    inject(w.get(), &fake);
    QVERIFY(w->findChild<QLabel *>("mNowPlayingSubtitle")->text().contains(" AT 00:03"));
}

void AppUiTests::e4_unrecoverable_codes_never_carry_position()
{
    for (const int codeValue : kUnrecoverableCodes) {
        const QString subtitle = PlayerController::composeErrorSubtitle(
            static_cast<soundbridge::ErrorCode>(codeValue), true, 5000, 0, QString());
        QVERIFY2(!subtitle.contains(" AT "),
                 qPrintable(QString("code %1: %2").arg(codeValue).arg(subtitle)));
    }
}

void AppUiTests::e5_batch_summary_is_uppercase_detail_only()
{
    FakeCommandPort fake;
    std::unique_ptr<MainWindow> w(makeWindow(&fake));
    m_lastWindow = w.get();

    const QString subtitle = PlayerController::composeErrorSubtitle(
        soundbridge::ErrorCode::ExtractorUnsupported, true, 0, -1, "3 files cannot be played");
    QCOMPARE(subtitle, QString("3 FILES CANNOT BE PLAYED"));

    fake.state.subtitle = subtitle;
    inject(w.get(), &fake);
    QCOMPARE(w->findChild<QLabel *>("mNowPlayingSubtitle")->text(),
             QString("3 FILES CANNOT BE PLAYED"));
}

void AppUiTests::e6_six_reason_texts_are_distinct_and_exact()
{
    QSet<QString> distinct;
    QMapIterator<int, QString> it(kReasonTexts);
    while (it.hasNext()) {
        it.next();
        const QString actual = PlayerController::errorReasonText(
            static_cast<soundbridge::ErrorCode>(it.key()));
        QCOMPARE(actual, it.value());
        distinct.insert(actual);
    }
    QCOMPARE(distinct.size(), 6);
}

void AppUiTests::e7_error_subtitle_restored_after_next_track()
{
    FakeCommandPort fake;
    std::unique_ptr<MainWindow> w(makeWindow(&fake));
    m_lastWindow = w.get();

    fake.state.playlist = (QStringList() << "Bad" << "Next");
    fake.state.trackIds = (QStringList() << "id-bad" << "id-next");
    fake.state.currentTrackIndex = 0;
    fake.state.subtitle = "FORMAT NOT SUPPORTED - SKIPPED";
    inject(w.get(), &fake);
    QCOMPARE(w->findChild<QLabel *>("mNowPlayingSubtitle")->text(),
             QString("FORMAT NOT SUPPORTED - SKIPPED"));

    fake.state.currentTrackIndex = 1;
    fake.state.title = "Next";
    fake.state.subtitle = "IDLE";
    fake.state.playerState = static_cast<int>(soundbridge::PlayerState::Stopped);
    inject(w.get(), &fake);
    QCOMPARE(w->findChild<QLabel *>("mNowPlayingSubtitle")->text(), QString("IDLE"));
}

// ============ P 区 ============

void AppUiTests::p1_first_run_window_size_and_centering()
{
    FakeCommandPort fake;
    std::unique_ptr<MainWindow> w(makeWindow(&fake));
    m_lastWindow = w.get();

    QCOMPARE(w->size(), QSize(1200, 720));
    QScreen *screen = QGuiApplication::primaryScreen();
    QVERIFY(screen != nullptr);
    const QRect area = screen->availableGeometry();
    /* restoreWindowState 用 rect().center()：QRect::center() 是整型除法取整，
       期望值必须用同一公式，不能手算 600/360。 */
    const QRect initialRect(QPoint(0, 0), QSize(1200, 720));
    QCOMPARE(w->geometry().topLeft(), area.center() - initialRect.center());
}

void AppUiTests::p2_geometry_mode_autoskip_roundtrip()
{
    FakeCommandPort fake1;
    {
        std::unique_ptr<MainWindow> w1(makeWindow(&fake1));
        w1->show();
        QTest::qWait(30);
        w1->resize(1100, 720);
        QTest::mouseClick(btnOf(w1.get(), "btn_mode"), Qt::LeftButton);
        QTest::mouseClick(w1->findChild<QCheckBox *>("mAutoSkipCheck"), Qt::LeftButton);
        w1->close();
    }

    FakeCommandPort fake2;
    std::unique_ptr<MainWindow> w2(makeWindow(&fake2));
    m_lastWindow = w2.get();
    w2->show();
    QTest::qWait(30);
    /* 三项均恢复（UI-P2）。 */
    QCOMPARE(w2->size(), QSize(1100, 720));
    QCOMPARE(fake2.lastArgOf("setPlaybackMode"),
             QString::number(static_cast<int>(soundbridge::PlaybackMode::Loop)));
    QCOMPARE(fake2.state.autoSkipOnError, false);
}

void AppUiTests::p3_illegal_mode_falls_back_and_rewrites()
{
    seedIni("playback/mode", 99);

    FakeCommandPort fake;
    std::unique_ptr<MainWindow> w(makeWindow(&fake));
    m_lastWindow = w.get();

    QCOMPARE(fake.countOf("setPlaybackMode"), 1);
    QCOMPARE(fake.lastArgOf("setPlaybackMode"),
             QString::number(static_cast<int>(soundbridge::PlaybackMode::Sequential)));

    QSettings settings;
    QCOMPARE(settings.value("playback/mode").toInt(),
             static_cast<int>(soundbridge::PlaybackMode::Sequential));
}

void AppUiTests::p5_settings_key_set_is_exact()
{
    FakeCommandPort fake;
    std::unique_ptr<MainWindow> w(makeWindow(&fake));
    m_lastWindow = w.get();
    w->show();
    QTest::qWait(30);
    w->close(); /* closeEvent → persistWindowState 写全部键 */

    const QStringList allKeys = QSettings().allKeys();
    const QSet<QString> keys(allKeys.cbegin(), allKeys.cend());
    const QSet<QString> expected = {
        "window/geometry", "window/maximized", "playback/auto_skip_on_error",
        "playback/mode", "playback/volume", "playback/muted", "library/favorites",
    };
    QCOMPARE(keys, expected);
}

void AppUiTests::p6_minimum_size_no_overlap_and_scrollable()
{
    FakeCommandPort fake;
    std::unique_ptr<MainWindow> w(makeWindow(&fake));
    m_lastWindow = w.get();

    QStringList rows;
    for (int i = 0; i < 100; ++i) {
        rows.append(QString("Track %1").arg(i, 3, 10, QLatin1Char('0')));
    }
    fake.state.playlist = rows;
    fake.state.currentTrackIndex = 0;
    inject(w.get(), &fake);

    w->resize(960, 540);
    w->show();
    QTest::qWait(80);

    QListWidget *list = w->findChild<QListWidget *>("mListWidget");
    QSlider *slider = w->findChild<QSlider *>("mDurationSlider");
    QVERIFY(list->verticalScrollBar()->isVisible());
    QCOMPARE(list->horizontalScrollBarPolicy(), Qt::ScrollBarAlwaysOff);

    const QRect frame = w->rect();
    const auto insideWindow = [&w, &frame](QWidget *widget) {
        const QPoint topLeft = widget->mapTo(w.get(), QPoint(0, 0));
        return frame.contains(topLeft)
            && frame.contains(topLeft + QPoint(widget->width(), widget->height()));
    };
    QVERIFY(insideWindow(list));
    QVERIFY(insideWindow(slider));
    QVERIFY(insideWindow(btnOf(w.get(), "btn_play")));
    QVERIFY(insideWindow(btnOf(w.get(), "btn_volume")));
    QVERIFY(insideWindow(w->findChild<QLabel *>("mDurationLabel")));

    /* 两栏不重叠。 */
    const QRect listRect(list->mapTo(w.get(), QPoint(0, 0)), list->size());
    const QRect sliderRect(slider->mapTo(w.get(), QPoint(0, 0)), slider->size());
    QVERIFY2(!listRect.intersects(sliderRect), "left column and right column overlap");
}

// ============ V 区 ============

void AppUiTests::v1_two_columns_ordered_and_visible()
{
    FakeCommandPort fake;
    std::unique_ptr<MainWindow> w(makeWindow(&fake));
    m_lastWindow = w.get();
    w->show();
    QTest::qWait(50);

    QListWidget *list = w->findChild<QListWidget *>("mListWidget");
    QSlider *slider = w->findChild<QSlider *>("mDurationSlider");
    QVERIFY(list->isVisible());
    QVERIFY(slider->isVisible());
    QVERIFY(list->mapTo(w.get(), QPoint(0, 0)).x() < slider->mapTo(w.get(), QPoint(0, 0)).x());
    QLabel *position = w->findChild<QLabel *>("mPositionLabel");
    QLabel *duration = w->findChild<QLabel *>("mDurationLabel");
    QVERIFY(position->isVisible());
    QVERIFY(duration->isVisible());
    QVERIFY(position->mapTo(w.get(), QPoint(0, 0)).x()
            < duration->mapTo(w.get(), QPoint(0, 0)).x());
}

void AppUiTests::v2_minimum_size_scrollbar_and_bounds()
{
    FakeCommandPort fake;
    std::unique_ptr<MainWindow> w(makeWindow(&fake));
    m_lastWindow = w.get();

    QStringList rows;
    for (int i = 0; i < 100; ++i) {
        rows.append(QString("Track %1").arg(i, 3, 10, QLatin1Char('0')));
    }
    fake.state.playlist = rows;
    fake.state.currentTrackIndex = 0;
    inject(w.get(), &fake);

    w->resize(960, 540);
    w->show();
    QTest::qWait(80);

    QListWidget *list = w->findChild<QListWidget *>("mListWidget");
    QVERIFY(list->verticalScrollBar()->isVisible());
    const QRect frame = w->rect();
    const QRect listRect(list->mapTo(w.get(), QPoint(0, 0)), list->size());
    QVERIFY(frame.contains(listRect)); /* 列表完整落在最小窗口内 */
}

void AppUiTests::v4_state_colors_pairwise_distinct()
{
    /* 半自动口径（§9.7.2）：三种状态色两两可区分，不判「好看」。 */
    QVERIFY(MainWindow::kSelectBg != MainWindow::kPlayBg);
    QVERIFY(MainWindow::kPlayBg != MainWindow::kPlayBgSelected);
    QVERIFY(MainWindow::kSelectBg != MainWindow::kPlayBgSelected);
}

void AppUiTests::v5_every_transport_button_has_qss_icon()
{
    const QStringList names = {"btn_previous", "btn_play",    "btn_next", "btn_favorite",
        "btn_mode", "btn_menu", "btn_volume"};
    const QString qss = qssText();
    QVERIFY(!qss.isEmpty());
    for (const QString &name : names) {
        QVERIFY2(qss.contains(QString("QPushButton#%1").arg(name)),
                 qPrintable(QString("missing qss rule for %1").arg(name)));
    }
    QVERIFY(qss.contains("border-image"));
}

void AppUiTests::v6_qss_selectors_stay_within_anchors()
{
    const QString qss = qssText();
    QVERIFY(!qss.isEmpty());

    /* ① 每个 id 选择器都必须是 §1.3 锚点。 */
    QRegularExpression idPattern("#([A-Za-z_][A-Za-z0-9_]*)");
    QRegularExpressionMatchIterator it = idPattern.globalMatch(qss);
    QSet<QString> ids;
    while (it.hasNext()) {
        ids.insert(it.next().captured(1));
    }
    const QSet<QString> anchors(kAnchors.cbegin(), kAnchors.cend());
    QVERIFY2((ids - anchors).isEmpty(),
             qPrintable("selectors outside anchors: "
                        + QStringList((ids - anchors).values()).join(", ")));

    /* ② mListMask 已随死代码删除作废（§7 更正四），不得回流。 */
    QVERIFY(!qss.contains("listMask"));
    /* ③ 已知拼写错误不得回流（§UI-V6 现状偏差记录）。 */
    QVERIFY(!qss.contains("QListWidgetlistWidget"));
    /* ④ 子控件伪元素必须双冒号。 */
    QRegularExpression singleColon("(?<!:):(handle|sub-page|add-line|sub-line)\\b");
    QVERIFY2(!singleColon.match(qss).hasMatch(), "single-colon pseudo element found");

    /* ⑤ 19 个锚点全部落在实际窗口树里。 */
    FakeCommandPort fake;
    std::unique_ptr<MainWindow> w(makeWindow(&fake));
    m_lastWindow = w.get();
    for (const QString &anchor : kAnchors) {
        QVERIFY2(w->findChild<QObject *>(anchor.toUtf8().constData()) != nullptr,
                 qPrintable(QString("anchor missing in window: %1").arg(anchor)));
    }
}

void AppUiTests::v7_qrc_files_exist_on_disk()
{
    QFile qrc(QString(SB_APP_SOURCE_DIR) + "/res.qrc");
    QVERIFY(qrc.open(QIODevice::ReadOnly | QIODevice::Text));
    QXmlStreamReader xml(&qrc);
    QStringList files;
    while (!xml.atEnd()) {
        if (xml.readNext() == QXmlStreamReader::StartElement && xml.name() == QLatin1String("file")) {
            files.append(xml.readElementText());
        }
    }
    QVERIFY(!files.isEmpty());
    for (const QString &relative : files) {
        QVERIFY2(QFileInfo(QString(SB_APP_SOURCE_DIR) + "/" + relative).exists(),
                 qPrintable(QString("qrc file missing on disk: %1").arg(relative)));
    }
}

void AppUiTests::v9_popup_geometry_within_bounds_and_off_anchor()
{
    FakeCommandPort fake;
    std::unique_ptr<MainWindow> w(makeWindow(&fake));
    m_lastWindow = w.get();
    w->show();
    QTest::qWait(50);

    QScreen *screen = QGuiApplication::primaryScreen();
    QVERIFY(screen != nullptr);
    const QRect area = screen->availableGeometry();

    auto checkPopup = [&](VolumePopup *popup) {
        QVERIFY(popup != nullptr && popup->isVisible());
        const QRect global(popup->mapToGlobal(QPoint(0, 0)), popup->size());
        QVERIFY2(area.contains(global), "popup escapes available geometry");
        QWidget *anchor = btnOf(w.get(), "btn_volume");
        const QRect anchorGlobal(anchor->mapToGlobal(QPoint(0, 0)), anchor->size());
        QVERIFY2(!global.intersects(anchorGlobal), "popup covers its anchor button");
    };

    QVERIFY(QMetaObject::invokeMethod(w.get(), "btn_volume_clicked"));
    VolumePopup *popup = w->findChild<VolumePopup *>("mVolumePopup");
    checkPopup(popup);
    QTest::keyClick(popup, Qt::Key_Escape);
    QTest::qWait(20);
    QVERIFY(!popup->isVisible());

    /* 960×540 复跑（UI-V9 判定列）。 */
    w->resize(960, 540);
    QTest::qWait(30);
    QVERIFY(QMetaObject::invokeMethod(w.get(), "btn_volume_clicked"));
    checkPopup(popup);
    popup->hide();
    QTest::qWait(20);
}

void AppUiTests::v10_popup_flags_esc_and_focus_return()
{
    FakeCommandPort fake;
    std::unique_ptr<MainWindow> w(makeWindow(&fake));
    m_lastWindow = w.get();
    w->show();
    QTest::qWait(50);

    VolumePopup *volume = w->findChild<VolumePopup *>("mVolumePopup");
    PlaylistPopup *panel = w->findChild<PlaylistPopup *>("mPlaylistPopup");
    QVERIFY(volume->windowFlags().testFlag(Qt::Popup));
    QVERIFY(panel->windowFlags().testFlag(Qt::Popup));

    QVERIFY(QMetaObject::invokeMethod(w.get(), "btn_volume_clicked"));
    QVERIFY(volume->isVisible());
    QTest::keyClick(volume, Qt::Key_Escape);
    QTest::qWait(50);
    QVERIFY(!volume->isVisible());
    /* 关闭后焦点回到触发按钮（ADR 0015 / UI-V10）。offscreen 无活动窗口，
       用窗口内焦点链（QWidget::focusWidget）而不是 QApplication 全局焦点。 */
    QCOMPARE(w->focusWidget(), static_cast<QWidget *>(btnOf(w.get(), "btn_volume")));
}

// ============ F 区 ============

void AppUiTests::f1_favorite_checkable_and_two_qss_branches()
{
    FakeCommandPort fake;
    std::unique_ptr<MainWindow> w(makeWindow(&fake));
    m_lastWindow = w.get();

    QPushButton *favorite = btnOf(w.get(), "btn_favorite");
    QVERIFY(favorite->isCheckable());
    const QString qss = qssText();
    QVERIFY(qss.contains("QPushButton#btn_favorite")); /* 默认分支 */
    QVERIFY(qss.contains("QPushButton#btn_favorite:checked")); /* 已收藏分支 */
    QVERIFY(qss.contains("btn_favorite_no.png"));
    QVERIFY(qss.contains("btn_favorite_yes.png"));
}

void AppUiTests::f2_favorite_survives_restart()
{
    FakeCommandPort fake1;
    {
        std::unique_ptr<MainWindow> w1(makeWindow(&fake1));
        w1->show();
        QTest::qWait(30);
        fake1.state.playlist = (QStringList() << "Track A");
        fake1.state.trackIds = (QStringList() << "track-a");
        fake1.state.currentTrackIndex = 0;
        fake1.state.title = "Track A";
        inject(w1.get(), &fake1);
        QVERIFY(btnOf(w1.get(), "btn_favorite")->isEnabled());
        QTest::mouseClick(btnOf(w1.get(), "btn_favorite"), Qt::LeftButton);
        QCOMPARE(btnOf(w1.get(), "btn_favorite")->isChecked(), true);
        QSettings settings;
        QVERIFY(settings.value("library/favorites").toStringList().contains("track-a"));
        w1->close();
    }

    FakeCommandPort fake2;
    std::unique_ptr<MainWindow> w2(makeWindow(&fake2));
    m_lastWindow = w2.get();
    fake2.state.playlist = (QStringList() << "Track A");
    fake2.state.trackIds = (QStringList() << "track-a");
    fake2.state.currentTrackIndex = 0;
    fake2.state.title = "Track A";
    inject(w2.get(), &fake2);
    QCOMPARE(btnOf(w2.get(), "btn_favorite")->isChecked(), true);
}

void AppUiTests::f3_favorite_marker_follows_track_switch()
{
    FakeCommandPort fake;
    std::unique_ptr<MainWindow> w(makeWindow(&fake));
    m_lastWindow = w.get();
    w->show();
    QTest::qWait(30);

    fake.state.playlist = (QStringList() << "A" << "B");
    fake.state.trackIds = (QStringList() << "id-a" << "id-b");
    fake.state.currentTrackIndex = 0;
    fake.state.title = "A";
    inject(w.get(), &fake);
    QTest::mouseClick(btnOf(w.get(), "btn_favorite"), Qt::LeftButton);
    QCOMPARE(btnOf(w.get(), "btn_favorite")->isChecked(), true);

    fake.state.currentTrackIndex = 1;
    fake.state.title = "B";
    inject(w.get(), &fake);
    QCOMPARE(btnOf(w.get(), "btn_favorite")->isChecked(), false);

    fake.state.currentTrackIndex = 0;
    fake.state.title = "A";
    inject(w.get(), &fake);
    QCOMPARE(btnOf(w.get(), "btn_favorite")->isChecked(), true);
}

void AppUiTests::f4_empty_id_disables_and_does_not_write()
{
    FakeCommandPort fake;
    std::unique_ptr<MainWindow> w(makeWindow(&fake));
    m_lastWindow = w.get();

    /* 无当前曲目（index -1）：禁用 + 未勾选。 */
    QVERIFY(!btnOf(w.get(), "btn_favorite")->isEnabled());
    QVERIFY(!btnOf(w.get(), "btn_favorite")->isChecked());

    QVERIFY(QMetaObject::invokeMethod(w.get(), "btn_favorite_clicked"));
    QSettings settings;
    QVERIFY(!settings.contains("library/favorites"));
}

// ============ VOL 区 ============

void AppUiTests::vol3_volume_persisted_and_restored()
{
    seedIni("playback/volume", 60);

    FakeCommandPort fake;
    std::unique_ptr<MainWindow> w(makeWindow(&fake));
    m_lastWindow = w.get();

    QCOMPARE(fake.countOf("setVolume"), 1);
    QCOMPARE(fake.lastArgOf("setVolume"), QString("60"));
    QCOMPARE(btnOf(w.get(), "btn_volume")->toolTip(), QString("Volume: 60%"));

    QVERIFY(QMetaObject::invokeMethod(w.get(), "btn_volume_clicked"));
    QCOMPARE(w->findChild<QSlider *>("mVolumeSlider")->value(), 60);
    w->findChild<VolumePopup *>("mVolumePopup")->hide();
    QTest::qWait(20);

    w->close();
    QCOMPARE(QSettings().value("playback/volume").toInt(), 60);
}

void AppUiTests::vol4_mute_toggle_keeps_volume_value()
{
    seedIni("playback/volume", 87);

    FakeCommandPort fake;
    std::unique_ptr<MainWindow> w(makeWindow(&fake));
    m_lastWindow = w.get();
    w->show();
    QTest::qWait(30);
    QCOMPARE(fake.countOf("setVolume"), 1);
    QCOMPARE(fake.lastArgOf("setVolume"), QString("87"));
    const int volumeCallsAtStart = fake.countOf("setVolume");
    const int mutedCallsAtStart = fake.countOf("setMuted"); /* 构造期 restoreWindowState 调过一次 */

    QVERIFY(QMetaObject::invokeMethod(w.get(), "btn_volume_clicked"));
    QPushButton *mute = w->findChild<QPushButton *>("btn_mute");
    QVERIFY(mute != nullptr);
    mute->setChecked(true); /* mutedChanged(true) → persistVolumeState 立即写盘 */
    QTest::qWait(20);
    QCOMPARE(fake.countOf("setMuted"), mutedCallsAtStart + 1);
    QCOMPARE(fake.muted(), true);
    QCOMPARE(QSettings().value("playback/muted").toBool(), true);
    /* 静音全程不改音量。 */
    QCOMPARE(fake.countOf("setVolume"), volumeCallsAtStart);
    QCOMPARE(fake.volume(), 87);

    mute->setChecked(false);
    QTest::qWait(20);
    QCOMPARE(fake.countOf("setVolume"), volumeCallsAtStart);
    QCOMPARE(fake.countOf("setMuted"), mutedCallsAtStart + 2);
    QCOMPARE(QSettings().value("playback/muted").toBool(), false);
    QCOMPARE(QSettings().value("playback/volume").toInt(), 87);
}

// ============ M 区 ============

void AppUiTests::m1_menu_enabled_follows_playlist()
{
    FakeCommandPort fake;
    std::unique_ptr<MainWindow> w(makeWindow(&fake));
    m_lastWindow = w.get();

    QCOMPARE(btnOf(w.get(), "btn_menu")->isEnabled(), false);
    fake.state.playlist = (QStringList() << "A" << "B");
    inject(w.get(), &fake);
    QCOMPARE(btnOf(w.get(), "btn_menu")->isEnabled(), true);
}

void AppUiTests::m2_panel_rows_and_bold_current()
{
    FakeCommandPort fake;
    std::unique_ptr<MainWindow> w(makeWindow(&fake));
    m_lastWindow = w.get();
    w->show();
    QTest::qWait(30);

    fake.state.playlist = (QStringList() << "A" << "B" << "C");
    fake.state.currentTrackIndex = 1;
    inject(w.get(), &fake);
    QVERIFY(QMetaObject::invokeMethod(w.get(), "btn_playList_clicked"));

    QListWidget *panelList = w->findChild<QListWidget *>("mPanelList");
    QVERIFY(panelList != nullptr && panelList->isVisible());
    QCOMPARE(panelList->count(), w->findChild<QListWidget *>("mListWidget")->count());
    QVERIFY(panelList->item(1)->font().bold());
    QCOMPARE(panelList->item(1)->foreground().color(), QColor(94, 220, 243));
    QVERIFY(!panelList->item(0)->font().bold());
}

void AppUiTests::m3_panel_row_click_selects_and_closes()
{
    FakeCommandPort fake;
    std::unique_ptr<MainWindow> w(makeWindow(&fake));
    m_lastWindow = w.get();
    w->show();
    QTest::qWait(30);

    fake.state.playlist = (QStringList() << "A" << "B" << "C" << "D");
    fake.state.currentTrackIndex = 0;
    inject(w.get(), &fake);
    QVERIFY(QMetaObject::invokeMethod(w.get(), "btn_playList_clicked"));
    PlaylistPopup *panel = w->findChild<PlaylistPopup *>("mPlaylistPopup");
    QVERIFY(panel->isVisible());

    QListWidget *panelList = w->findChild<QListWidget *>("mPanelList");
    QVERIFY(QMetaObject::invokeMethod(panel, "listItemClicked",
                                      Q_ARG(QListWidgetItem *, panelList->item(2))));
    QCOMPARE(fake.countOf("selectTrack"), 1);
    QCOMPARE(fake.lastArgOf("selectTrack"), QString("2"));
    QTest::qWait(20);
    QVERIFY(!panel->isVisible());

    /* 主列表选中同步（UI-M3）。 */
    fake.state.currentTrackIndex = 2;
    fake.state.title = "C";
    inject(w.get(), &fake);
    QCOMPARE(w->findChild<QListWidget *>("mListWidget")->currentRow(), 2);
}

void AppUiTests::m4_panel_highlight_follows_next()
{
    FakeCommandPort fake;
    std::unique_ptr<MainWindow> w(makeWindow(&fake));
    m_lastWindow = w.get();
    w->show();
    QTest::qWait(30);

    QStringList rows;
    for (int i = 0; i < 40; ++i) {
        rows.append(QString("Track %1").arg(i, 2, 10, QLatin1Char('0')));
    }
    fake.state.playlist = rows;
    fake.state.currentTrackIndex = 0;
    inject(w.get(), &fake);
    QVERIFY(QMetaObject::invokeMethod(w.get(), "btn_playList_clicked"));
    QListWidget *panelList = w->findChild<QListWidget *>("mPanelList");

    fake.state.currentTrackIndex = 30;
    inject(w.get(), &fake); /* 面板可见时跟随（§8.6）：updateCurrentIndex */
    QVERIFY(panelList->item(30)->font().bold());
    QVERIFY(!panelList->item(0)->font().bold());
    QVERIFY(panelList->visualItemRect(panelList->item(30)).intersects(panelList->viewport()->rect()));
}

void AppUiTests::m5_both_popups_are_qt_popup_and_esc_closes()
{
    FakeCommandPort fake;
    std::unique_ptr<MainWindow> w(makeWindow(&fake));
    m_lastWindow = w.get();
    w->show();
    QTest::qWait(30);

    fake.state.playlist = (QStringList() << "A" << "B" << "C");
    fake.state.currentTrackIndex = 0;
    inject(w.get(), &fake);

    VolumePopup *volume = w->findChild<VolumePopup *>("mVolumePopup");
    PlaylistPopup *panel = w->findChild<PlaylistPopup *>("mPlaylistPopup");
    QVERIFY(volume->windowFlags().testFlag(Qt::Popup));
    QVERIFY(panel->windowFlags().testFlag(Qt::Popup));

    QVERIFY(QMetaObject::invokeMethod(w.get(), "btn_volume_clicked"));
    QVERIFY(volume->isVisible());
    QTest::keyClick(volume, Qt::Key_Escape);
    QTest::qWait(20);
    QVERIFY(!volume->isVisible());

    QVERIFY(QMetaObject::invokeMethod(w.get(), "btn_playList_clicked"));
    QVERIFY(panel->isVisible());
    QTest::keyClick(panel, Qt::Key_Escape);
    QTest::qWait(20);
    QVERIFY(!panel->isVisible());
}

int main(int argc, char *argv[])
{
    QCoreApplication::setOrganizationName("SoundBridge");
    QCoreApplication::setApplicationName("SoundBridge");
    QApplication app(argc, argv);

    /* MainWindow 内部会调用 LogPrintf——未初始化时 LogApi 兜底建固定名
       "./log"（ctest -j 撞名 EIO 的老坑），所以测试必须抢先初始化 pid 化目录。 */
    const QString logRoot = pidLogRoot();
    QDir().mkpath(logRoot);
    soundbridge::LogConfig logConfig;
    logConfig.directory = logRoot.toStdString();
    logConfig.filePrefix = "app_ui_tests";
    soundbridge::InitializeLogging(logConfig);

    /* QSettings 隔离（§9.7.3）：只有 IniFormat 下 setPath 才生效；
       Windows 默认 NativeFormat 会写注册表。 */
    QSettings::setDefaultFormat(QSettings::IniFormat);
    const QString tmpRoot
        = QDir::tempPath() + QString("/app_ui_tests_%1").arg(sb_test::processId());
    QDir().mkpath(tmpRoot);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, tmpRoot);

    qRegisterMetaType<PlayerViewState>("PlayerViewState");

    AppUiTests tc;
    const int rc = QTest::qExec(&tc, argc, argv);
    QDir(tmpRoot).removeRecursively();
    return rc;
}

#include "AppUiTests.moc"
