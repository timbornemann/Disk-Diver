#include "scanner.hpp"
#include "excluded_dirs.hpp"
#include <cerrno>
#include <dirent.h>
#include <fcntl.h>
#include <fstream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <system_error>
#include <unistd.h>

namespace dd {
namespace {
struct CloseDir {
    void operator()(DIR* dir) const noexcept { closedir(dir); }
};
using Directory = std::unique_ptr<DIR, CloseDir>;

Directory open_directory(int parent, const char* name) {
    int fd = openat(parent, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return {};
    Directory dir(fdopendir(fd));
    if (!dir) {
        auto cause = errno;
        close(fd);
        errno = cause;
    }
    return dir;
}

struct Metadata {
    mode_t mode{};
    u64 device{}, inode{}, logical{}, allocated = unknown, mount{};
};

// Linux <5.8 lacks STATX_MNT_ID. fdinfo also distinguishes bind mounts on the same device.
bool read_mount_id(int dir, const char* name, u64& id) {
    bool existing = *name == '\0';
    int fd = existing ? dir : openat(dir, name, O_PATH | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return false;
    std::ifstream info("/proc/self/fdinfo/" + std::to_string(fd));
    std::string key;
    bool found = false;
    while (info >> key) {
        if (key == "mnt_id:") {
            found = static_cast<bool>(info >> id);
            break;
        }
        info.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
    }
    if (!existing) close(fd);
    return found;
}

bool describe(int dir, const char* name, Metadata& result) {
    const int flags = AT_NO_AUTOMOUNT | AT_SYMLINK_NOFOLLOW | (*name ? 0 : AT_EMPTY_PATH);
    struct statx sx{};
    unsigned mask = STATX_BASIC_STATS;
#ifdef STATX_MNT_ID
    mask |= STATX_MNT_ID;
#endif
    if (statx(dir, name, flags, mask, &sx) == 0) {
        if (!(sx.stx_mask & STATX_TYPE)) return false;
        result.mode = sx.stx_mode;
        result.device = makedev(sx.stx_dev_major, sx.stx_dev_minor);
        result.inode = (sx.stx_mask & STATX_INO) ? sx.stx_ino : 0;
        if (S_ISREG(result.mode)) {
            if (!(sx.stx_mask & STATX_SIZE)) return false;
            result.logical = sx.stx_size;
            if ((sx.stx_mask & STATX_BLOCKS) && sx.stx_blocks <= unknown / 512)
                result.allocated = sx.stx_blocks * 512;
        }
#ifdef STATX_MNT_ID
        if (sx.stx_mask & STATX_MNT_ID) {
            result.mount = sx.stx_mnt_id;
            return true;
        }
#endif
    } else {
        if (errno != ENOSYS && errno != EINVAL && errno != EOPNOTSUPP) return false;
        struct stat st{};
        if (fstatat(dir, name, &st, flags) < 0) return false;
        result.mode = st.st_mode;
        result.device = st.st_dev;
        result.inode = st.st_ino;
        if (S_ISREG(st.st_mode)) {
            result.logical = st.st_size;
            if (st.st_blocks >= 0 && static_cast<u64>(st.st_blocks) <= unknown / 512)
                result.allocated = static_cast<u64>(st.st_blocks) * 512;
        }
    }
    return read_mount_id(dir, name, result.mount);
}

struct Frame {
    Directory dir;
    u64 id;
};
} // namespace

void scan(const std::filesystem::path& path, const Sink& emit, bool recursive) {
    const ExcludedDirectories exclude;
    auto root_path = path;
    while (!root_path.empty() && root_path != root_path.root_path() && root_path.filename().empty())
        root_path = root_path.parent_path();
    auto root = open_directory(AT_FDCWD, root_path.c_str());
    if (!root) throw std::system_error(errno, std::generic_category(), "cannot open scan root");
    Metadata base;
    if (!describe(dirfd(root.get()), "", base)) throw std::runtime_error("cannot identify scan root");
    u64 root_id = emit({Kind::directory, unknown, 0, 0, base.device, base.inode, {}});
    std::vector<Frame> stack;
    stack.push_back({std::move(root), root_id});

    while (!stack.empty()) {
        auto& frame = stack.back();
        errno = 0;
        auto* entry = readdir(frame.dir.get());
        if (!entry) {
            if (errno) emit({Kind::error, frame.id, 0, 0, 0, 0, {}});
            stack.pop_back();
            continue;
        }
        std::string_view name(entry->d_name);
        if (name == "." || name == "..") continue;
        Metadata m;
        auto fd = dirfd(frame.dir.get());
        if (!describe(fd, entry->d_name, m)) {
            emit({Kind::error, frame.id, 0, 0, 0, 0, name});
            continue;
        }
        Kind kind = Kind::other;
        if (m.device == base.device && m.mount == base.mount) {
            if (S_ISREG(m.mode)) kind = Kind::file;
            if (S_ISDIR(m.mode) && !exclude.contains(m.device, m.inode)) kind = Kind::directory;
        }
        const u64 id = emit({kind, frame.id, kind == Kind::file ? m.logical : 0,
                             kind == Kind::file ? m.allocated : 0, m.device, m.inode, name});
        if (kind != Kind::directory || !recursive) continue;
        auto child = open_directory(fd, entry->d_name);
        Metadata opened;
        if (!child || !describe(dirfd(child.get()), "", opened) || opened.inode != m.inode ||
            opened.device != m.device || opened.mount != m.mount) {
            emit({Kind::error, id, 0, 0, 0, 0, {}});
            continue;
        }
        stack.push_back({std::move(child), id});
    }
}
} // namespace dd
