#pragma once

#include "index.h"
#include <QStringList>

namespace diver {

// A snapshot of the path and IDs recorded by the scanner, independent of row order.
struct EntryTarget {
    QString root;
    QStringList names;
    std::vector<quint64> inodes; // Starts with the selected scan root.
    quint64 device = 0;
    Kind kind = Error;
    bool exactName = false;
    QString path() const;
    bool mutableEntry() const;
};

EntryTarget entryTarget(const Index &index, const QString &root, Id id);
// Holding a reference keeps a deleted object's identity from being reused while a menu or dialog is open.
class EntryPin {
public:
    explicit EntryPin(const EntryTarget &target);
    ~EntryPin();
    EntryPin(const EntryPin &) = delete;
    EntryPin &operator=(const EntryPin &) = delete;
    QString error;
private:
    qintptr handle_ = -1;
};
QString checkNewName(const QString &name);
QString renameEntry(const EntryTarget &target, const QString &newName);
QString trashEntry(const EntryTarget &target);

}
