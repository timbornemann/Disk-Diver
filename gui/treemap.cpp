#include "treemap.h"
#include <QHelpEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QToolTip>
#include <algorithm>
#include <array>

namespace {
using diver::Id;
using diver::WeightedEntry;
constexpr size_t previewLimit = 64;
constexpr size_t maxTiles = 1200;
constexpr int maxDepth = 6;
constexpr size_t immediateChildren = 128;
constexpr size_t recordsPerSlice = 2048;
const std::array<QColor, 6> colors = {QColor("#215f70"), QColor("#326557"), QColor("#63517d"),
                                      QColor("#86562e"), QColor("#385d85"), QColor("#834b5b")};

bool heavier(const WeightedEntry &a, const WeightedEntry &b) {
    return a.size != b.size ? a.size > b.size : a.id < b.id;
}

// Split the sorted sibling weights, including the unlabelled tail, along the longer axis.
template<typename Assign>
void split(const std::vector<long double> &prefix, size_t begin, size_t end, QRectF bounds, Assign &assign) {
    if (end == begin + 1) {
        assign(begin, bounds);
        return;
    }
    const auto total = prefix[end] - prefix[begin];
    if (total <= 0) return;
    const auto half = prefix[begin] + total / 2;
    size_t cut = std::lower_bound(prefix.begin() + begin + 1, prefix.begin() + end, half) - prefix.begin();
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
    split(prefix, begin, cut, left, assign);
    split(prefix, cut, end, right, assign);
}
}

Treemap::Treemap(QWidget *parent) : QWidget(parent) {
    setMinimumSize(160, 150);
    setAccessibleName(tr("Disk usage treemap"));
    previewTimer_.setSingleShot(true);
    connect(&previewTimer_, &QTimer::timeout, this, &Treemap::continuePreviews);
}

quint64 Treemap::weight(Id id) const {
    const auto &node = (*index_)[id];
    return allocated_ ? node.allocated : node.logical;
}

void Treemap::setItems(const diver::Index *index, const std::vector<Id> &items, bool allocated,
                       std::optional<Remainder> precomputed) {
    const Id folder = index && !items.empty() ? (*index)[items.front()].parent : diver::none;
    if (index != index_.data() || folder != folder_ || allocated != allocated_ || !finished_)
        previews_.clear();
    index_ = index;
    folder_ = folder;
    allocated_ = allocated;
    finished_ = index && index->finished();
    root_ = {};
    if (index && folder != diver::none) {
        quint64 visibleBytes = 0;
        const size_t n = std::min(limit, items.size());
        for (size_t i = 0; i < n; ++i) {
            const auto size = weight(items[i]);
            if (size) {
                root_.largest.push_back({items[i], size});
                visibleBytes += size;
            }
        }
        if (precomputed) {
            root_.remainder = *precomputed;
        } else if (n < items.size()) {
            // Rows are already sorted. Both the total and the zero-weight suffix
            // are available without walking an arbitrarily wide directory.
            const auto begin = items.begin() + n;
            const auto end = std::partition_point(begin, items.end(), [this](Id id) { return weight(id) > 0; });
            root_.remainder = {weight(folder) - visibleBytes, size_t(end - begin)};
        }
    }
    layoutTiles();
}

void Treemap::readChildren(Preview &preview, size_t &budget) {
    auto &group = preview.group;
    while (budget && preview.next != diver::none) {
        --budget;
        const Id id = preview.next;
        preview.next = (*index_)[id].sibling;
        WeightedEntry item{id, weight(id)};
        if (!item.size) continue;
        if (group.largest.size() < previewLimit) {
            group.largest.push_back(item);
            std::push_heap(group.largest.begin(), group.largest.end(), heavier);
            continue;
        }
        if (heavier(item, group.largest.front())) {
            std::pop_heap(group.largest.begin(), group.largest.end(), heavier);
            std::swap(item, group.largest.back());
            std::push_heap(group.largest.begin(), group.largest.end(), heavier);
        }
        ++group.remainder.count;
        group.remainder.bytes += item.size;
    }
    if (preview.next == diver::none)
        std::sort_heap(group.largest.begin(), group.largest.end(), heavier);
}

void Treemap::continuePreviews() {
    if (!index_) {
        layoutTiles();
        return;
    }
    size_t budget = recordsPerSlice;
    bool completed = false;
    while (budget && !pending_.empty()) {
        auto &preview = previews_.at(pending_.back());
        readChildren(preview, budget);
        if (preview.next == diver::none) {
            pending_.pop_back();
            completed = true;
        }
    }
    if (completed) layoutTiles();
    else if (!pending_.empty()) previewTimer_.start(0);
}

