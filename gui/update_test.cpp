#include "updates.h"
#include <QAction>
#include <QApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QMainWindow>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QTemporaryDir>
#include <QThread>
#include <functional>

static bool waitFor(const std::function<bool()> &ready) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 10000) {
        QApplication::processEvents();
        if (ready()) return true;
        QThread::msleep(20);
    }
    return false;
}

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    if (app.arguments().size() != 2) return 1;
    QTemporaryDir dir;
    if (!dir.isValid()) return 2;
    QCoreApplication::setOrganizationName("Disk-Diver-Tests");
    QCoreApplication::setApplicationName("UpdateFlow");
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, dir.path());
    QFile script(dir.filePath("updater.py"));
    if (!script.open(QIODevice::WriteOnly)) return 3;
    script.write(R"PY(import json, sys
from pathlib import Path
root = Path(__file__).parent
if sys.argv[1:] == ["fetch"]:
    package = root / "package.exe"
    package.write_bytes(b"test fixture")
    print(json.dumps(dict(status="ready", version="9.0.0", path=str(package))))
else:
    assert sys.argv[1:] == ["verify", str(root / "package.exe")]
    print(json.dumps(dict(status="error", message="test: signature invalid")))
    sys.exit(1)
)PY");
    script.close();
    QMainWindow window;
    setupUpdates(window, dir.path(), app.arguments().at(1), true);
    window.show();
    auto visibleDialog = [&]() -> QMessageBox * {
        for (auto *dialog : window.findChildren<QMessageBox *>())
            if (dialog->isVisible()) return dialog;
        return nullptr;
    };
    if (!waitFor([&] { return visibleDialog() != nullptr; })) return 4;
    auto *offer = visibleDialog();
    if (!offer->text().contains("9.0.0")) return 5;
    for (auto *button : offer->buttons())
        if (offer->buttonRole(button) == QMessageBox::RejectRole) button->click();
    if (!waitFor([&] { return visibleDialog() == nullptr; })) return 6;
    for (auto *action : window.findChildren<QAction *>())
        if (action->isVisible() && action->text() == "Install Downloaded Update…") action->trigger();
    if (!waitFor([&] { return visibleDialog() && visibleDialog()->text().contains("signature invalid"); })) return 7;
    if (QSettings().contains("updates/package") || !window.isVisible()) return 8;
    return 0;
}
