#include "scanner.hpp"
#include "excluded_dirs.hpp"
#include <windows.h>
#include <array>
#include <cstddef>
#include <memory>
#include <stdexcept>
#include <system_error>

namespace dd {
namespace {
struct CloseHandleDeleter {
    void operator()(void* h) const noexcept { CloseHandle(h); }
};
struct CloseFind {
    void operator()(void* h) const noexcept { FindClose(h); }
};
using Handle = std::unique_ptr<void, CloseHandleDeleter>;

Handle open(const std::wstring& path, DWORD access) {
    auto handle = CreateFileW(path.c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    return Handle(handle == INVALID_HANDLE_VALUE ? nullptr : handle);
}

std::wstring absolute_path(const std::filesystem::path& path) {
    if (path.empty()) throw std::invalid_argument("empty scan root");
    DWORD length = GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
    if (!length) throw std::system_error(GetLastError(), std::system_category(), "cannot resolve scan root");
    std::wstring name(length, L'\0');
    DWORD used = GetFullPathNameW(path.c_str(), length, name.data(), nullptr);
    if (!used || used >= length) throw std::runtime_error("scan root changed during path resolution");
    name.resize(used);
    if (name.starts_with(L"\\\\.\\")) throw std::invalid_argument("choose a filesystem path");
    if (!name.starts_with(L"\\\\?\\")) {
        if (name.starts_with(L"\\\\")) name = L"\\\\?\\UNC\\" + name.substr(2);
        else name = L"\\\\?\\" + name;
    }
    // Preserve the trailing slash required when opening a drive or a volume GUID root.
    while (name.size() > 1 && name.back() == L'\\' && name[name.size() - 2] != L':' &&
           !(name.starts_with(L"\\\\?\\Volume{") && name.find(L"}\\") == name.size() - 2))
        name.pop_back();
    return name;
}
std::wstring child_path(const std::wstring& parent, std::wstring_view name) {
    std::wstring path(parent);
    if (path.back() != L'\\') path.push_back(L'\\');
    path.append(name);
    return path;
}
std::string_view bytes(std::wstring_view name) {
    static_assert(sizeof(wchar_t) == 2);
    return {reinterpret_cast<const char*>(name.data()), name.size() * sizeof(wchar_t)};
}
u64 identity(const BY_HANDLE_FILE_INFORMATION& info) {
    return (static_cast<u64>(info.nFileIndexHigh) << 32) | info.nFileIndexLow;
}
Kind type(DWORD attrs) {
    if (attrs & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DEVICE)) return Kind::other;
    return (attrs & FILE_ATTRIBUTE_DIRECTORY) ? Kind::directory : Kind::file;
}

struct Directory {
    std::wstring path;
    u64 node;
    u64 inode;
};

class Walker {
public:
    Walker(const Sink& sink, bool recurse, DWORD volume) : emit(sink), recursive(recurse), device(volume) {}

    void enumerate(const Directory& parent, HANDLE directory) {
        bool first = true;
        for (;;) {
            if (!GetFileInformationByHandleEx(directory, FileIdBothDirectoryInfo, buffer.data(), sizeof(buffer))) {
                auto status = GetLastError();
                if (status == ERROR_NO_MORE_FILES || status == ERROR_FILE_NOT_FOUND) return;
                if (first && (status == ERROR_INVALID_FUNCTION || status == ERROR_INVALID_PARAMETER ||
                              status == ERROR_NOT_SUPPORTED || status == ERROR_INVALID_LEVEL)) {
                    compatibility_walk(parent);
                } else {
                    emit({Kind::error, parent.node, 0, 0, 0, 0, {}});
                }
                return;
            }
            first = false;
            std::size_t offset = 0;
            for (;;) {
                constexpr auto prefix = offsetof(FILE_ID_BOTH_DIR_INFO, FileName);
                if (sizeof(buffer) - offset < prefix) throw std::runtime_error("invalid directory buffer");
                const auto* e = reinterpret_cast<const FILE_ID_BOTH_DIR_INFO*>(buffer.data() + offset);
                const auto available = e->NextEntryOffset ? e->NextEntryOffset : sizeof(buffer) - offset;
                if (available < prefix || available > sizeof(buffer) - offset ||
                    (e->FileNameLength % 2) || e->FileNameLength > available - prefix)
                    throw std::runtime_error("invalid directory entry");
                std::wstring_view name(e->FileName, e->FileNameLength / 2);
                if (name != L"." && name != L"..")
                    entry(parent, name, e->FileAttributes, static_cast<u64>(e->FileId.QuadPart),
                          e->EndOfFile.QuadPart, e->AllocationSize.QuadPart);
                if (e->NextEntryOffset == 0) break;
                if (e->NextEntryOffset % alignof(FILE_ID_BOTH_DIR_INFO))
                    throw std::runtime_error("invalid directory entry alignment");
                offset += e->NextEntryOffset;
            }
        }
    }

