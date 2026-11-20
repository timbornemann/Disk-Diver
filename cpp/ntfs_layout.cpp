#include "ntfs_layout.hpp"
#include "excluded_dirs.hpp"
#include <windows.h>
#include <winioctl.h>
#include <algorithm>
#include <cstddef>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <system_error>
#include <unordered_set>

namespace dd {
namespace {
using Bytes = std::span<const std::byte>;
void require(bool condition) {
    if (!condition) throw std::runtime_error("incomplete or invalid NTFS layout; use directory scan");
}

// Do not dereference potentially unaligned or variable-length records from the driver.
template<typename T> T header(Bytes bytes, std::size_t length = sizeof(T)) {
    require(bytes.size() >= length);
    T h{};
    std::memcpy(&h, bytes.data(), length);
    return h;
}
Bytes at(Bytes bytes, std::size_t offset, std::size_t min_offset) {
    require(offset >= min_offset && offset < bytes.size());
    return bytes.subspan(offset);
}
std::wstring text(Bytes bytes, std::size_t offset, std::size_t length) {
    require(length % 2 == 0 && offset <= bytes.size() && length <= bytes.size() - offset);
    std::wstring value(length / 2, L'\0');
    std::memcpy(value.data(), bytes.data() + offset, length);
    require(value.find(L'\0') == std::wstring::npos);
    return value;
}
bool reserved(u64 ref) { return (ref & 0x0000FFFFFFFFFFFFULL) < 16; }
struct Closer { void operator()(void* h) const noexcept { CloseHandle(h); } };
using Handle = std::unique_ptr<void, Closer>;

Handle read_handle(const wchar_t* path, DWORD access, DWORD flags) {
    auto raw = CreateFileW(path, access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, flags, nullptr);
    if (raw == INVALID_HANDLE_VALUE)
        throw std::system_error(GetLastError(), std::system_category(), "cannot open local NTFS volume for layout scan");
    return Handle(raw);
}
}

void ntfs::Layout::parse(Bytes bytes) {
    auto output = header<QUERY_FILE_LAYOUT_OUTPUT>(bytes);
    require(output.FileEntryCount > 0);
    auto more = at(bytes, output.FirstFileOffset, sizeof(output));
    for (DWORD n = 0; n < output.FileEntryCount; ++n) {
        auto f = header<FILE_LAYOUT_ENTRY>(more);
        require(f.Version == 1 && (f.NextFileOffset != 0) == (n + 1 < output.FileEntryCount));
        if (f.NextFileOffset) require(f.NextFileOffset >= sizeof(f) && f.NextFileOffset < more.size());
        auto bytes_for_file = f.NextFileOffset ? more.first(f.NextFileOffset) : more;
        Kind kind = Kind::file;
        if (f.FileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DEVICE)) kind = Kind::other;
        else if (f.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) kind = Kind::directory;
        auto [it, inserted] = files.try_emplace(f.FileReferenceNumber, File{kind});
        require(inserted);
        if (f.FirstNameOffset) {
            auto item = at(bytes_for_file, f.FirstNameOffset, sizeof(f));
            for (;;) {
                constexpr auto prefix = offsetof(FILE_LAYOUT_NAME_ENTRY, FileName);
                auto link = header<FILE_LAYOUT_NAME_ENTRY>(item, prefix);
                require(!link.NextNameOffset || (link.NextNameOffset >= prefix && link.NextNameOffset < item.size()));
                auto record = link.NextNameOffset ? item.first(link.NextNameOffset) : item;
                if ((link.Flags & (FILE_LAYOUT_NAME_ENTRY_DOS | FILE_LAYOUT_NAME_ENTRY_PRIMARY)) != FILE_LAYOUT_NAME_ENTRY_DOS) {
                    auto name = text(record, prefix, link.FileNameLength);
                    bool root_name = link.ParentFileReferenceNumber == f.FileReferenceNumber && name == L"\\";
                    require(!name.empty() && (root_name || name.find_first_of(L"/\\") == std::wstring::npos));
                    links.push_back({link.ParentFileReferenceNumber, f.FileReferenceNumber, std::move(name)});
                }
                if (!link.NextNameOffset) break;
                item = item.subspan(link.NextNameOffset);
            }
        }
        if (f.FirstStreamOffset && kind == Kind::file) {
            auto item = at(bytes_for_file, f.FirstStreamOffset, sizeof(f));
            for (;;) {
                constexpr auto prefix = offsetof(STREAM_LAYOUT_ENTRY, StreamIdentifier);
                auto stream = header<STREAM_LAYOUT_ENTRY>(item, prefix);
                require(stream.Version == 1 || stream.Version == 2);
                require(!stream.NextStreamOffset || (stream.NextStreamOffset >= prefix && stream.NextStreamOffset < item.size()));
                auto record = stream.NextStreamOffset ? item.first(stream.NextStreamOffset) : item;
                // NTFS 0x80 is the $DATA attribute. Both names occur for the default stream.
                if (stream.AttributeTypeCode == 0x80) {
                    auto name = text(record, prefix, stream.StreamIdentifierLength);
                    if (name.empty() || name == L"::$DATA") {
                        auto& file = it->second;
                        require(!file.has_data && stream.EndOfFile.QuadPart >= 0 && stream.AllocationSize.QuadPart >= 0);
                        file.has_data = true;
                        file.logical = stream.EndOfFile.QuadPart;
                        file.allocated = stream.AllocationSize.QuadPart;
                    }
                }
                if (!stream.NextStreamOffset) break;
                item = item.subspan(stream.NextStreamOffset);
            }
        }
        if (f.NextFileOffset) more = more.subspan(f.NextFileOffset);
    }
}

