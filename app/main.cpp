#include "mainwindow.h"

#include "soundbridge/logging.h"

#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QMessageLogContext>
#include <cstdlib>

namespace {

constexpr const char *kTag = "App.Main";

soundbridge::LogLevel toSdkLevel(QtMsgType type)
{
    switch (type) {
    case QtDebugMsg:
        return soundbridge::LogLevel::Debug;
    case QtInfoMsg:
        return soundbridge::LogLevel::Info;
    case QtWarningMsg:
        return soundbridge::LogLevel::Warning;
    case QtCriticalMsg:
        return soundbridge::LogLevel::Error;
    case QtFatalMsg:
        return soundbridge::LogLevel::Fatal;
    }
    return soundbridge::LogLevel::Info;
}

void qtToSdkMessageHandler(QtMsgType type, const QMessageLogContext &context, const QString &msg)
{
    const QString fileName = context.file ? QString::fromUtf8(context.file) : QString("-");
    const QString functionName
        = context.function ? QString::fromUtf8(context.function) : QString("-");
    const QString finalMessage
        = QString("[%1:%2] [%3] %4").arg(fileName).arg(context.line).arg(functionName).arg(msg);

    soundbridge::LogMessage(toSdkLevel(type), kTag, finalMessage.toUtf8().constData());

    if (type == QtFatalMsg) {
        abort();
    }
}

void initSdkLogSystem()
{
    const QString logDir = QCoreApplication::applicationDirPath() + "/log";
    QDir().mkpath(logDir);

    soundbridge::LogConfig config;
    config.directory           = logDir.toStdString();
    config.filePrefix          = "soundbridge";
    config.singleFileSizeBytes = 10 * 1024 * 1024;
    config.maxFileCount        = 20;
    soundbridge::InitializeLogging(config);

    qInstallMessageHandler(qtToSdkMessageHandler);
    soundbridge::LogMessage(soundbridge::LogLevel::Info, kTag, "sdk log system initialized");
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
    QCoreApplication::setAttribute(Qt::AA_UseHighDpiPixmaps);
    QCoreApplication::setOrganizationName("SoundBridge");
    QCoreApplication::setApplicationName("SoundBridge");

    QApplication a(argc, argv);
    initSdkLogSystem();

    QFile file(":/style.qss");
    if (file.exists()) {
        if (file.open(QFile::ReadOnly)) {
            const QString styleSheet = QString::fromUtf8(file.readAll());
            a.setStyleSheet(styleSheet);
            file.close();
        } else {
            soundbridge::LogPrintf(soundbridge::LogLevel::Warning, "App",
                                   "Failed to open style sheet: %s",
                                   file.errorString().toUtf8().constData());
        }
    } else {
        soundbridge::LogMessage(soundbridge::LogLevel::Warning, "App", "Style sheet not found");
    }

    MainWindow w;
    w.show();

    return a.exec();
}
