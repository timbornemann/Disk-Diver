#include "index.h"
#include "stream.h"
#include "treemap.h"
#include "window.h"
#include <QApplication>
#include <QComboBox>
#include <QElapsedTimer>
#include <QFile>
#include <QLabel>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QSplitter>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMenu>
#include <QAbstractButton>
#include <QTableView>
#include <QTemporaryDir>
#include <QThread>
#include <QToolButton>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}

void testIndex() {
    using diver::Frame;
    QByteArray promoted = QByteArrayLiteral("DDV1\x08\x05");
    require(diver::frameHeader(promoted, false, false) == Frame::Invalid,
            "accepted an unchanged marker without preview");
    require(diver::frameHeader(promoted, true, true) == Frame::Invalid,
            "accepted a marker after the final dataset");
    require(diver::frameHeader(QByteArrayView(promoted).first(5), true, false) == Frame::More,
            "split unchanged marker rejected");
    require(diver::frameHeader(promoted, true, false) == Frame::VerifiedPreview,
            "could not verify completed preview");
    promoted[5] = 1;
    require(diver::frameHeader(promoted, true, false) == Frame::Invalid,
            "accepted extra records under unchanged marker");
    diver::Index index;
    auto add = [&](diver::Kind kind, quint64 parent, quint64 logical, quint64 allocated,
                   quint64 inode, QString name) {
#ifdef Q_OS_WIN
        QByteArray bytes(reinterpret_cast<const char *>(name.utf16()), name.size() * 2);
#else
        QByteArray bytes = name.toUtf8();
#endif
        require(index.append(kind, parent, logical, allocated, {1, inode}, bytes.constData(), bytes.size()),
                "could not add record to index");
    };
    add(diver::Directory, diver::unknown, 0, 0, 1, {});
    add(diver::Directory, 0, 0, 0, 2, "z");
    add(diver::File, 1, 100, 4096, 3, "linked");
    index.flush();
    add(diver::Directory, 0, 0, 0, 4, "a");
    add(diver::File, 3, 100, 4096, 3, "linked");
    add(diver::File, 3, 10, diver::unknown, 0, "unknown");
    add(diver::File, 3, 20, 20, 0, "unidentified");
    add(diver::Error, 1, 0, 0, 0, {});
    index.finish();
    require(index[0].allocated == 4116 && index[0].logical == 130, "incorrect root sums");
    require(index[1].allocated == 0 && index[1].logical == 0 && index[2].alias,
            "later, lexicographically first hardlink did not take ownership");
    require(index.aliasSizes(2).logical == 100 && index.aliasSizes(2).allocated == 4096,
            "hardlink's original sizes unavailable");
    require(index[0].errors == 1 && index[0].missingSizes == 1, "incomplete scan lost error counts");
    require(index.findRelative(index.relativeName(4)) == 4, "native path roundtrip failed");
}

void makeFile(const QString &path, int size) {
    QFile f(path);
    require(f.open(QIODevice::WriteOnly), "cannot create fixture");
    require(f.write(QByteArray(size, 'd')) == size, "cannot write fixture");
}

