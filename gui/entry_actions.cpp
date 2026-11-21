#include "entry_actions.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QScopeGuard>
#include <algorithm>
#include <cstring>

#ifdef Q_OS_WIN
#define NOMINMAX
#include <windows.h>
#include <shlobj.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace diver {

QString EntryTarget::path() const {
    return QDir::toNativeSeparators(QDir(root).filePath(names.join('/')));
}

QString checkNewName(const QString &name) {
    if (name.isEmpty() || name == "." || name == ".." || name.contains('/') || name.contains(QChar::Null))
        return QStringLiteral("Enter a single file or folder name.");
#ifdef Q_OS_WIN
    static const QRegularExpression reserved(QStringLiteral(
        R"(^(?:CON|CONIN\$|CONOUT\$|PRN|AUX|NUL|COM[1-9¹²³]|LPT[1-9¹²³])$)"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression forbidden(QStringLiteral(R"([\\:*?"<>|\x00-\x1f])"));
    if (name.contains(forbidden)
            || name.endsWith('.') || name.endsWith(' ')
            || reserved.match(name.section('.', 0, 0).trimmed()).hasMatch())
        return QStringLiteral("Windows cannot use this name. Choose another name.");
#else
    if (QFile::decodeName(QFile::encodeName(name)) != name)
        return QStringLiteral("This name cannot be represented by the file system.");
#endif
    return {};
}

EntryTarget entryTarget(const Index &index, const QString &root, Id id) {
    EntryTarget target;
    if (id == none || id == 0 || id >= index.size() || index[id].kind == Error) return target;
    target.root = root;
    target.device = index.device();
    target.kind = index[id].kind;
    target.exactName = true;
#ifndef Q_OS_WIN
    const auto raw = index.relativeName(id);
    target.exactName = QFile::encodeName(QFile::decodeName(raw)) == raw;
#endif
    for (; id != none; id = index[id].parent) {
        target.inodes.push_back(index[id].inode);
        if (index[id].parent != none) {
            const auto component = index.name(id);
            if (!checkNewName(component).isEmpty()) return {};
            target.names.prepend(component);
        }
    }
    std::reverse(target.inodes.begin(), target.inodes.end());
    return target;
}

bool EntryTarget::mutableEntry() const {
    return exactName && !names.isEmpty() && (kind == Directory || kind == File)
        && std::find(inodes.begin(), inodes.end(), 0) == inodes.end();
}

namespace {
const auto changed = QStringLiteral("The entry or one of its parent folders has changed. Scan again before continuing.");

#ifdef Q_OS_WIN
QString winError(DWORD code) {
    wchar_t *message = nullptr;
    const auto n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                                  nullptr, code, 0, reinterpret_cast<wchar_t *>(&message), 0, nullptr);
    const auto result = n ? QString::fromWCharArray(message, n).trimmed()
                          : QStringLiteral("Windows error 0x%1").arg(code, 8, 16, QChar('0'));
    LocalFree(message);
    return result;
}

QString nativePath(QString path) {
    path = QDir::toNativeSeparators(path);
    if (path.startsWith("\\\\?\\")) return path;
    return path.startsWith("\\\\") ? "\\\\?\\UNC\\" + path.mid(2) : "\\\\?\\" + path;
}

QString recycle(const QString &path) {
    IShellItem *item = nullptr, *parent = nullptr, *bin = nullptr, *result = nullptr;
    ITransferSource *source = nullptr;
    const auto release = qScopeGuard([&] {
        if (result) result->Release();
        if (source) source->Release();
        if (bin) bin->Release();
        if (parent) parent->Release();
        if (item) item->Release();
    });
    HRESULT hr = SHCreateItemFromParsingName(reinterpret_cast<LPCWSTR>(path.utf16()), nullptr, IID_PPV_ARGS(&item));
    if (SUCCEEDED(hr)) hr = item->GetParent(&parent);
    if (SUCCEEDED(hr)) hr = parent->BindToHandler(nullptr, BHID_Transfer, IID_PPV_ARGS(&source));
    if (SUCCEEDED(hr)) hr = SHGetKnownFolderItem(FOLDERID_RecycleBinFolder, KF_FLAG_DEFAULT, nullptr, IID_PPV_ARGS(&bin));
    // ITransferSource exposes permanent removal as a separate method. Only request recycling.
    if (SUCCEEDED(hr)) hr = source->RecycleItem(item, bin, TSF_NORMAL, &result);
    if (FAILED(hr)) return winError(hr);
    return result ? QString{} : QStringLiteral("Windows could not move this entry to the Recycle Bin.");
}

QString mutate(const EntryTarget &target, const QString *newName) {
    std::vector<HANDLE> handles;
    const auto release = qScopeGuard([&] { for (auto h : handles) CloseHandle(h); });
    QString path = target.root;
    for (qsizetype i = 0; i <= target.names.size(); ++i) {
        if (i) path = QDir(path).filePath(target.names[i - 1]);
        const bool leaf = i == target.names.size();
        const auto full = nativePath(path);
        // Prevent an ancestor from being moved or replaced while the operation is in progress.
        HANDLE h = CreateFileW(reinterpret_cast<LPCWSTR>(full.utf16()), FILE_READ_ATTRIBUTES | (leaf && newName ? DELETE : 0),
                FILE_SHARE_READ | FILE_SHARE_WRITE | (leaf ? FILE_SHARE_DELETE : 0), nullptr, OPEN_EXISTING,
                FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        if (h == INVALID_HANDLE_VALUE) return winError(GetLastError());
        handles.push_back(h);
        BY_HANDLE_FILE_INFORMATION info{};
        if (!GetFileInformationByHandle(h, &info)) return winError(GetLastError());
        const auto inode = (quint64(info.nFileIndexHigh) << 32) | info.nFileIndexLow;
        const bool directory = (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        if (info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DEVICE)
                || info.dwVolumeSerialNumber != target.device || inode != target.inodes[i]
                || directory != (!leaf || target.kind == Directory)) return changed;
    }
    if (!newName) return recycle(QDir::toNativeSeparators(path));
    const auto dest = nativePath(QDir(QFileInfo(path).absolutePath()).filePath(*newName));
    const DWORD bytes = DWORD(dest.size() * sizeof(wchar_t));
    QByteArray storage(sizeof(FILE_RENAME_INFO) + bytes, '\0');
    auto *info = reinterpret_cast<FILE_RENAME_INFO *>(storage.data());
    info->ReplaceIfExists = FALSE;
    info->FileNameLength = bytes;
    memcpy(info->FileName, dest.utf16(), bytes);
    if (!SetFileInformationByHandle(handles.back(), FileRenameInfo, info, DWORD(storage.size())))
        return winError(GetLastError());
    return {};
}
#else
QString posixError() { return QString::fromLocal8Bit(strerror(errno)); }

QString mutate(const EntryTarget &target, const QString *newName) {
    const auto root = QFile::encodeName(target.root);
    int parent = open(root.constData(), O_PATH | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (parent < 0) return posixError();
    const auto closeParent = qScopeGuard([&] { close(parent); });
    struct stat info{};
    for (qsizetype i = 0; i < target.names.size(); ++i) {
        if (i) {
            const auto name = QFile::encodeName(target.names[i - 1]);
            const int next = openat(parent, name.constData(), O_PATH | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            if (next < 0) return posixError();
            close(parent);
            parent = next;
        }
        if (fstat(parent, &info) != 0) return posixError();
        if (info.st_dev != target.device || info.st_ino != target.inodes[i]) return changed;
    }
    const auto name = QFile::encodeName(target.names.last());
    if (fstatat(parent, name.constData(), &info, AT_SYMLINK_NOFOLLOW) != 0) return posixError();
    if (info.st_dev != target.device || info.st_ino != target.inodes.back()
            || (target.kind == Directory ? !S_ISDIR(info.st_mode) : !S_ISREG(info.st_mode))) return changed;
    if (newName) {
        const auto dest = QFile::encodeName(*newName);
        if (syscall(SYS_renameat2, parent, name.constData(), parent, dest.constData(), RENAME_NOREPLACE) != 0)
            return posixError();
        return {};
    }
    const auto path = target.path();
    struct stat visible{};
    if (lstat(QFile::encodeName(path).constData(), &visible) != 0) return posixError();
    if (visible.st_dev != info.st_dev || visible.st_ino != info.st_ino) return changed;
    QFile file(path);
    return file.moveToTrash() ? QString{} : file.errorString();
}
#endif
}

EntryPin::EntryPin(const EntryTarget &target) {
#ifdef Q_OS_WIN
    const auto path = nativePath(target.path());
    HANDLE h = CreateFileW(reinterpret_cast<LPCWSTR>(path.utf16()), FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
            FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE) { error = winError(GetLastError()); return; }
    handle_ = reinterpret_cast<qintptr>(h);
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(h, &info)) { error = winError(GetLastError()); return; }
    const quint64 inode = (quint64(info.nFileIndexHigh) << 32) | info.nFileIndexLow;
    if (info.dwVolumeSerialNumber != target.device || inode != target.inodes.back()) error = changed;
#else
    handle_ = open(QFile::encodeName(target.path()).constData(), O_PATH | O_CLOEXEC | O_NOFOLLOW);
    if (handle_ < 0) { error = posixError(); return; }
    struct stat info{};
    if (fstat(int(handle_), &info) < 0) { error = posixError(); return; }
    if (info.st_dev != target.device || info.st_ino != target.inodes.back()) error = changed;
#endif
}

EntryPin::~EntryPin() {
    if (handle_ == -1) return;
#ifdef Q_OS_WIN
    CloseHandle(reinterpret_cast<HANDLE>(handle_));
#else
    close(int(handle_));
#endif
}

QString renameEntry(const EntryTarget &target, const QString &newName) {
    if (!target.mutableEntry()) return QStringLiteral("This entry cannot be renamed safely.");
    if (auto error = checkNewName(newName); !error.isEmpty()) return error;
    if (newName == target.names.last()) return {};
    return mutate(target, &newName);
}

QString trashEntry(const EntryTarget &target) {
    if (!target.mutableEntry()) return QStringLiteral("This entry cannot be moved to the trash safely.");
    return mutate(target, nullptr);
}

}