void Treemap::layoutTiles() {
    previewTimer_.stop();
    pending_.clear();
    tiles_.clear();
    layoutBudget_ = recordsPerSlice;
    for (auto &entry : previews_) entry.second.visible = false;
    if (index_) placeGroup(root_, folder_, QRectF(rect()), 0, 0);
    for (auto i = previews_.begin(); i != previews_.end(); ) {
        if (i->second.visible) ++i;
        else i = previews_.erase(i);
    }
    if (!pending_.empty()) previewTimer_.start(0);
    update();
}

void Treemap::placeGroup(const Group &group, Id parent, QRectF bounds, int level, int branchColor) {
    const size_t room = maxTiles - tiles_.size();
    if (!room || bounds.isEmpty()) return;
    // At deeper levels reserve subpixel shares for a single aggregate tile.
    const auto pixelCapacity = size_t(std::max(2.0, bounds.width() * bounds.height() / 300.0));
    const size_t capacity = level == 0 ? room : std::min(room, pixelCapacity);
    const bool aggregate = group.remainder.count || group.largest.size() > capacity;
    const size_t named = std::min(group.largest.size(), capacity - size_t(aggregate));
    auto remainder = group.remainder;
    for (size_t i = named; i < group.largest.size(); ++i) {
        ++remainder.count;
        remainder.bytes += group.largest[i].size;
    }

    const size_t start = tiles_.size();
    std::vector<long double> prefix{0};
    for (size_t i = 0; i < named; ++i) {
        const auto item = group.largest[i];
        const int color = level == 0 ? int(i % colors.size()) : branchColor;
        tiles_.push_back({item.id, parent, item.size, 0, {}, color});
        prefix.push_back(prefix.back() + item.size);
    }
    if (remainder.count && remainder.bytes) {
        tiles_.push_back({diver::none, parent, remainder.bytes, remainder.count, {}, 0});
        prefix.push_back(prefix.back() + remainder.bytes);
    }
    const size_t count = tiles_.size() - start;
    if (!count) return;
    auto assign = [&](size_t i, QRectF bounds) { tiles_[start + i].rect = bounds.adjusted(1, 1, -1, -1); };
    split(prefix, 0, count, bounds, assign);
    if (level < maxDepth)
        for (size_t i = 0; i < named; ++i) expand(start + i, level);
}

void Treemap::expand(size_t tile, int level) {
    const Id id = tiles_[tile].id;
    const auto &node = (*index_)[id];
    if (node.kind != diver::Directory || !node.childCount || tiles_.size() + 2 > maxTiles) return;
    const auto height = fontMetrics().height();
    const auto body = tiles_[tile].rect.adjusted(3, height + 7, -3, -3);
    if (body.width() < 56 || body.height() < height + 4) return;

    auto [it, inserted] = previews_.try_emplace(id);
    auto &preview = it->second;
    if (inserted) preview.next = node.firstChild;
    preview.visible = true;
    if (preview.next != diver::none) {
        if (node.childCount <= immediateChildren && node.childCount <= layoutBudget_) {
            readChildren(preview, layoutBudget_);
        } else {
            // Deferred readers see only finished indexes. A live scan can change
            // an existing sibling's weight when it discovers a hard link.
            if (finished_) pending_.push_back(id);
            return;
        }
    }
    if (preview.group.largest.empty()) return;
    tiles_[tile].expanded = true;
    placeGroup(preview.group, id, body, level + 1, tiles_[tile].color);
}

void Treemap::setSelection(Id id) {
    selected_ = id;
    update();
}

void Treemap::resizeEvent(QResizeEvent *) { layoutTiles(); }

QString Treemap::label(const Tile &tile) const {
    if (tile.id != diver::none) return index_->name(tile.id);
    return tile.remainderCount == 1 ? tr("1 more entry") : tr("%L1 more entries").arg(tile.remainderCount);
}

QString Treemap::relativePath(Id id) const {
    QStringList components;
    while (id != diver::none && (*index_)[id].parent != diver::none) {
        components.push_back(index_->name(id));
        id = (*index_)[id].parent;
    }
    std::reverse(components.begin(), components.end());
    return components.join('/');
}