void testWindow() {
    QTemporaryDir fixture;
    require(fixture.isValid(), "temporary directory unavailable");
    auto dir = fixture.path() + "/data";
    require(QDir().mkpath(dir + "/nested"), "cannot create fixture directory");
    auto cache = QFile::encodeName(fixture.path() + "/cache");
    qputenv("XDG_CACHE_HOME", cache);
    qputenv("LOCALAPPDATA", cache);
    makeFile(dir + "/nested/leaf", 23);
    makeFile(dir + "/flat", 11);
    ViewTiming timing;
    auto window = createWindow(dir, true, &timing);
    window->show();
    auto *table = window->findChild<QTableView *>();
    auto button = [&](const QString &label) -> QAbstractButton * {
        for (auto *widget : window->findChildren<QAbstractButton *>())
            if (widget->text() == label) return widget;
        throw std::runtime_error("required button missing");
    };
    auto wait = [&] {
        auto *start = button("Scan");
        QElapsedTimer deadline;
        deadline.start();
        while (!start->isEnabled() && deadline.elapsed() < 12000) {
            QApplication::processEvents();
            QThread::msleep(5);
        }
        for (auto *label : window->findChildren<QLabel *>())
            if (label->text().startsWith("Scan complete.")) return;
        for (auto *label : window->findChildren<QLabel *>())
            std::cerr << label->text().toStdString() << '\n';
        throw std::runtime_error("normal scan did not finish");
    };
    auto findChildRow = [&](const QString &name) {
        for (int row = 0; row < table->model()->rowCount(); ++row) {
            auto cell = table->model()->index(row, 0);
            if (cell.data().toString() == name) return cell;
        }
        throw std::runtime_error("expected scan entry missing");
    };
    // The command-line scan is already running before the window is shown.
    require(button("Cancel")->isVisible() && !button("Scan")->isVisible(),
            "cancel must replace the scan action during an operation");
    wait();
    require(!button("Cancel")->isVisible() && button("Scan")->isVisible(),
            "idle scan controls did not return");
    require(table->model()->headerData(1, Qt::Horizontal).toString() == "Allocated"
            && table->model()->headerData(2, Qt::Horizontal).toString() == "Logical",
            "size column labels are incorrect");
    auto *split = window->findChild<QSplitter *>();
    require(split && split->count() == 2, "results splitter missing");
    const auto initial = split->sizes();
    const int available = initial[0] + initial[1];
    require(initial[0] >= available * 78 / 100 && initial[0] <= available * 82 / 100,
            "treemap should start with about 80 percent of the results area");
    require(!table->isColumnHidden(1) && table->isColumnHidden(2), "compact table should show allocated size");
    QComboBox *metric = nullptr;
    for (auto *combo : window->findChildren<QComboBox *>())
        if (combo->findText("Logical size") >= 0) metric = combo;
    require(metric, "size metric control missing");
    metric->setCurrentIndex(1);
    require(table->isColumnHidden(1) && !table->isColumnHidden(2), "compact table did not follow the selected metric");
    require(table->columnViewportPosition(2) + table->columnWidth(2) <= table->viewport()->width(),
            "active size column requires horizontal scrolling");
    split->setSizes({available / 3, 2 * available / 3});
    QApplication::processEvents();
    require(!table->isColumnHidden(1) && !table->isColumnHidden(2) && !table->isColumnHidden(3),
            "widening the table should reveal all columns");
    split->setSizes(initial);
    auto *map = dynamic_cast<Treemap *>(split->widget(0));
    require(map, "treemap missing");
    QSet<diver::Id> visible;
    for (int x = 8; x < map->width(); x += 16)
        for (int y = 8; y < map->height(); y += 16) {
            const auto id = map->entryAt({qreal(x), qreal(y)});
            if (id != diver::none) visible.insert(id);
        }
    require(visible.size() == 3, "expected folder header, nested leaf, and top-level file");
    int nestedHits = 0;
    for (auto id : visible) {
        table->selectRow(findChildRow("flat").row());
        map->selected(id);
        if (!table->currentIndex().isValid()) ++nestedHits;
    }
    require(nestedHits == 1 && table->model()->rowCount() == 2,
            "selecting a nested tile should clear the previous table row and keep the current folder");
    metric->setCurrentIndex(0);
    auto *source = window->findChild<QComboBox *>("scanSource");
    auto *browse = qobject_cast<QToolButton *>(button("Choose folder…"));
    auto *options = qobject_cast<QToolButton *>(button("Scan options"));
    require(source && source->isEditable() && source->lineEdit()->isClearButtonEnabled()
            && browse && options && options->menu(), "source or scan options missing");
    const auto settings = options->menu()->actions();
    require(settings.size() == 4 && settings[0]->text() == "Use elevated privileges" && settings[0]->isCheckable()
            && settings[1]->text() == "Use scan cache" && settings[1]->isChecked()
            && settings[3]->text() == "Clear cache", "scan options menu changed");
    window->resize(480, 540);
    QApplication::processEvents();
    require(window->width() == 480 && browse->toolButtonStyle() == Qt::ToolButtonIconOnly
            && !browse->accessibleName().isEmpty() && options->isVisible() && button("Scan")->isVisible()
            && source->width() >= 80, "source and scan actions do not fit the compact window");
    window->resize(3072, 680);
    QApplication::processEvents();
    require(browse->toolButtonStyle() == Qt::ToolButtonTextBesideIcon && source->width() < window->width() / 2
            && browse->x() <= source->geometry().right() + 24, "source controls spread apart on an ultrawide display");
    window->resize(1040, 680);
    auto tap = [](QWidget *target, Qt::Key key, Qt::KeyboardModifiers modifiers = {}) {
        QKeyEvent press(QEvent::KeyPress, key, modifiers);
        QApplication::sendEvent(target, &press);
        QKeyEvent release(QEvent::KeyRelease, key, modifiers);
        QApplication::sendEvent(target, &release);
    };
    window->activateWindow();
    table->setFocus();
    QApplication::processEvents();
    tap(table, Qt::Key_L, Qt::ControlModifier);
    require(source->hasFocus() && source->lineEdit()->selectedText() == source->currentText(),
            "Ctrl+L must focus and select the scan location");
    auto nested = findChildRow("nested");
    require(table->model()->index(nested.row(), 2).data().toString() == "23 B",
            "directory size in UI incorrect");
    table->activated(nested);
    findChildRow("leaf");
    button("Back")->click();
    findChildRow("flat");
    makeFile(dir + "/added", 17);
    button("Scan")->click();
    wait();
    findChildRow("added");
    require(!timing.verifiedPreview, "an outdated preview was promoted");
    source->lineEdit()->clear();
    require(!button("Scan")->isEnabled(), "empty scan source enabled Scan");
    source->setEditText(dir);
    tap(source->lineEdit(), Qt::Key_Return);
    wait();
    findChildRow("added");
    require(timing.verifiedPreview, "unchanged preview was not promoted");
    source->setEditText(dir + "/nested");
    tap(source->lineEdit(), Qt::Key_Return);
    wait();
    findChildRow("leaf");
    require(table->model()->rowCount() == 1, "scan ignored a newly entered folder path");
    require(!window->grab().isNull(), "offscreen rendering failed");
    window->close();
}

