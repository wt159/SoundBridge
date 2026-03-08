#include "mainwindow.h"

#include <QApplication>
#include <QCoreApplication>
#include <QFile>
#include <QDebug>
#include <QScreen>

int main(int argc, char *argv[])
{
    QCoreApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
    QCoreApplication::setAttribute(Qt::AA_UseHighDpiPixmaps);
    QCoreApplication::setOrganizationName("SoundBridge");
    QCoreApplication::setApplicationName("SoundBridge");

    QApplication a(argc, argv);

    /* 加载样式�?*/
    QFile file(":/style.qss");
    if (file.exists()) {
        if (file.open(QFile::ReadOnly)) {
            /* 以字符串的方式保存读出的结果 */
            QString styleSheet = QString::fromUtf8(file.readAll());
            /* 设置全局样式 */
            a.setStyleSheet(styleSheet);
            /* 关闭文件 */
            file.close();
        } else {
            qWarning() << "无法打开样式表文�?" << file.errorString();
        }
    } else {
        /* 如果文件不存在，则打印错误信�?*/
        qWarning() << "样式表文件不存在!";
    }

    MainWindow w;
    w.show();

    return a.exec();
}
