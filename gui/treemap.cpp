#include "treemap.h"
#include <QHelpEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QToolTip>
#include <algorithm>

Treemap::Treemap(QWidget *parent) : QWidget(parent) {
    setMinimumSize(160, 150);
    setAccessibleName(tr("Speicherbelegung als Flächenkarte"));
}

void Treemap::setItems(const diver::Index *index, const std::vector<diver::Id> &items, bool allocated,
                       std::optional<Remainder> precomputed) {
    tiles_.clear();
    auto tail = precomputed.value_or(Remainder{});
    const size_t end = precomputed ? std::min(limit, items.size()) : items.size();
    for (size_t i = 0; i < end; ++i) {
        const auto id = items[i];
        const auto &node = (*index)[id];
        const auto size = allocated ? node.allocated : node.logical;
        if (!size) continue;
        if (i < limit)
            tiles_.push_back({id, index->name(id), size, {}});
        else {
            tail.bytes += size;
            ++tail.count;
        }
    }
    if (tail.count)
        tiles_.push_back({diver::none, tr("%1 weitere Einträge").arg(tail.count), tail.bytes, {}});
    layoutTiles();
    update();
}

void Treemap::layoutTiles() {
    if (tiles_.empty()) return;
    std::vector<long double> prefix{0};
    for (const auto &tile : tiles_) prefix.push_back(prefix.back() + tile.bytes);
    auto split = [&](auto &&self, size_t begin, size_t end, QRectF bounds) -> void {
        if (end == begin + 1) {
            tiles_[begin].rect = bounds;
            return;
        }
        const auto total = prefix[end] - prefix[begin];
        if (total <= 0) {
            for (size_t i = begin; i < end; ++i) tiles_[i].rect = {};
            return;
        }
        const auto half = prefix[begin] + total / 2;
        auto cut = size_t(std::lower_bound(prefix.begin() + begin + 1,
                                           prefix.begin() + end, half) - prefix.begin());
        cut = std::min(cut, end - 1);
        if (cut > begin + 1 && half - prefix[cut - 1] < prefix[cut] - half) --cut;
        const double fraction = double((prefix[cut] - prefix[begin]) / total);
        QRectF left = bounds, right = bounds;
        if (bounds.width() >= bounds.height()) {
            left.setWidth(bounds.width() * fraction);
            right.setLeft(left.right());
        } else {
            left.setHeight(bounds.height() * fraction);
            right.setTop(left.bottom());
        }
        self(self, begin, cut, left);
        self(self, cut, end, right);
    };
    split(split, 0, tiles_.size(), QRectF(rect()));
}

void Treemap::setSelection(diver::Id id) {
    selected_ = id;
    update();
}

void Treemap::resizeEvent(QResizeEvent *) { layoutTiles(); }

void Treemap::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.fillRect(rect(), palette().base());
    if (tiles_.empty()) {
        p.setPen(palette().color(QPalette::Text));
        p.drawText(rect(), Qt::AlignCenter, tr("Keine messbare Belegung"));
        return;
    }
    const QColor colors[] = {QColor("#215f70"), QColor("#326557"), QColor("#63517d"),
                             QColor("#86562e"), QColor("#385d85"), QColor("#834b5b")};
    const QFontMetrics metrics(font());
    QRectF selection;
    for (size_t i = 0; i < tiles_.size(); ++i) {
        const auto &tile = tiles_[i];
        QRectF area = tile.rect.adjusted(1, 1, -1, -1);
        if (area.isEmpty()) continue;
        p.fillRect(area, tile.id == diver::none ? QColor("#58606a")
                                                : colors[i % 6]);
        if (tile.id != diver::none && tile.id == selected_) selection = area;
        QRectF label = area.adjusted(6, 3, -6, -3);
        if (label.width() < 24 || label.height() < metrics.height()) continue;
        p.setPen(Qt::white);
        p.drawText(label, Qt::AlignTop | Qt::AlignLeft,
                   metrics.elidedText(tile.label, Qt::ElideRight, int(label.width())));
        if (label.height() >= 2 * metrics.height() + 2) {
            label.setTop(label.top() + metrics.height() + 2);
            const QString size = diver::formatSize(tile.bytes);
            if (metrics.horizontalAdvance(size) <= label.width())
                p.drawText(label, Qt::AlignTop | Qt::AlignLeft, size);
        }
    }
    if (!selection.isEmpty()) {
        p.setPen(QPen(Qt::white, 3));
        p.setBrush(Qt::NoBrush);
        p.drawRect(selection.adjusted(1.5, 1.5, -1.5, -1.5));
    }
}

int Treemap::tileAt(QPointF point) const {
    for (size_t i = 0; i < tiles_.size(); ++i)
        if (tiles_[i].rect.contains(point)) return int(i);
    return -1;
}

void Treemap::mousePressEvent(QMouseEvent *event) {
    const int i = tileAt(event->position());
    if (event->button() == Qt::LeftButton && i >= 0 && tiles_[i].id != diver::none && selected)
        selected(tiles_[i].id);
}

void Treemap::mouseDoubleClickEvent(QMouseEvent *event) {
    const int i = tileAt(event->position());
    if (event->button() == Qt::LeftButton && i >= 0 && tiles_[i].id != diver::none && activated)
        activated(tiles_[i].id);
}

bool Treemap::event(QEvent *event) {
    if (event->type() == QEvent::ToolTip) {
        auto *help = static_cast<QHelpEvent *>(event);
        const int i = tileAt(help->pos());
        if (i >= 0) {
            QToolTip::showText(help->globalPos(), QStringLiteral("<p>%1<br>%2</p>")
                               .arg(tiles_[i].label.toHtmlEscaped(), diver::formatSize(tiles_[i].bytes)), this);
            return true;
        }
    }
    return QWidget::event(event);
}
