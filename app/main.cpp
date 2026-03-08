#include "mainwindow.h"

#include "LogApi.h"

#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QMessageLogContext>
#include <cstdlib>

namespace {

sdk::SdkLogLevel toSdkLevel(QtMsgType type)
{
    switch (type) {
    case QtDebugMsg:
        return sdk::SdkLogLevel::Debug;
    case QtInfoMsg:
        return sdk::SdkLogLevel::Info;
    case QtWarningMsg:
        return sdk::SdkLogLevel::Warning;
    case QtCriticalMsg:
        return sdk::SdkLogLevel::Error;
    case QtFatalMsg:
        return sdk::SdkLogLevel::Fatal;
    }
    return sdk::SdkLogLevel::Info;
}

void qtToSdkMessageHandler(QtMsgType type, const QMessageLogContext &context, const QString &msg)
{
    const QString fileName = context.file ? QString::fromUtf8(context.file) : QString("-");
    const QString functionName = context.function ? QString::fromUtf8(context.function) : QString("-");
    const QString finalMessage = QString("[%1:%2] [%3] %4")
                                     .arg(fileName)
                                     .arg(context.line)
                                     .arg(functionName)
                                     .arg(msg);

    sdk::LogMessage(toSdkLevel(type), "Qt", finalMessage.toUtf8().constData());

    if (type == QtFatalMsg) {
        abort();
    }
}

void initSdkLogSystem()
{
    const QString logDir = QCoreApplication::applicationDirPath() + "/log";
    QDir().mkpath(logDir);

    sdk::SdkLogConfig config;
    config.directory = logDir.toStdString();
    config.filePrefix = "soundbridge";
    config.singleFileSizeBytes = 10 * 1024 * 1024;
    config.maxFileCount = 20;
    sdk::InitializeLogging(config);

    qInstallMessageHandler(qtToSdkMessageHandler);
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
            qWarning() << "Failed to open style sheet:" << file.errorString();
        }
    } else {
        qWarning() << "Style sheet not found";
    }

    MainWindow w;
    w.show();

    return a.exec();
}


