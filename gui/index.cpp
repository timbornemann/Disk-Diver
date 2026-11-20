#include "index.h"

#include <QFile>
#include <QLocale>
#include <QtEndian>
#include <algorithm>

namespace diver {

namespace {
void sum(Node &a, const Node &b) {
    a.logical += b.logical;
    a.allocated += b.allocated;
    a.errors += b.errors;
    a.missingSizes += b.missingSizes;
}

int nativeCompare(const QByteArray &a, const QByteArray &b) {
#ifdef Q_OS_WIN
    const int len = static_cast<int>(std::min(a.size(), b.size()));
    for (int i = 0; i < len; i += 2) {
        const int x = qFromLittleEndian<quint16>(a.constData() + i);
        const int y = qFromLittleEndian<quint16>(b.constData() + i);
        if (x != y) return x - y;
    }
    return (a.size() > b.size()) - (a.size() < b.size());
#else
    return a.compare(b);
#endif
}
}

void Index::addToAncestors(Id id, const Node &from, bool subtract) {
    while (id != none) {
        Node &node = nodes_[id];
        if (subtract) {
            node.logical -= from.logical;
            node.allocated -= from.allocated;
            node.errors -= from.errors;
            node.missingSizes -= from.missingSizes;
        } else {
            sum(node, from);
        }
        id = node.parent;
    }
}

void Index::flush() {
    addToAncestors(pendingParent_, pending_, false);
    pendingParent_ = none;
    pending_ = {};
}

void Index::finish() {
    flush();
    decltype(owners_){}.swap(owners_);
}

bool Index::append(Kind kind, quint64 parent, quint64 logical, quint64 allocated,
                   Identity identity, const char *name, quint32 length) {
    if (nodes_.size() >= none || quint64(names_.size()) + length > none) return false;
    const bool root = nodes_.empty();
    if (root ? (parent != unknown || kind != Directory)
             : (parent >= nodes_.size() || nodes_[parent].kind != Directory)) return false;
#ifdef Q_OS_WIN
    if (length % 2 != 0) return false;
#endif
    const Id id = static_cast<Id>(nodes_.size());
    const Id parentId = root ? none : static_cast<Id>(parent);
    if (pendingParent_ != parentId) flush();
    Node node;
    node.kind = kind;
    node.parent = parentId;
    node.nameOffset = static_cast<quint32>(names_.size());
    node.nameLength = length;
    names_.append(name, length);
    node.logical = logical;
    node.allocated = allocated == unknown ? 0 : allocated;
    node.missingSizes = allocated == unknown ? 1 : 0;
    node.errors = kind == Error ? 1 : 0;
    if (!root) {
        node.sibling = nodes_[parentId].firstChild;
        nodes_[parentId].firstChild = id;
        ++nodes_[parentId].childCount;
    }
    nodes_.push_back(node);
    if (kind == File && identity.inode != 0) {
        auto [owner, inserted] = owners_.emplace(identity, id);
        if (!inserted) {
            const Id first = owner->second;
            if (nativeCompare(relativeName(id), relativeName(first)) < 0) {
                flush();
                Node removed;
                removed.logical = nodes_[first].logical;
                removed.allocated = nodes_[first].allocated;
                removed.missingSizes = nodes_[first].missingSizes;
                aliases_.emplace(first, AliasSizes{removed.logical, removed.missingSizes ? unknown : removed.allocated});
                addToAncestors(nodes_[first].parent, removed, true);
                nodes_[first].alias = true;
                nodes_[first].logical = 0;
                nodes_[first].allocated = 0;
                nodes_[first].missingSizes = 0;
                owner->second = id;
            } else {
                aliases_.emplace(id, AliasSizes{logical, allocated});
                nodes_[id].logical = 0;
                nodes_[id].allocated = 0;
                nodes_[id].missingSizes = 0;
                nodes_[id].alias = true;
            }
        }
    }
    pendingParent_ = parentId;
    sum(pending_, nodes_[id]);
    return true;
}

QByteArray Index::rawName(Id id) const {
    const Node &node = nodes_[id];
    return QByteArray::fromRawData(names_.constData() + node.nameOffset, node.nameLength);
}

QByteArray Index::relativeName(Id id) const {
    std::vector<Id> components;
    while (nodes_[id].parent != none) {
        components.push_back(id);
        id = nodes_[id].parent;
    }
    QByteArray result;
    for (auto it = components.rbegin(); it != components.rend(); ++it) {
        if (!result.isEmpty()) {
            result.append('/');
#ifdef Q_OS_WIN
            result.append(char(0));
#endif
        }
        result.append(rawName(*it));
    }
    return result;
}

QString Index::name(Id id) const {
    const auto raw = rawName(id);
#ifdef Q_OS_WIN
    QString result(raw.size() / 2, Qt::Uninitialized);
    qFromLittleEndian<quint16>(raw.constData(), raw.size() / 2, result.data());
    return result;
#else
    return QFile::decodeName(raw);
#endif
}

Id Index::findRelative(const QByteArray &path) const {
    if (nodes_.empty()) return none;
    if (path.isEmpty()) return 0;
    Id current = 0;
    qsizetype start = 0;
#ifdef Q_OS_WIN
    constexpr qsizetype step = 2;
#else
    constexpr qsizetype step = 1;
#endif
    for (qsizetype i = 0; i <= path.size(); i += step) {
        if (i != path.size() && (path[i] != '/' || (step == 2 && path[i + 1] != 0))) continue;
        auto child = nodes_[current].firstChild;
        const auto component = QByteArray::fromRawData(path.constData() + start, i - start);
        while (child != none && rawName(child) != component) child = nodes_[child].sibling;
        if (child == none) return none;
        current = child;
        start = i + step;
    }
    return current;
}

std::vector<Id> Index::children(Id parent, bool allocated) const {
    std::vector<Id> result;
    if (parent == none || parent >= nodes_.size()) return result;
    result.reserve(nodes_[parent].childCount);
    for (Id i = nodes_[parent].firstChild; i != none; i = nodes_[i].sibling)
        result.push_back(i);
    std::sort(result.begin(), result.end(), [&](Id a, Id b) {
        const auto av = allocated ? nodes_[a].allocated : nodes_[a].logical;
        const auto bv = allocated ? nodes_[b].allocated : nodes_[b].logical;
        if (av != bv) return av > bv;
        return nativeCompare(rawName(a), rawName(b)) < 0;
    });
    return result;
}

std::vector<WeightedEntry> Index::weights(Id parent, bool allocated) const {
    std::vector<WeightedEntry> result;
    result.reserve(nodes_[parent].childCount);
    for (Id i = nodes_[parent].firstChild; i != none; i = nodes_[i].sibling)
        result.push_back({i, allocated ? nodes_[i].allocated : nodes_[i].logical});
    return result;
}

QString formatSize(quint64 bytes) {
    static const char *const units[] = {"B", "KiB", "MiB", "GiB", "TiB", "PiB", "EiB"};
    double value = static_cast<double>(bytes);
    int unit = 0;
    while (value >= 1024.0 && unit < 6) {
        value /= 1024.0;
        ++unit;
    }
    return QLocale().toString(value, 'f', unit == 0 ? 0 : 1)
            + QChar(' ') + QString::fromLatin1(units[unit]);
}

}
