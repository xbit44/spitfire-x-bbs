#include "sfx/platform/linux/installation_layout.hpp"
#include "sfx/platform/linux/private_files.hpp"
#include "sfx/archive/physical_layout.hpp"
#include <algorithm>
#include <array>
#include <cerrno>
#include <dirent.h>
#include <fcntl.h>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace sfx::platform::linux_host::layout {
namespace {
namespace fs = std::filesystem;
void require(bool condition, const char* diagnostic) {
    if (!condition) throw std::runtime_error(diagnostic);
}
std::string lower(std::string_view value) {
    std::string result(value);
    for (auto& byte : result) if (byte >= 'A' && byte <= 'Z') byte = static_cast<char>(byte + ('a' - 'A'));
    return result;
}
bool prefix(std::string_view value, std::string_view start) {
    return value == start || (value.size() > start.size() && value.substr(0, start.size()) == start && value[start.size()] == '/');
}
std::vector<std::string> names(int directory) {
    FileDescriptor copy(openat(directory, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    require(bool(copy), "linux_layout_directory_scan_failed");
    const auto descriptor = copy.release();
    DIR* raw = fdopendir(descriptor);
    if (!raw) { close(descriptor); throw std::runtime_error("linux_layout_directory_scan_failed"); }
    struct Close { DIR* value; ~Close() { closedir(value); } } close{raw};
    std::vector<std::string> result;
    errno = 0;
    while (const auto* entry = readdir(raw)) {
        const std::string name(entry->d_name);
        if (name != "." && name != "..") {
            require(result.size() < 4096, "linux_layout_directory_entry_bound");
            result.push_back(name);
        }
        errno = 0;
    }
    require(errno == 0, "linux_layout_directory_scan_failed");
    return result;
}
bool exists(int directory, const char* name) {
    struct stat value{};
    if (fstatat(directory, name, &value, AT_SYMLINK_NOFOLLOW) == 0) return true;
    require(errno == ENOENT, "linux_layout_object_inspection_failed");
    return false;
}
void verify_marker(const fs::path& root) {
    auto config = open_owned_directory(root / "config");
    const auto bytes = read_private_file(config.get(), "linux-layout.v1", 128);
    require(std::string(bytes.begin(), bytes.end()) == marker_text, "linux_lowercase_layout_marker_invalid");
}
void fixed_names(const fs::path& directory, const std::vector<std::string_view>& expected) {
    if (!fs::exists(directory)) return;
    auto held = open_owned_directory(directory);
    for (const auto& name : names(held.get())) {
        const auto folded = lower(name);
        if(folded.rfind("sfxnode",0)==0&&folded.size()>12&&folded.substr(folded.size()-6)==".state"&&
            folded.substr(7,folded.size()-13).find_first_not_of("0123456789")==std::string::npos)
            require(name==folded,"linux_lowercase_layout_mixed_owned_name_refused");
        for (const auto value : expected) {
            const bool sqlite = value == "sfx.db" && (folded == value || folded.rfind("sfx.db-", 0) == 0 || folded.rfind("sfx.db.", 0) == 0);
            if (folded == value || sqlite) require(name == folded, "linux_lowercase_layout_mixed_owned_name_refused");
        }
    }
}
}

std::filesystem::path physical_relative(std::string_view canonical_owned) {
    require(!canonical_owned.empty() && canonical_owned.front() != '/' && canonical_owned.find_first_of("\\:") == canonical_owned.npos && canonical_owned.find('\0') == canonical_owned.npos,
        "linux_layout_owned_relative_required");
    const fs::path relative = fs::u8path(canonical_owned);
    require(relative.generic_u8string() == canonical_owned, "linux_layout_path_alias_refused");
    for (const auto& part : relative) require(part != "." && part != ".." && !part.empty(), "linux_layout_path_alias_refused");
    constexpr std::string_view program_paths[]{"Bin/SFXRuntime", "Bin/SFXWeb", "Bin/SFXHostLookup", "Bin/SFXBinkD", "Bin/Tools/SFXFileTransfer", "Bin/Tools/SFXQWKTransfer"};
    for (const auto value : program_paths) if (canonical_owned == value) return fs::u8path(lower(value));
    if ((prefix(canonical_owned, "Docs/Doors") || prefix(canonical_owned, "Docs/Sysop") || prefix(canonical_owned, "Docs/ThirdParty")) &&
        !prefix(canonical_owned, "Docs/ThirdParty/ClamAV")) return fs::u8path(lower(canonical_owned));
    // More-specific native wrapper directories precede their broader roots.
    constexpr std::string_view owned_prefixes[]{
        "Docs/ThirdParty/ClamAV", "Bin/tzdata", "Bin/ClamAV", "Bin/Dosemu", "Bin/Tools",
        "Docs/Sysop", "Docs/ThirdParty", "Docs/Doors", "Service"
    };
    for (const auto value : owned_prefixes) if (prefix(canonical_owned, value)) {
        const auto suffix = canonical_owned.substr(value.size());
        return fs::u8path(lower(value) + std::string(suffix));
    }
    return fs::u8path(archive::physical_relative(canonical_owned, archive::PhysicalLayout::linux_lowercase_v1));
}
std::filesystem::path path(const fs::path& root, std::string_view canonical_owned) {
    require(root.is_absolute(), "linux_layout_absolute_root_required");
    return root / physical_relative(canonical_owned);
}
std::string owned_name(std::string_view generated_leaf) {
    require(valid_private_name(generated_leaf), "linux_layout_generated_name_invalid");
    return lower(generated_leaf);
}
std::string archive_relative(std::string_view native_relative) {
    return archive::archive_relative(native_relative, archive::PhysicalLayout::linux_lowercase_v1);
}
adapters::storage::SqliteStorageSettings storage_settings(const fs::path& root) {
    adapters::storage::SqliteStorageSettings result;
    result.data_directory = path(root, "Data");
    result.layout_profile = adapters::storage::SqliteStorageLayoutProfile::linux_lowercase_v1;
    return result;
}
void refuse_legacy_or_mixed(const fs::path& root) {
    auto directory = open_owned_directory(root);
    constexpr std::string_view roots[]{"bin", "web", "docs", "display", "doors", "botgate", "config", "data", "files", "ftn", "logs", "backup", "work", "node", "management", "games", "service"};
    for (const auto& name : names(directory.get())) {
        const auto folded = lower(name);
        for (const auto expected : roots) if (folded == expected) require(name == expected, "linux_lowercase_layout_legacy_or_mixed_tree_refused");
    }
    fixed_names(root / "config", {"sfx.conf", "sfxsecurity.conf", "sfxscheduler.conf", "sfxmessageretention.conf", "sfxmessageintegrity.conf", "sfxnodes.state", "sfxnode1.state", "joker.dat", "linux-layout.v1"});
    fixed_names(root / "data", {"sfx.db", "sfxscheduler.state", "sfxmessageretention.state", "sfxmessageintegrity.state"});
    fixed_names(root / "logs", {"callers.log", "heysysop.log", "sfxruntime.log"});
    // Closed native wrapper/default folders only. Do not traverse arbitrary
    // owner libraries, game trees, uploaded contents or IANA/vendor islands.
    fixed_names(root / "bin", {"sfxruntime","sfxweb","sfxhostlookup","sfxbinkd","tools","clamav","dosemu","tzdata"});
    fixed_names(root / "bin/tools", {"sfxfiletransfer","sfxqwktransfer"});
    fixed_names(root / "docs", {"sysop","doors","thirdparty"});
    fixed_names(root / "data", {"fileintake","fileremoved","clamav"});
    fixed_names(root / "work", {"clamav"});
    fixed_names(root / "backup", {"scheduled","upgrades","recovery","restorefiles"});
    fixed_names(root / "files", {"general","uploads","uploadtosysop"});
    fixed_names(root / "ftn", {"inbound","outbound","partial","nonsecure","bad","processed","conflicts","external"});
    fixed_names(root / "ftn/external", {"inbound","temp","records","consumed","runs","tics"});
    fixed_names(root / "ftn/external/inbound", {"secure","nonsecure","links"});
}
void require_current(const fs::path& root) {
    refuse_legacy_or_mixed(root);
    verify_marker(root);
}
namespace {
bool current_or_fresh(const fs::path& root) {
    refuse_legacy_or_mixed(root);
    auto directory = open_owned_directory(root);
    if (exists(directory.get(), "config")) {
        auto config = open_owned_directory(root / "config");
        if (exists(config.get(), "linux-layout.v1")) { verify_marker(root); return true; }
        require(names(config.get()).empty(), "linux_lowercase_layout_marker_required_for_existing_state");
    }
    if (exists(directory.get(), "data")) {
        auto data = open_owned_directory(root / "data");
        require(names(data.get()).empty(), "linux_lowercase_layout_marker_required_for_existing_state");
    }
    require(!exists(directory.get(), ".sfx-installed.json"), "linux_lowercase_layout_installed_marker_missing");
    return false;
}
}
void preflight(const fs::path& root) { (void)current_or_fresh(root); }
void initialize_or_verify(const fs::path& root) {
    if (current_or_fresh(root)) return;
    auto directory = open_owned_directory(root);
    if (!exists(directory.get(), "config")) {
        require(mkdirat(directory.get(), "config", 0700) == 0 && fsync(directory.get()) == 0, "linux_layout_fresh_config_create_failed");
    }
    auto config = open_owned_directory(root / "config");
    publish_private_file(config.get(), "linux-layout.v1", {marker_text.begin(), marker_text.end()}, false);
    verify_marker(root);
}
} // namespace sfx::platform::linux_host::layout
