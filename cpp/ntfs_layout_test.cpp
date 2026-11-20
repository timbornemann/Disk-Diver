#include "ntfs_layout.hpp"
#include <windows.h>
#include <winioctl.h>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

namespace {
using dd::u64;
using Bytes = std::vector<std::byte>;
void check(bool condition) { if (!condition) throw std::runtime_error("layout test failed"); }

// One complete file record per driver response. Deliberately feed children before their parents.
Bytes batch(u64 id, u64 parent, DWORD attributes, std::wstring_view name, bool include_data = false) {
    QUERY_FILE_LAYOUT_OUTPUT output{};
    output.FileEntryCount = 1;
    output.FirstFileOffset = sizeof(output);
    FILE_LAYOUT_ENTRY file{};
    file.FileReferenceNumber = id;
    file.Version = 1;
    file.FileAttributes = attributes;
    file.FirstNameOffset = sizeof(file);
    FILE_LAYOUT_NAME_ENTRY link{};
    link.ParentFileReferenceNumber = parent;
    link.FileNameLength = static_cast<DWORD>(name.size() * sizeof(wchar_t));
    link.Flags = FILE_LAYOUT_NAME_ENTRY_PRIMARY;
    auto name_size = offsetof(FILE_LAYOUT_NAME_ENTRY, FileName) + link.FileNameLength;
    auto name_padded = (name_size + 7) & ~std::size_t{7};
    STREAM_LAYOUT_ENTRY stream{};
    stream.Version = 2;
    stream.AttributeTypeCode = 0x80;
    stream.EndOfFile.QuadPart = 9000;
    stream.AllocationSize.QuadPart = 4096;
    if (include_data) file.FirstStreamOffset = static_cast<DWORD>(sizeof(file) + name_padded);
    Bytes bytes(sizeof(output) + sizeof(file) + name_padded + (include_data ? sizeof(stream) : 0));
    auto* first = bytes.data() + sizeof(output);
    std::memcpy(bytes.data(), &output, sizeof(output));
    std::memcpy(first, &file, sizeof(file));
    std::memcpy(first + file.FirstNameOffset, &link, offsetof(FILE_LAYOUT_NAME_ENTRY, FileName));
    std::memcpy(first + file.FirstNameOffset + offsetof(FILE_LAYOUT_NAME_ENTRY, FileName), name.data(), link.FileNameLength);
    if (include_data) std::memcpy(first + file.FirstStreamOffset, &stream, sizeof(stream));
    return bytes;
}

bool rejects(auto&& action) {
    try { action(); } catch (const std::runtime_error&) { return true; }
    return false;
}
}

int main() {
    constexpr u64 root = 5;
    auto root_record = batch(root, root, FILE_ATTRIBUTE_DIRECTORY, L".");
    auto file = batch(82, 74, 0, L"large", true);
    dd::ntfs::Layout valid;
    valid.parse(file);
    valid.parse(batch(74, root, FILE_ATTRIBUTE_DIRECTORY, L"folder"));
    valid.parse(batch(11, root, FILE_ATTRIBUTE_DIRECTORY, L"$Extend"));
    valid.parse(batch(90, 11, 0, L"hidden NTFS metadata"));
    valid.parse(batch(91, root, FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY, L"junction"));
    valid.parse(batch(92, 91, 0, L"beneath reparse"));
    valid.parse(root_record);
    std::vector<u64> ids;
    valid.write(123, root, [&](const dd::Record& r) {
        check(r.device == 123);
        if (r.inode == 82) check(r.logical == 9000 && r.allocated == 4096 && ids.at(r.parent) == 74);
        if (r.inode == 91) check(r.kind == dd::Kind::other);
        ids.push_back(r.inode);
        return ids.size() - 1;
    });
    check((ids == std::vector<u64>{root, 74, 91, 82}));
    _putenv_s("DISK_DIVER_SKIP_DIR_IDS", "123:74,123:5");
    ids.clear();
    valid.write(123, root, [&](const dd::Record& r) {
        if (r.inode == root) check(r.kind == dd::Kind::directory);
        if (r.inode == 74) check(r.kind == dd::Kind::other && !r.logical && !r.allocated);
        ids.push_back(r.inode);
        return ids.size() - 1;
    });
    check((ids == std::vector<u64>{root, 74, 91}));
    _putenv_s("DISK_DIVER_SKIP_DIR_IDS", "");
    dd::ntfs::Layout orphan;
    orphan.parse(root_record);
    orphan.parse(file);
    int emitted = 0;
    check(rejects([&] { orphan.write(1, root, [&](const dd::Record&) { return emitted++; }); }));
    check(emitted == 0);
    check(rejects([&] { orphan.parse(root_record); }));
    dd::ntfs::Layout truncated;
    check(rejects([&] { truncated.parse(std::span(file).first(sizeof(QUERY_FILE_LAYOUT_OUTPUT) + 3)); }));
    return EXIT_SUCCESS;
}