int measure(const QString &root, bool useCache) {
    ViewTiming timing;
    auto window = createWindow(root, useCache, &timing);
    window->show();
    while (timing.finalMs < 0 && timing.failure.isEmpty())
        QApplication::processEvents(QEventLoop::AllEvents | QEventLoop::WaitForMoreEvents);
    QJsonObject result{{"first_view_ms", timing.firstMs}, {"verified_view_ms", timing.finalMs},
                       {"first_source", timing.firstSource}, {"entries", qint64(timing.entries)},
                       {"errors", qint64(timing.errors)}, {"unknown_sizes", qint64(timing.unknownSizes)},
                       {"cache", useCache}, {"verified_preview", timing.verifiedPreview}};
    if (!timing.failure.isEmpty()) result.insert("failure", timing.failure);
    std::cout << QJsonDocument(result).toJson(QJsonDocument::Compact).constData() << '\n';
    window->close();
    return timing.failure.isEmpty() ? 0 : 1;
}
}

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    const auto args = app.arguments();
    if (args.size() > 1) {
        if (args[1] == "--timing" && (args.size() == 3 || (args.size() == 4 && args[3] == "--no-cache")))
            return measure(args[2], args.size() == 3);
        std::cerr << "Usage: disk-diver-ui-test --timing ROOT [--no-cache]\n";
        return 2;
    }
    try { testIndex(); testWindow(); }
    catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