void ntfs::Layout::write(u64 volume, u64 root, const Sink& emit) {
    const ExcludedDirectories exclude;
    require(files.contains(root) && files.at(root).kind == Kind::directory);
    std::sort(links.begin(), links.end(), [](const auto& a, const auto& b) {
        return a.parent != b.parent ? a.parent < b.parent : a.name < b.name;
    });
    struct Visit { u64 inode, output; bool visible; };
    std::vector<Visit> queue{{root, 0, true}};
    std::unordered_set<u64> visited{root};
    struct OutputEntry { const Link* link; u64 parent; Kind kind; };
    std::vector<OutputEntry> order;
    for (std::size_t i = 0; i < queue.size(); ++i) {
        auto dir = queue[i];
        auto start = std::lower_bound(links.begin(), links.end(), dir.inode,
            [](const auto& e, u64 p) { return e.parent < p; });
        for (auto it = start; it != links.end() && it->parent == dir.inode; ++it) {
            if (it->child == root && it->parent == root) continue;
            require(it == start || it->name != (it - 1)->name);
            require(it->name != L"." && it->name != L"..");
            auto& file = files.at(it->child);
            auto kind = file.kind;
            if (kind == Kind::directory && exclude.contains(volume, it->child)) kind = Kind::other;
            bool visible = dir.visible && !reserved(it->child);
            u64 output = 0;
            if (visible) {
                require(file.kind != Kind::file || file.has_data);
                order.push_back({&*it, dir.output, kind});
                output = order.size();
            }
            // A directory reparse point can have real children too. Suppress its subtree.
            if (file.kind == Kind::directory || file.kind == Kind::other) {
                bool descend = visible && kind == Kind::directory;
                if (visited.insert(it->child).second)
                    queue.push_back({it->child, output, descend});
                else require(file.kind != Kind::directory);
            }
        }
    }
    // A concurrently moved or missing parent cannot be published as a complete tree.
    for (const auto& link : links) require(visited.contains(link.parent));
    emit({Kind::directory, unknown, 0, 0, volume, root, {}});
    for (const auto& [link, parent, kind] : order) {
        const auto& f = files.at(link->child);
        std::string_view name(reinterpret_cast<const char*>(link->name.data()), link->name.size() * 2);
        emit({kind, parent, f.logical, f.allocated, volume, link->child, name});
    }
}

void scan_ntfs_layout(const std::filesystem::path& path, const Sink& emit) {
    // Accept an explicit drive root only. Never expand a requested subdirectory into a volume.
    auto drive = path.native();
    if (drive.size() != 3 || !((drive[0] >= L'A' && drive[0] <= L'Z') || (drive[0] >= L'a' && drive[0] <= L'z')) ||
        drive[1] != L':' || (drive[2] != L'\\' && drive[2] != L'/'))
        throw std::invalid_argument("layout requires an explicit local NTFS drive root");
    drive[2] = L'\\';
    wchar_t fs[32]{}, guid[MAX_PATH]{};
    DWORD volume = 0;
    if (GetDriveTypeW(drive.c_str()) == DRIVE_REMOTE ||
        !GetVolumeInformationW(drive.c_str(), nullptr, 0, &volume, nullptr, nullptr, fs, 32) ||
        _wcsicmp(fs, L"NTFS") || !GetVolumeNameForVolumeMountPointW(drive.c_str(), guid, MAX_PATH))
        throw std::invalid_argument("layout requires a local NTFS volume");
    auto root = read_handle(guid, FILE_READ_ATTRIBUTES, FILE_FLAG_BACKUP_SEMANTICS);
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(root.get(), &info))
        throw std::system_error(GetLastError(), std::system_category(), "cannot identify NTFS root");
    auto root_id = (static_cast<u64>(info.nFileIndexHigh) << 32) | info.nFileIndexLow;
    std::wstring device(guid);
    device.pop_back();
    auto handle = read_handle(device.c_str(), GENERIC_READ, 0);
    QUERY_FILE_LAYOUT_INPUT input{};
    input.FilterType = QUERY_FILE_LAYOUT_FILTER_TYPE_NONE;
    input.Flags = QUERY_FILE_LAYOUT_RESTART | QUERY_FILE_LAYOUT_INCLUDE_NAMES |
                  QUERY_FILE_LAYOUT_INCLUDE_STREAMS | QUERY_FILE_LAYOUT_INCLUDE_STREAMS_WITH_NO_CLUSTERS_ALLOCATED;
    ntfs::Layout model;
    std::vector<std::byte> buffer(1 << 20);
    for (;;) {
        DWORD returned{};
        if (DeviceIoControl(handle.get(), FSCTL_QUERY_FILE_LAYOUT, &input, sizeof(input), buffer.data(),
                            static_cast<DWORD>(buffer.size()), &returned, nullptr)) {
            require(returned <= buffer.size());
            model.parse(Bytes(buffer).first(returned));
            input.Flags &= ~QUERY_FILE_LAYOUT_RESTART;
            continue;
        }
        auto status = GetLastError();
        if (status == ERROR_HANDLE_EOF) break;
        if (status == ERROR_INSUFFICIENT_BUFFER && buffer.size() < 64 * 1024 * 1024) {
            buffer.resize(buffer.size() * 2);
            continue;
        }
        throw std::system_error(status, std::system_category(), "cannot query NTFS file layout");
    }
    model.write(volume, root_id, emit);
}
} // namespace dd
