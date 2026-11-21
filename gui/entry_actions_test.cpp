#include "entry_actions.h"
#include "treemap.h"
#include "window.h"

#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QElapsedTimer>
#include <QFile>
#include <QInputDialog>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QScopeGuard>
#include <QTableView>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <iostream>
#include <stdexcept>

#ifndef Q_OS_WIN
#include <fcntl.h>
#include <unistd.h>
#endif

namespace {
void require(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}

void writeFile(const QString &path) {
    QFile file(path);
    require(file.open(QIODevice::WriteOnly | QIODevice::NewOnly) && file.write("fixture", 7) == 7, "fixture write failed");
}

// Dispatch a real context-menu event and select the live QAction with the keyboard.
void choose(QWidget *view, QPoint pos, const QString &action, bool enabled = true,
            std::function<void(QDialog *)> answer = {}) {
    bool found = false, correctState = false;
    QTimer::singleShot(0, [&] {
        auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
        if (!menu) return;
        for (auto *a : menu->actions()) {
            if (a->text() != action) continue;
            found = true;
            correctState = a->isEnabled() == enabled;
            if (!enabled || !correctState) break;
            if (answer) QTimer::singleShot(0, [answer] {
                auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
                if (dialog) answer(dialog);
            });
            menu->setActiveAction(a);
            QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
            QApplication::sendEvent(menu, &enter);
            return;
        }
        menu->close();
    });
    QContextMenuEvent open(QContextMenuEvent::Mouse, pos, view->mapToGlobal(pos));
    QApplication::sendEvent(view, &open);
    // Consume the timer if the event was ignored before a menu opened.
    QApplication::processEvents();
    require(found && correctState, "context action missing or has incorrect enabled state");
}

void testActions() {
    QTemporaryDir fixture;
    const QString root = fixture.filePath("tree");
    const QString parent = root + "/parent";
    require(QDir().mkpath(parent), "fixture directory missing");
    require(QDir().mkpath(fixture.filePath("share")), "fixture data directory missing");
    qputenv("XDG_DATA_HOME", QFile::encodeName(fixture.filePath("share")));
    qputenv("XDG_CACHE_HOME", QFile::encodeName(fixture.filePath("cache")));
    writeFile(parent + "/first");
    writeFile(parent + "/second");
#ifndef Q_OS_WIN
    require(QFile::link(parent + "/first", parent + "/linked"), "could not create fixture symlink");
    const auto nativeName = QFile::encodeName(parent) + "/bad-\xff";
    const int raw = open(nativeName.constData(), O_CREAT | O_EXCL | O_WRONLY, 0600);
    require(raw >= 0, "could not create native filename");
    close(raw);
    const auto clearNative = qScopeGuard([&] { unlink(nativeName.constData()); });
#endif
    auto window = createWindow(root, false);
    window->show();
    auto *table = window->findChild<QTableView *>();
    Treemap *map = nullptr;
    for (auto *widget : window->findChildren<QWidget *>())
        if ((map = dynamic_cast<Treemap *>(widget))) break;
    require(table && map, "missing result views");
    for (auto *metric : window->findChildren<QComboBox *>())
        if (metric->findText("Logical size") >= 0) metric->setCurrentText("Logical size");
    auto wait = [&] {
        QElapsedTimer timeout;
        timeout.start();
        while (timeout.elapsed() < 10000) {
            QApplication::processEvents();
            for (auto *label : window->findChildren<QLabel *>())
                if (label->text() == "Scan complete.") return;
            QThread::msleep(5);
        }
        throw std::runtime_error("scan timeout");
    };
    auto cell = [&](const QString &name) {
        for (int row = 0; row < table->model()->rowCount(); ++row) {
            auto index = table->model()->index(row, 0);
            if (index.data().toString() == name) return index;
        }
        throw std::runtime_error("expected row missing");
    };
    auto tableAction = [&](const QString &name, const QString &action, bool enabled = true,
                           std::function<void(QDialog *)> answer = {}) {
        choose(table->viewport(), table->visualRect(cell(name)).center(), action, enabled, std::move(answer));
    };
    wait();
    // The only root row is "parent" (ID 1); a deeper hit must address one of its two files.
    QPoint descendant(-1, -1);
    for (int y = 8; y < map->height() && descendant.x() < 0; y += 12)
        for (int x = 8; x < map->width(); x += 12) {
            const auto id = map->entryAt({qreal(x), qreal(y)});
            if (id != diver::none && id > 1) { descendant = {x, y}; break; }
        }
    require(descendant.x() >= 0, "missing nested treemap hit");
    choose(map, descendant, "Copy full path");
    require(QApplication::clipboard()->text().startsWith(QDir::toNativeSeparators(parent + "/")),
            "nested map action addressed its top-level folder");
    tableAction("parent", "Open folder in treemap");
    table->selectRow(cell("second").row());
    tableAction("first", "Copy full path");
    require(QApplication::clipboard()->text() == QDir::toNativeSeparators(parent + "/first"), "copied the selected row instead of the hit");
    require(table->currentIndex().siblingAtColumn(0).data().toString() == "first",
            "context menu did not select its target row");
    tableAction("first", "Open folder in treemap", false);
#ifdef Q_OS_WIN
    const QString trash = "Move to Recycle Bin";
#else
    const QString trash = "Move to Trash";
#endif
    tableAction("first", "Rename" + QString(QChar(0x2026)), true,
                [](QDialog *d) { d->reject(); });
    const QString rename = "Rename" + QString(QChar(0x2026));
    const QString trashAction = trash + QChar(0x2026);
#ifndef Q_OS_WIN
    tableAction("linked", rename, false);
    tableAction("linked", trashAction, false);
    tableAction(QFile::decodeName("bad-\xff"), rename, false);
    tableAction(QFile::decodeName("bad-\xff"), "Copy full path", false);
#endif
    bool confirmedPath = false;
    tableAction("first", trashAction, true, [&](QDialog *d) {
        auto *box = qobject_cast<QMessageBox *>(d);
        confirmedPath = box && box->text() == QDir::toNativeSeparators(parent + "/first");
        d->reject();
    });
    require(confirmedPath && QFile::exists(parent + "/first"), "trash confirmation canceled or labeled incorrectly");
    auto renameTo = [&](QString name, bool expectError, std::function<void()> beforeAccept = {}) {
        bool sawError = false;
        tableAction("first", rename, true, [&](QDialog *d) {
            auto *input = qobject_cast<QInputDialog *>(d);
            if (!input) { d->reject(); return; }
            input->setTextValue(name);
            if (beforeAccept) beforeAccept();
            if (expectError) QTimer::singleShot(0, [&] {
                auto *error = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
                if (error) { sawError = true; error->accept(); }
            });
            input->accept();
        });
        require(sawError == expectError, "incorrect rename result");
    };
    renameTo("first", false);
    renameTo("../escape", true);
    renameTo("second", true);
    renameTo("replacement", true, [&] {
#ifdef Q_OS_WIN
        require(QFile::rename(parent + "/first", parent + "/saved"), "could not change fixture while dialog was open");
#else
        require(QFile::remove(parent + "/first"), "could not unlink fixture while dialog was open");
#endif
        writeFile(parent + "/first");
    });
    require(!QFile::exists(parent + "/replacement"), "renamed a replacement entry");
#ifdef Q_OS_WIN
    require(QFile::remove(parent + "/first") && QFile::rename(parent + "/saved", parent + "/first"), "could not restore fixture");
#else
    for (auto *button : window->findChildren<QPushButton *>()) if (button->text() == "Scan") button->click();
    wait();
#endif
    renameTo("new-name", false);
    wait();
    require(cell("new-name").isValid() && !QFile::exists(parent + "/first"), "rename or focus restoration failed");
#ifndef Q_OS_WIN
    tableAction("new-name", trashAction, true, [&](QDialog *d) {
        auto *box = qobject_cast<QMessageBox *>(d);
        if (!box) { d->reject(); return; }
        for (auto *button : box->buttons()) if (button->text() == trash) { button->click(); return; }
        d->reject();
    });
    wait();
    require(!QFile::exists(parent + "/new-name") && QFile::exists(fixture.filePath("share/Trash/files/new-name")),
            "file missing from the isolated Trash");
#endif
}
}

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    try { testActions(); }
    catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
    return 0;
}
