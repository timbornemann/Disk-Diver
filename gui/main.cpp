#include "window.h"
#include <QApplication>
#include <QIcon>
#ifdef Q_OS_WIN
#include <objbase.h>
#endif

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    QCoreApplication::setApplicationName("Disk-Diver");
    QApplication::setWindowIcon(QIcon(QStringLiteral(":/icons/disk-diver.png")));
#ifdef Q_OS_LINUX
    QGuiApplication::setDesktopFileName(QStringLiteral("disk-diver"));
#endif
#ifdef Q_OS_WIN
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
#endif
    auto window = createWindow(app.arguments().size() == 2 ? app.arguments().at(1) : QString{});
    window->show();
    const int result = app.exec();
    window.reset();
#ifdef Q_OS_WIN
    if (SUCCEEDED(com)) CoUninitialize();
#endif
    return result;
}
