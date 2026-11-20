#include "window.h"
#include "updates.h"
#include <QApplication>
#include <QDir>
#include <QIcon>
#include <QLocale>
#ifdef Q_OS_WIN
#include <objbase.h>
#endif

int main(int argc, char **argv) {
    QLocale::setDefault(QLocale(QLocale::English));
    QApplication app(argc, argv);
    QCoreApplication::setApplicationName("Disk-Diver");
    QCoreApplication::setOrganizationName("Disk-Diver");
    QCoreApplication::setApplicationVersion(QStringLiteral(DISK_DIVER_VERSION));
    QApplication::setWindowIcon(QIcon(QStringLiteral(":/icons/disk-diver.png")));
#ifdef Q_OS_LINUX
    QGuiApplication::setDesktopFileName(QStringLiteral("disk-diver"));
#endif
#ifdef Q_OS_WIN
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
#endif
    auto window = createWindow(app.arguments().size() == 2 ? app.arguments().at(1) : QString{});
    const QDir appDir(QCoreApplication::applicationDirPath());
#ifdef Q_OS_WIN
    const QString updatePython = appDir.filePath("python/python.exe");
#else
    const QString updatePython = QStringLiteral("/usr/bin/python3");
#endif
    setupUpdates(*window, appDir.absolutePath(), updatePython, DISK_DIVER_UPDATES_ENABLED);
    window->show();
    const int result = app.exec();
    window.reset();
#ifdef Q_OS_WIN
    if (SUCCEEDED(com)) CoUninitialize();
#endif
    return result;
}
