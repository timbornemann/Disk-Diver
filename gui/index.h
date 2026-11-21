#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QtGlobal>
#include <cstdint>
#include <limits>
#include <unordered_map>
#include <vector>

namespace diver {

using Id = quint32;
constexpr Id none = std::numeric_limits<Id>::max();
constexpr quint64 unknown = std::numeric_limits<quint64>::max();
enum Kind : quint8 { Directory = 1, File = 2, Other = 3, Error = 4 };

struct Node {
    Id parent = none;
    Id firstChild = none;
    Id sibling = none;
    quint32 nameOffset = 0;
    quint32 nameLength = 0;
    quint32 childCount = 0;
    quint64 logical = 0;
    quint64 allocated = 0;
    quint64 inode = 0;
    quint32 errors = 0;
    quint32 missingSizes = 0;
    Kind kind = Other;
    bool alias = false;
};

struct Identity {
    quint64 device;
    quint64 inode;
    bool operator==(const Identity &other) const {
        return device == other.device && inode == other.inode;
    }
};

struct IdentityHash {
    size_t operator()(const Identity &key) const {
        const size_t a = std::hash<quint64>{}(key.device);
        const size_t b = std::hash<quint64>{}(key.inode);
        return a ^ (b + 0x9e3779b9u + (a << 6) + (a >> 2));
    }
};

struct AliasSizes {
    quint64 logical, allocated;
};

struct WeightedEntry {
    Id id;
    quint64 size;
};

class Index : public QObject {
public:
    bool append(Kind kind, quint64 parent, quint64 logical, quint64 allocated,
                Identity identity, const char *name, quint32 length);
    void flush();
    void finish();
    bool finished() const { return finished_; }
    QString name(Id id) const;
    QByteArray relativeName(Id id) const;
    Id findRelative(const QByteArray &path) const;
    std::vector<Id> children(Id parent, bool allocated) const;
    std::vector<WeightedEntry> weights(Id parent, bool allocated) const;
    const AliasSizes &aliasSizes(Id id) const { return aliases_.at(id); }
    const Node &operator[](Id id) const { return nodes_[id]; }
    size_t size() const { return nodes_.size(); }
    quint64 device() const { return device_; }

private:
    void addToAncestors(Id id, const Node &from, bool subtract);
    QByteArray rawName(Id id) const;
    std::vector<Node> nodes_;
    quint64 device_ = 0;
    QByteArray names_;
    std::unordered_map<Identity, Id, IdentityHash> owners_;
    std::unordered_map<Id, AliasSizes> aliases_;
    // Directory enumeration tends to deliver siblings in runs. Propagate one
    // combined update per run so file counts do not multiply by path depth.
    Id pendingParent_ = none;
    Node pending_;
    bool finished_ = false;
};

QString formatSize(quint64 bytes);

}
