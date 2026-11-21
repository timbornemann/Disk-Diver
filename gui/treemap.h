#pragma once

#include "index.h"
#include <QPointer>
#include <QTimer>
#include <QWidget>
#include <functional>
#include <optional>
#include <unordered_map>

class Treemap final : public QWidget {
public:
    explicit Treemap(QWidget *parent = nullptr);
    static constexpr size_t limit = 256;
    struct Remainder { quint64 bytes = 0; size_t count = 0; };
    // Pass the directory's sorted rows, or its largest rows plus a precomputed tail.
    void setItems(const diver::Index *index, const std::vector<diver::Id> &items, bool allocated,
                  std::optional<Remainder> precomputed = {});
    void setSelection(diver::Id id);
    diver::Id entryAt(QPointF point) const;
    std::function<void(diver::Id)> selected;
    std::function<void(diver::Id)> activated;

protected:
    void paintEvent(QPaintEvent *) override;
    void resizeEvent(QResizeEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseDoubleClickEvent(QMouseEvent *) override;
    bool event(QEvent *) override;

private:
    struct Group {
        std::vector<diver::WeightedEntry> largest;
        Remainder remainder;
    };
    struct Preview {
        Group group;
        diver::Id next = diver::none;
        bool visible = false;
    };
    struct Tile {
        diver::Id id;
        diver::Id parent;
        quint64 bytes;
        size_t remainderCount;
        QRectF rect;
        int color;
        bool expanded = false;
    };
    void layoutTiles();
    void placeGroup(const Group &group, diver::Id parent, QRectF bounds, int level, int color);
    void expand(size_t tile, int level);
    void readChildren(Preview &preview, size_t &budget);
    void continuePreviews();
    quint64 weight(diver::Id id) const;
    QString label(const Tile &tile) const;
    QString relativePath(diver::Id id) const;
    const Tile *tileAt(QPointF point) const;

    QPointer<const diver::Index> index_;
    bool allocated_ = false, finished_ = false;
    diver::Id folder_ = diver::none, selected_ = diver::none;
    Group root_;
    std::unordered_map<diver::Id, Preview> previews_;
    std::vector<diver::Id> pending_;
    QTimer previewTimer_;
    size_t layoutBudget_ = 0;
    std::vector<Tile> tiles_;
};
