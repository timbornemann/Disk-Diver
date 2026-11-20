#include "scanner.hpp"
#include <array>
#include <cstring>
#include <istream>
#include <ostream>
#include <stdexcept>

namespace dd {
namespace {
constexpr std::uint32_t max_name_bytes = 65534;
using Header = std::array<char, 45>;

void put(char* p, u64 value, unsigned bytes) {
    for (unsigned i = 0; i < bytes; ++i) p[i] = static_cast<char>(value >> (8 * i));
}
u64 get(const char* p, unsigned bytes) {
    u64 value = 0;
    for (unsigned i = 0; i < bytes; ++i)
        value |= static_cast<u64>(static_cast<unsigned char>(p[i])) << (8 * i);
    return value;
}
void checked_read(std::istream& stream, char* target, std::streamsize size) {
    if (!stream.read(target, size)) throw std::runtime_error("truncated DDV1 capture");
}
void add_size(u64& total, u64 amount) {
    if (unknown - total < amount) throw std::overflow_error("byte sum exceeds 64 bits");
    total += amount;
}
} // namespace

Encoder::Encoder(std::ostream& stream) : output(stream) {}

u64 Encoder::add(const Record& r) {
    if (count == 0) output.write("DDV1", 4);
    if (r.name.size() > max_name_bytes) throw std::length_error("invalid name length");
    Header header{};
    header[0] = static_cast<char>(r.kind);
    put(header.data() + 1, r.parent, 8);
    put(header.data() + 9, r.logical, 8);
    put(header.data() + 17, r.allocated, 8);
    put(header.data() + 25, r.device, 8);
    put(header.data() + 33, r.inode, 8);
    put(header.data() + 41, r.name.size(), 4);
    output.write(header.data(), header.size());
    if (!r.name.empty()) output.write(r.name.data(), r.name.size());
    if (!output) throw std::runtime_error("cannot write DDV1 output");
    return count++;
}

void Encoder::finish() {
    output.put(static_cast<char>(Kind::end));
    if (!output.flush()) throw std::runtime_error("cannot finish DDV1 output");
}

void replay(std::istream& input, const Sink& emit) {
    std::array<char, 4> magic;
    checked_read(input, magic.data(), magic.size());
    if (std::string_view(magic.data(), 4) != "DDV1") throw std::runtime_error("invalid DDV1 magic");
    Header h;
    std::string name;
    for (;;) {
        checked_read(input, h.data(), 1);
        auto kind = static_cast<Kind>(h[0]);
        if (kind == Kind::end) {
            if (input.peek() != std::char_traits<char>::eof() || input.bad())
                throw std::runtime_error("invalid end of DDV1 capture");
            return;
        }
        if (kind < Kind::directory || kind > Kind::error) throw std::runtime_error("invalid DDV1 record type");
        checked_read(input, h.data() + 1, h.size() - 1);
        auto size = get(h.data() + 41, 4);
        if (size > max_name_bytes) throw std::runtime_error("invalid DDV1 name length");
        name.resize(static_cast<std::size_t>(size));
        checked_read(input, name.data(), static_cast<std::streamsize>(size));
        emit({kind, get(h.data() + 1, 8), get(h.data() + 9, 8), get(h.data() + 17, 8),
              get(h.data() + 25, 8), get(h.data() + 33, 8), name});
    }
}

std::size_t Index::IdentityHash::operator()(Identity id) const noexcept {
    return std::hash<u64>{}(id.inode ^ (id.device + 0x9e3779b97f4a7c15ULL +
                                      (id.inode << 6) + (id.inode >> 2)));
}

std::basic_string<std::filesystem::path::value_type> Index::path_key(u64 node) const {
    using Char = std::filesystem::path::value_type;
    std::vector<u64> ancestors;
    while (node != 0) {
        ancestors.push_back(node);
        node = nodes[node].parent;
    }
    std::basic_string<Char> path;
    for (auto it = ancestors.rbegin(); it != ancestors.rend(); ++it) {
        const auto& entry = nodes[*it];
        path.push_back(static_cast<Char>('/'));
        auto at = path.size();
        path.resize(at + entry.name_length / sizeof(Char));
        std::memcpy(path.data() + at, names.data() + entry.name_at,
                    (path.size() - at) * sizeof(Char));
    }
    return path;
}

u64 Index::add(const Record& r) {
    const u64 id = nodes.size();
    if (id == 0) {
        if (r.kind != Kind::directory || r.parent != unknown || !r.name.empty())
            throw std::runtime_error("invalid DDV1 root");
    } else if (r.parent >= id || (nodes[r.parent].kind != Kind::directory &&
                                 (r.kind != Kind::error || !r.name.empty()))) {
        throw std::runtime_error("invalid DDV1 parent");
    } else if (r.name.empty() && r.kind != Kind::error) {
        throw std::runtime_error("empty DDV1 component");
    }
    if (r.name.size() > max_name_bytes) throw std::runtime_error("invalid DDV1 name length");
    nodes.push_back({r.parent, names.size(), r.logical, r.allocated,
                     static_cast<std::uint32_t>(r.name.size()), r.kind, true});
    names.append(r.name);
    switch (r.kind) {
    case Kind::directory: ++directories; break;
    case Kind::other: ++other; break;
    case Kind::error: ++errors; break;
    case Kind::file: {
        ++files;
        if (r.inode != 0) {
            auto [it, inserted] = owners.try_emplace(Identity{r.device, r.inode}, id);
            if (!inserted) {
                if (path_key(id) < path_key(it->second)) {
                    nodes[it->second].owner = false;
                    it->second = id;
                } else {
                    nodes[id].owner = false;
                }
            }
        }
        break;
    }
    default: throw std::runtime_error("invalid DDV1 record type");
    }
    return id;
}

void Index::summarize(std::ostream& out) {
    if (nodes.empty()) throw std::runtime_error("DDV1 root missing");
    u64 unknown_allocations = 0;
    // Parents precede children. Fold once, without recursive calls or path reconstruction.
    for (std::size_t i = nodes.size(); i-- > 1;) {
        auto& n = nodes[i];
        if (n.kind == Kind::directory || (n.kind == Kind::file && n.owner)) {
            auto& parent = nodes[n.parent];
            add_size(parent.logical, n.logical);
            if (n.kind == Kind::file && n.allocated == unknown) ++unknown_allocations;
            else add_size(parent.allocated, n.allocated);
        }
    }
    out << "{\"files\":" << files << ",\"directories\":" << directories
        << ",\"other\":" << other << ",\"errors\":" << errors
        << ",\"logical\":" << nodes[0].logical << ",\"allocated\":" << nodes[0].allocated
        << ",\"nodes\":" << nodes.size() << ",\"complete\":" << (errors || unknown_allocations ? "false" : "true")
        << ",\"unknown_allocations\":" << unknown_allocations << "}\n";
}
} // namespace dd
