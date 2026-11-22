#pragma once

// ADR 0016 测试替身：记录「点哪个控件 → 下发哪条命令、参数、次数」。
//
// - 同时是 QObject：以同签名 `viewStateChanged` / `errorOccurred` 信号复用
//   MainWindow 既有的 `SLOT()` 字符串连接（AppUiPorts::playerSignals 指向本对象）。
// - `state` 是测试直接操纵的状态事实：注入渲染走
//   `window->renderPlayerViewState(fake.state)`（public slots，不改名）。
// - 不构造 PlayerController，进程零 SDL（ADR 0016 不变量）。

#include "playerport.h"

#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>

struct FakeCall {
    QString name;
    QStringList args;
};

class FakeCommandPort : public QObject, public PlayerCommandPort {
    Q_OBJECT

public:
    QList<FakeCall> calls;
    PlayerViewState state;

    int countOf(const QString &name) const
    {
        int n = 0;
        for (const FakeCall &call : calls) {
            if (call.name == name) {
                ++n;
            }
        }
        return n;
    }

    QString lastArgOf(const QString &name) const
    {
        for (int i = calls.size() - 1; i >= 0; --i) {
            if (calls.at(i).name == name) {
                return calls.at(i).args.isEmpty() ? QString() : calls.at(i).args.first();
            }
        }
        return QString();
    }

    /* 渲染驱动：等价生产侧 PlayerController::publishViewState 的信号半边。 */
    void publish() { emit viewStateChanged(state); }

    /* 错误驱动：等价 PlayerController::onError 的信号半边。 */
    void emitError(int code, const QString &detail, int trackIndex, const QString &path,
                   const QString &traceId, bool autoSkipEnabled)
    {
        emit errorOccurred(code, detail, trackIndex, path, traceId, autoSkipEnabled);
    }

    void addMusicDirectory(const std::string &path) override
    {
        calls.append(FakeCall{"addMusicDirectory", {QString::fromStdString(path)}});
    }

    void playPause() override { calls.append(FakeCall{"playPause", {}}); }

    void nextTrack() override { calls.append(FakeCall{"nextTrack", {}}); }

    void previousTrack() override { calls.append(FakeCall{"previousTrack", {}}); }

    void selectTrack(int index) override
    {
        calls.append(FakeCall{"selectTrack", {QString::number(index)}});
    }

    void seekSeconds(int seconds) override
    {
        calls.append(FakeCall{"seekSeconds", {QString::number(seconds)}});
    }

    void setPlaybackMode(soundbridge::PlaybackMode mode) override
    {
        state.playbackMode = static_cast<int>(mode);
        calls.append(FakeCall{"setPlaybackMode", {QString::number(static_cast<int>(mode))}});
    }

    soundbridge::PlaybackMode playbackMode() const override
    {
        return static_cast<soundbridge::PlaybackMode>(state.playbackMode);
    }

    void setAutoSkipOnError(bool enabled) override
    {
        state.autoSkipOnError = enabled;
        calls.append(FakeCall{"setAutoSkipOnError", {enabled ? "1" : "0"}});
    }

    bool autoSkipOnError() const override { return state.autoSkipOnError; }

    void setVolume(int percent) override
    {
        m_volume = percent;
        calls.append(FakeCall{"setVolume", {QString::number(percent)}});
    }

    int volume() const override
    {
        /* state 无音量字段（PlayerViewState 不含音量）：替身自记一份。 */
        return m_volume;
    }

    void setMuted(bool muted) override
    {
        m_muted = muted;
        calls.append(FakeCall{"setMuted", {muted ? "1" : "0"}});
    }

    bool muted() const override { return m_muted; }

    int trackCount() const override { return state.playlist.size(); }

    int currentTrackIndex() const override { return state.currentTrackIndex; }

    const PlayerViewState &viewState() const override { return state; }

signals:
    /* 与 PlayerController 同签名，复用 MainWindow 既有的 SLOT() 连接。 */
    void viewStateChanged(PlayerViewState viewState);
    void errorOccurred(int code, const QString &detail, int trackIndex, const QString &path,
                       const QString &traceId, bool autoSkipEnabled);

private:
    int m_volume = 100;
    bool m_muted = false;
};
