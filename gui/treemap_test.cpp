#include "treemap.h"
#include <QApplication>
#include <QElapsedTimer>
#include <QHelpEvent>
#include <QMouseEvent>
#include <QToolTip>
#include <iostream>
#include <memory>
#include <stdexcept>

namespace {
using diver::Id;

void require(bool ok, const char *message) {
    if (!ok) throw std::runtime_error(message);
}

Id add(diver::Index &index, diver::Kind kind, quint64 parent, const QString &name,
       quint64 logical = 0, quint64 allocated = 0) {
#ifdef Q_OS_WIN
    const QByteArray bytes(reinterpret_cast<const char *>(name.utf16()), name.size() * 2);
#else
    const auto bytes = name.toUtf8();
#endif
    const auto id = Id(index.size());
    require(index.append(kind, parent, logical, allocated, {}, bytes.constData(), bytes.size()), "bad fixture");
    return id;
}

QPoint locate(const Treemap &map, Id id) {
    for (int y = 6; y < map.height() - 2; y += 7)
        for (int x = 6; x < map.width() - 2; x += 7)
            if (map.entryAt({double(x), double(y)}) == id) return {x, y};
    return {-1, -1};
}

void click(Treemap &map, QPoint pos, QEvent::Type type) {
    QMouseEvent event(type, pos, map.mapToGlobal(pos), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&map, &event);
}

QString tooltip(Treemap &map, QPoint pos) {
    QHelpEvent help(QEvent::ToolTip, pos, map.mapToGlobal(pos));
    QApplication::sendEvent(&map, &help);
    return QToolTip::text();
}

void nestAndSelect(Treemap &map) {
    diver::Index index;
    add(index, diver::Directory, diver::unknown, {});
    auto projects = add(index, diver::Directory, 0, "Projects");
    auto archive = add(index, diver::Directory, projects, "archive");
    auto file = add(index, diver::File, archive, "a<&>.zip", 900, 1);
    auto sibling = add(index, diver::File, 0, "other", 100, 999);
    index.finish();
    map.setItems(&index, index.children(0, false), false);
    const auto header = locate(map, projects), subheader = locate(map, archive), leaf = locate(map, file);
    require(header.x() >= 0 && subheader.x() >= 0 && leaf.x() >= 0 && locate(map, sibling).x() >= 0,
            "nested headers or leaf unavailable");
    require(map.entryAt({-1, -1}) == diver::none, "background reported an entry");
    require(tooltip(map, leaf).contains("Projects/archive/a&lt;&amp;&gt;.zip"), "tooltip lost or failed to escape path");
    Id selected = diver::none, activated = diver::none;
    map.selected = [&](Id id) { selected = id; };
    map.activated = [&](Id id) { activated = id; };
    click(map, leaf, QEvent::MouseButtonPress);
    click(map, subheader, QEvent::MouseButtonDblClick);
    require(selected == file && activated == archive, "callbacks received the wrong nested IDs");
    click(map, header, QEvent::MouseButtonPress);
    require(selected == projects, "folder header was not selectable");
    map.selected = {};
    map.activated = {};

    map.setItems(&index, index.children(0, true), true);
    require(map.entryAt(QPointF(map.width() / 2, map.height() / 2)) == sibling,
            "area did not follow the new metric");
    map.setItems(&index, {archive}, false);
    require(tooltip(map, locate(map, file)).contains("Projects/archive/"), "navigation lost the scan-relative path");
    map.setItems(&index, {file}, false);
    const auto image = map.grab().toImage();
    require(image.pixelColor(3, 4) != image.pixelColor(3, 100)
            && map.entryAt({3, 4}) == file && map.entryAt({3, 100}) == file, "file title strip missing");

    // Deep single-child chains must leave a real, selectable folder at the depth cap.
    diver::Index deep;
    add(deep, diver::Directory, diver::unknown, {});
    for (Id i = 0; i < 60; ++i) add(deep, diver::Directory, i, "dir");
    const auto deepest = add(deep, diver::File, 60, "end", 50, 50);
    deep.finish();
    map.setItems(&deep, {1}, false);
    require(locate(map, deepest).x() < 0 && locate(map, 2).x() >= 0, "recursive depth was unbounded");
}

void wideAndTransient(Treemap &map) {
    auto index = std::make_unique<diver::Index>();
    add(*index, diver::Directory, diver::unknown, {});
    auto folder = add(*index, diver::Directory, 0, "wide");
    // The largest file is the last sibling visited by the index's linked list.
    constexpr int smallFiles = 100000;
    auto largest = add(*index, diver::File, folder, "largest", 300000, 300000);
    for (int i = 0; i < smallFiles; ++i) add(*index, diver::File, folder, "small", 1, 1);
    add(*index, diver::File, folder, "empty", 0, 0);
    index->flush();
    const QPoint probe(map.width() / 3, map.height() / 2);
    map.setItems(index.get(), {folder}, false);
    require(map.entryAt(probe) == folder, "wide live directory was synchronously traversed");
    index->finish();
    map.setItems(index.get(), {folder}, false);
    require(map.entryAt(probe) == folder, "wide finished directory was synchronously traversed");

    QElapsedTimer wait;
    wait.start();
    while (map.entryAt(probe) != largest && wait.elapsed() < 4000)
        QApplication::processEvents(QEventLoop::AllEvents, 5);
    require(map.entryAt(probe) == largest, "deferred preview lost the late heavy sibling");
    const QPoint tail(map.width() * 9 / 10, map.height() / 2);
    require(map.entryAt(tail) == diver::none, "synthetic remainder reported a file ID");
    const QString count = QLocale().toString(smallFiles + 1 - 64);
    require(tooltip(map, tail).contains(count) && tooltip(map, tail).contains("97.6 KiB"),
            "remainder count or size was wrong");
    int calls = 0;
    map.selected = [&](Id) { ++calls; };
    click(map, tail, QEvent::MouseButtonPress);
    map.selected = {};
    require(!calls, "aggregate sent a selection");
    map.resize(280, 200);
    QApplication::processEvents();
    require(locate(map, largest).x() >= 0, "resize lost the largest child");

    auto rows = index->children(folder, false);
    map.setItems(index.get(), rows, false);
    require(locate(map, largest).x() >= 0, "wide root lost its largest file");
    require(tooltip(map, QPoint(map.width() * 9 / 10, map.height() / 2)).contains(QLocale().toString(smallFiles + 1 - Treemap::limit)),
            "top-level remainder did not omit zero-sized siblings");
    map.setItems(index.get(), {folder}, true);  // queues a new metric
    index.reset();
    map.resize(520, 320);
    QApplication::processEvents();
    require(map.entryAt({50, 50}) == diver::none && !map.grab().isNull(), "dangling preview after index replacement");
}
}

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    QLocale::setDefault(QLocale::English);
    Treemap map;
    map.resize(720, 460);
    map.show();
    QApplication::processEvents();
    try {
        nestAndSelect(map);
        wideAndTransient(map);
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
