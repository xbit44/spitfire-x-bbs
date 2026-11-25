#include "linux_ssh_host_key.hpp"
#include "sfx/platform/linux/private_files.hpp"
#include <deucessh-algorithms.h>
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace sfx::apps::runtime {
namespace {
struct Descriptor {
    int value{-1};
    explicit Descriptor(int fd) : value(fd) {}
    ~Descriptor() { if (value >= 0) close(value); }
    Descriptor(const Descriptor&) = delete;
    Descriptor& operator=(const Descriptor&) = delete;
};
bool owned(int fd, bool directory, bool private_access) {
    struct stat info{};
    return fd >= 0 && fstat(fd, &info) == 0 && info.st_uid == geteuid() &&
        (directory ? S_ISDIR(info.st_mode) : S_ISREG(info.st_mode)) &&
        (info.st_mode & (private_access ? 077 : 022)) == 0 &&
        (directory || (info.st_nlink == 1 && info.st_size > 0 && info.st_size <= 16384));
}
int no_password(char*, int, int, void*) { return 0; }
std::string descriptor_path(int fd) { return "/proc/self/fd/" + std::to_string(fd); }
}

bool load_linux_ssh_host_key(const std::filesystem::path& root,
    const std::wstring& owner, std::string& diagnostic,
    std::uint64_t& generation_attempts, std::uint64_t& load_attempts) {
    const auto fail = [&](const char* reason) { diagnostic = reason; return false; };
    if (geteuid() == 0 || owner != std::to_wstring(geteuid()))
        return fail("ssh_host_key_owner_missing");
    const auto board=sfx::platform::linux_host::open_owned_directory(root,false);
    const Descriptor config(openat(board.get(), "config",
        O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (!owned(board.get(), true, false) || !owned(config.value, true, true))
        return fail("ssh_host_key_config_root_insecure");
    struct stat existing{};
    if (fstatat(config.value, "ssh_host_ed25519.pem", &existing, AT_SYMLINK_NOFOLLOW) != 0) {
        if (errno != ENOENT) return fail("ssh_host_key_unreadable");
        const Descriptor pending(openat(config.value, "ssh_host_ed25519.pending",
            O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600));
        if (pending.value < 0) return fail("ssh_host_key_pending_exists_or_unwritable");
        ++generation_attempts;
        // The library opens the already pinned object, never an unchecked path.
        const auto pinned = descriptor_path(pending.value);
        if (dssh_ed25519_generate_key() < 0 ||
            dssh_ed25519_save_key_file(pinned.c_str(), nullptr, nullptr) < 0 ||
            !owned(pending.value, false, true) || fsync(pending.value) != 0)
            return fail("ssh_host_key_generation_or_protection_failed");
        struct stat current{}, original{};
        if (fstat(pending.value, &original) != 0 ||
            fstatat(config.value, "ssh_host_ed25519.pending", &current, AT_SYMLINK_NOFOLLOW) != 0 ||
            current.st_dev != original.st_dev || current.st_ino != original.st_ino)
            return fail("ssh_host_key_pending_changed");
        // linkat is atomic and refuses overwrite; keep an interrupted pending file.
        if (linkat(config.value, "ssh_host_ed25519.pending", config.value,
                "ssh_host_ed25519.pem", 0) != 0)
            return fail("ssh_host_key_publication_failed");
        if (unlinkat(config.value, "ssh_host_ed25519.pending", 0) != 0 || fsync(config.value) != 0)
            return fail("ssh_host_key_publication_flush_failed");
    }
    const Descriptor key(openat(config.value, "ssh_host_ed25519.pem",
        O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK));
    if (!owned(key.value, false, true)) return fail("ssh_host_key_insecure_or_unreadable");
    ++load_attempts;
    if (dssh_ed25519_load_key_file(descriptor_path(key.value).c_str(), no_password, nullptr) < 0)
        return fail("ssh_host_key_invalid");
    diagnostic = "ssh_host_key_ready";
    return true;
}
} // namespace sfx::apps::runtime
