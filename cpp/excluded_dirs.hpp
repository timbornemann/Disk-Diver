#pragma once

#include "scanner.hpp"
#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <stdexcept>

namespace dd {
class ExcludedDirectories {
public:
    ExcludedDirectories() {
        const char* value = std::getenv("DISK_DIVER_SKIP_DIR_IDS");
        if (!value || !*value) return;
        std::string_view list(value);
        for (;;) {
            const auto next = list.find(',');
            const auto pair = list.substr(0, next);
            const auto colon = pair.find(':');
            if (colon == pair.npos) throw std::invalid_argument("invalid excluded directory identity");
            ids.emplace_back(number(pair.substr(0, colon)), number(pair.substr(colon + 1)));
            if (next == list.npos) break;
            list.remove_prefix(next + 1);
        }
    }
    bool contains(u64 device, u64 inode) const {
        return inode && std::find(ids.begin(), ids.end(), std::pair{device, inode}) != ids.end();
    }
private:
    static u64 number(std::string_view text) {
        u64 out;
        const auto end = text.data() + text.size();
        const auto parsed = std::from_chars(text.data(), end, out);
        if (parsed.ec != std::errc{} || parsed.ptr != end)
            throw std::invalid_argument("invalid excluded directory identity");
        return out;
    }
    std::vector<std::pair<u64, u64>> ids;
};
}