    void finish() {
        while (!pending.empty()) {
            Directory current = std::move(pending.back());
            pending.pop_back();
            auto handle = open(current.path, FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES);
            BY_HANDLE_FILE_INFORMATION info{};
            if (!handle || !GetFileInformationByHandle(handle.get(), &info) ||
                type(info.dwFileAttributes) != Kind::directory || info.dwVolumeSerialNumber != device ||
                (current.inode && identity(info) != current.inode)) {
                emit({Kind::error, current.node, 0, 0, 0, 0, {}});
                continue;
            }
            enumerate(current, handle.get());
        }
    }
private:
    void entry(const Directory& parent, std::wstring_view name, DWORD attrs, u64 inode, LONGLONG logical, LONGLONG allocation) {
        auto kind = type(attrs);
        if (kind == Kind::directory && exclude.contains(device, inode)) kind = Kind::other;
        if (kind == Kind::file && logical < 0) {
            emit({Kind::error, parent.node, 0, 0, 0, 0, bytes(name)});
            return;
        }
        u64 id = emit({kind, parent.node, kind == Kind::file ? static_cast<u64>(logical) : 0,
                       kind == Kind::file ? (allocation < 0 ? unknown : static_cast<u64>(allocation)) : 0,
                       device, inode, bytes(name)});
        if (kind == Kind::directory && recursive) pending.push_back({child_path(parent.path, name), id, inode});
    }

    void compatibility_walk(const Directory& parent) {
        WIN32_FIND_DATAW data{};
        auto raw = FindFirstFileExW(child_path(parent.path, L"*").c_str(), FindExInfoBasic, &data,
                                    FindExSearchNameMatch, nullptr, 0);
        if (raw == INVALID_HANDLE_VALUE) {
            if (GetLastError() != ERROR_FILE_NOT_FOUND) emit({Kind::error, parent.node, 0, 0, 0, 0, {}});
            return;
        }
        std::unique_ptr<void, CloseFind> search(raw);
        do {
            std::wstring_view name(data.cFileName);
            if (name == L"." || name == L"..") continue;
            auto handle = open(child_path(parent.path, name), FILE_READ_ATTRIBUTES);
            BY_HANDLE_FILE_INFORMATION info{};
            if (!handle || !GetFileInformationByHandle(handle.get(), &info)) {
                emit({Kind::error, parent.node, 0, 0, 0, 0, bytes(name)});
                continue;
            }
            FILE_STANDARD_INFO standard{};
            LONGLONG size = (static_cast<u64>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
            LONGLONG allocated = -1;
            if (type(info.dwFileAttributes) == Kind::file &&
                GetFileInformationByHandleEx(handle.get(), FileStandardInfo, &standard, sizeof(standard))) {
                size = standard.EndOfFile.QuadPart;
                allocated = standard.AllocationSize.QuadPart;
            }
            entry(parent, name, info.dwFileAttributes, identity(info), size, allocated);
        } while (FindNextFileW(search.get(), &data));
        if (GetLastError() != ERROR_NO_MORE_FILES) emit({Kind::error, parent.node, 0, 0, 0, 0, {}});
    }
    const Sink& emit;
    bool recursive;
    DWORD device;
    ExcludedDirectories exclude;
    std::vector<Directory> pending;
    alignas(FILE_ID_BOTH_DIR_INFO) std::array<std::byte, 64 * 1024> buffer;
};
} // namespace

void scan(const std::filesystem::path& path, const Sink& emit, bool recursive) {
    auto full = absolute_path(path);
    auto handle = open(full, FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES);
    if (!handle) throw std::system_error(GetLastError(), std::system_category(), "cannot open scan root");
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(handle.get(), &info))
        throw std::system_error(GetLastError(), std::system_category(), "cannot identify scan root");
    if (type(info.dwFileAttributes) != Kind::directory)
        throw std::invalid_argument("scan root must be a directory on the selected filesystem");
    u64 root = emit({Kind::directory, unknown, 0, 0, info.dwVolumeSerialNumber, identity(info), {}});
    Walker walker(emit, recursive, info.dwVolumeSerialNumber);
    walker.enumerate({std::move(full), root, identity(info)}, handle.get());
    walker.finish();
}
} // namespace dd
