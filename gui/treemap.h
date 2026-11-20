#pragma once

#include "index.h"
#include <QWidget>
#include <functional>
#include <optional>

class Treemap final : public QWidget {
public:
    explicit Treemap(QWidget *parent = nullptr);
    static constexpr size_t limit = 256;
    struct Remainder { quint64 bytes = 0; size_t count = 0; };
    void setItems(const diver::Index *index, const std::vector<diver::Id> &items, bool allocated,
                  std::optional<Remainder> precomputed = {});
    void setSelection(diver::Id id);
    std::function<void(diver::Id)> selected;
    std::function<void(diver::Id)> activated;

protected:
    void paintEvent(QPaintEvent *) override;
    void resizeEvent(QResizeEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseDoubleClickEvent(QMouseEvent *) override;
    bool event(QEvent *) override;

private:
    struct Tile {
        diver::Id id;
        QString label;
        quint64 bytes;
        QRectF rect;
    };
    void layoutTiles();
    int tileAt(QPointF point) const;
    std::vector<Tile> tiles_;
    diver::Id selected_ = diver::none;
};
