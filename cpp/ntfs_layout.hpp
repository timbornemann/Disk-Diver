#pragma once
#include "scanner.hpp"
#include <span>

namespace dd::ntfs {
// Collect all batches before resolving parent IDs or forwarding any results.
class Layout {
public:
    void parse(std::span<const std::byte> bytes);
    void write(u64 volume, u64 root, const Sink& emit);
private:
    struct File {
        Kind kind;
        u64 logical = 0, allocated = 0;
        bool has_data = false;
    };
    struct Link {
        u64 parent, child;
        std::wstring name;
    };
    std::unordered_map<u64, File> files;
    std::vector<Link> links;
};
}