void Treemap::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.fillRect(rect(), palette().base());
    if (!index_ || tiles_.empty()) {
        p.setPen(palette().color(QPalette::Text));
        p.drawText(rect(), Qt::AlignCenter, tr("No measurable disk usage"));
        return;
    }
    const QFontMetrics metrics(font());
    QRectF selection;
    for (const auto &tile : tiles_) {
        const auto &area = tile.rect;
        if (area.isEmpty()) continue;
        const bool folder = tile.id != diver::none && (*index_)[tile.id].kind == diver::Directory;
        const auto color = tile.id == diver::none ? QColor("#58606a") : colors[tile.color];
        p.fillRect(area, folder ? color.darker(145) : color);
        const bool hasHeader = tile.id != diver::none && area.width() >= 30 && area.height() >= metrics.height() + 6;
        if (hasHeader)
            p.fillRect(QRectF(area.topLeft(), QSizeF(area.width(), metrics.height() + 6)),
                       color.darker(folder ? 175 : 125));
        if (folder && area.width() > 4 && area.height() > 4) {
            p.setPen(color.lighter(145));
            p.setBrush(Qt::NoBrush);
            p.drawRect(area.adjusted(0.5, 0.5, -0.5, -0.5));
        }
        if (tile.id != diver::none && tile.id == selected_) selection = area;
        auto textArea = area.adjusted(5, 3, -5, -3);
        if (textArea.width() < 20 || textArea.height() < metrics.height()) continue;
        const auto size = diver::formatSize(tile.bytes);
        const int sizeWidth = metrics.horizontalAdvance(size);
        p.setPen(Qt::white);
        if (folder) {
            // A header and its surrounding border belong to the folder. Files
            // occupy the inset body, leaving the full title bar selectable.
            const auto icon = textArea.topLeft() + QPointF(0, (metrics.height() - 10) / 2.0);
            p.drawLine(icon + QPointF(0, 2), icon + QPointF(4, 2));
            p.drawRect(QRectF(icon + QPointF(0, 4), QSizeF(10, 6)));
            textArea.setLeft(textArea.left() + 15);
        }
        const bool inlineSize = hasHeader && textArea.width() >= sizeWidth + 36;
        if (inlineSize) {
            p.drawText(textArea, Qt::AlignTop | Qt::AlignRight, size);
            textArea.setRight(textArea.right() - sizeWidth - 8);
        }
        p.drawText(textArea, Qt::AlignTop | Qt::AlignLeft,
                   metrics.elidedText(label(tile), Qt::ElideRight, int(textArea.width())));
        if (!inlineSize && !tile.expanded && textArea.height() >= 2 * metrics.height() + 2 && sizeWidth <= textArea.width()) {
            textArea.setTop(textArea.top() + metrics.height() + 2);
            p.drawText(textArea, Qt::AlignTop | Qt::AlignLeft, size);
        }
    }
    if (!selection.isEmpty()) {
        p.setPen(QPen(Qt::white, 3));
        p.setBrush(Qt::NoBrush);
        p.drawRect(selection.adjusted(1.5, 1.5, -1.5, -1.5));
    }
}

const Treemap::Tile *Treemap::tileAt(QPointF point) const {
    if (index_)
        for (auto it = tiles_.rbegin(); it != tiles_.rend(); ++it)
            if (!it->rect.isEmpty() && it->rect.contains(point)) return &*it;
    return nullptr;
}

Id Treemap::entryAt(QPointF point) const {
    const auto *tile = tileAt(point);
    return tile ? tile->id : diver::none;
}

void Treemap::mousePressEvent(QMouseEvent *event) {
    const Id id = entryAt(event->position());
    if (event->button() == Qt::LeftButton && id != diver::none && selected) selected(id);
}

void Treemap::mouseDoubleClickEvent(QMouseEvent *event) {
    const Id id = entryAt(event->position());
    if (event->button() == Qt::LeftButton && id != diver::none && activated) activated(id);
}

bool Treemap::event(QEvent *event) {
    if (event->type() == QEvent::ToolTip) {
        const auto *help = static_cast<QHelpEvent *>(event);
        if (const auto *tile = tileAt(help->pos())) {
            QString path = relativePath(tile->id != diver::none ? tile->id : tile->parent);
            if (tile->id == diver::none) {
                if (!path.isEmpty()) path += '/';
                path += label(*tile);
            }
            QToolTip::showText(help->globalPos(), QStringLiteral("<p>%1<br>%2</p>")
                               .arg(path.toHtmlEscaped(), diver::formatSize(tile->bytes)), this);
        } else {
            QToolTip::hideText();
            event->ignore();
        }
        return true;
    }
    return QWidget::event(event);
}
