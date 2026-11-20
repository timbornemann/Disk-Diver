#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <iosfwd>
#include <limits>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace dd {
using u64 = std::uint64_t;
inline constexpr u64 unknown = std::numeric_limits<u64>::max();
enum class Kind : std::uint8_t { directory = 1, file, other, error, end };

// Names are native component bytes. Record views only live through the sink call.
struct Record {
    Kind kind;
    u64 parent, logical, allocated, device, inode;
    std::string_view name;
};
using Sink = std::function<u64(const Record&)>;
void scan(const std::filesystem::path& path, const Sink& emit, bool recursive = true);
#ifdef _WIN32
void scan_ntfs_layout(const std::filesystem::path& path, const Sink& emit);
#endif

class Encoder {
public:
    explicit Encoder(std::ostream& stream);
    u64 add(const Record&);
    void finish();
private:
    std::ostream& output;
    u64 count = 0;
};
void replay(std::istream&, const Sink&);

class Index {
public:
    u64 add(const Record&);
    void summarize(std::ostream&);
private:
    struct Node {
        u64 parent, name_at, logical, allocated;
        std::uint32_t name_length;
        Kind kind;
        bool owner;
    };
    struct Identity {
        u64 device, inode;
        bool operator==(const Identity&) const = default;
    };
    struct IdentityHash {
        std::size_t operator()(Identity id) const noexcept;
    };
    std::basic_string<std::filesystem::path::value_type> path_key(u64 node) const;
    std::vector<Node> nodes;
    std::string names;
    std::unordered_map<Identity, u64, IdentityHash> owners;
    u64 files = 0, directories = 0, other = 0, errors = 0;
};
} // namespace dd
