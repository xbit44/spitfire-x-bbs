#include "sfx/platform/linux/installation_layout.hpp"
#include "sfx/platform/linux/private_files.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace fs = std::filesystem;
namespace native = sfx::platform::linux_host;
namespace layout = native::layout;
namespace {
unsigned checks{};
void check(bool value, const char* why) { ++checks; if (!value) throw std::runtime_error(why); }
void folder(const fs::path& path) { check(fs::create_directory(path) && chmod(path.c_str(), 0700) == 0, "fresh private directory"); }
void put(const fs::path& path, std::string_view bytes) {
    auto directory = native::open_owned_directory(path.parent_path());
    native::publish_private_file(directory.get(), path.filename().string(), {bytes.begin(), bytes.end()}, false);
}
std::string read(const fs::path& path) {
    auto directory = native::open_owned_directory(path.parent_path());
    const auto value = native::read_private_file(directory.get(), path.filename().string(), 4096);
    return {value.begin(), value.end()};
}
template<class Action> bool refused(Action action) { try { action(); return false; } catch (const std::exception&) { return true; } }
}
int main(int argc, char** argv) { try {
    check(argc == 2 && geteuid() != 0, "fresh fixture and ordinary Linux identity required");
    const fs::path root = argv[1]; check(root.is_absolute() && !fs::exists(root), "no reuse or deletion of prior evidence"); folder(root);
    check(layout::physical_relative("Config/SFX.conf") == "config/sfx.conf", "core config native alias");
    check(layout::physical_relative("Data/SFX.DB") == "data/sfx.db", "core SQLite native alias");
    check(layout::physical_relative("Bin/Tools/SFXQWKTransfer") == "bin/tools/sfxqwktransfer", "helper output alias through tools directory");
    check(layout::physical_relative("Service/sfx-0123.service") == "service/sfx-0123.service", "native-only lazy service directory has a central alias outside core archive scope");
    check(layout::physical_relative("FTN/External/Temp") == "ftn/external/temp", "external profile temp structure preserved");
    check(layout::physical_relative("FTN/External/ownernet/Profile") == "ftn/external/ownernet/profile", "central helper delegates per-network owned metadata slot");
    check(layout::physical_relative("FTN/External/ownernet/Outbound.003/00000001.pnt/00000002.flo") == "ftn/external/ownernet/outbound.003/00000001.pnt/00000002.flo", "central helper delegates nondefault BSO zone slot");
    check(layout::physical_relative("FTN/External/Runs/0123456789abcdef0123456789abcdef/Manifest") == "ftn/external/runs/0123456789abcdef0123456789abcdef/manifest", "central helper delegates positional run manifest slot");
    check(layout::physical_relative("Docs/Doors/SFXGuess32/THIRD_PARTY.txt") == "docs/doors/sfxguess32/third_party.txt", "first-party component document alias");
    check(layout::physical_relative("Bin/tzdata/America/Los_Angeles") == "bin/tzdata/America/Los_Angeles", "canonical IANA spelling preserved");
    check(layout::physical_relative("Docs/ThirdParty/ClamAV/COPYING") == "docs/thirdparty/clamav/COPYING", "upstream notice-relative spelling preserved");
    check(layout::path(root, "Files") / "Owner Keeps MyCase.zip" == root / "files/Owner Keeps MyCase.zip", "owner content appended without normalization");
    const std::string identity = "0123456789ABCDEF0123456789ABCDEF";
    check(layout::owned_name("Compaction-" + identity) == "compaction-0123456789abcdef0123456789abcdef" && identity == "0123456789ABCDEF0123456789ABCDEF", "owned filename lowers without mutating identity value");
    const std::string archive_rotation = "Logs/CALLERS-20261116T120000000Z-" + identity + ".LOG";
    check(layout::archive_relative(layout::physical_relative(archive_rotation).generic_string()) == archive_rotation, "native rotation and canonical namespace round-trip");
    for (const auto* bad : {"../Config", "/Config", "Config//SFX.conf", "Config/../Data", "C:/Config"})
        check(refused([&] { (void)layout::physical_relative(bad); }), "owned relative alias/traversal refused");
    const auto fresh = root / "fresh"; folder(fresh); layout::preflight(fresh);
    check(fs::is_empty(fresh), "fresh preflight performs no writes"); layout::initialize_or_verify(fresh); layout::require_current(fresh);
    check(read(fresh / "config/linux-layout.v1") == layout::marker_text, "exact native schema marker is private and verified");
    const auto settings = layout::storage_settings(fresh);
    check(settings.data_directory == fresh / "data" && settings.layout_profile == sfx::adapters::storage::SqliteStorageLayoutProfile::linux_lowercase_v1, "native callers select explicit DB profile");
    const auto copy = root / "OwnerChosenCopy"; fs::copy(fresh, copy, fs::copy_options::recursive); layout::require_current(copy);
    check(read(copy / "config/linux-layout.v1") == layout::marker_text, "schema marker is portable rather than board-ID-bound");
    folder(copy / "files"); folder(copy / "files/OwnerLibrary"); put(copy / "files/OwnerLibrary/OwnerName.ZIP", "owned by the caller\n");
    layout::require_current(copy);
    check(read(copy / "files/OwnerLibrary/OwnerName.ZIP") == "owned by the caller\n", "arbitrary owner library and payload spelling stays exact");
    folder(copy / "files/Uploads");
    check(refused([&] { layout::require_current(copy); }) && !fs::exists(copy / "files/uploads"), "nested mixed default role refuses without a parallel directory");
    const auto service = root / "service-mixed"; folder(service); layout::initialize_or_verify(service); folder(service / "Service");
    check(refused([&] { layout::require_current(service); }) && !fs::exists(service / "service"), "lazy service root is subject to the same mixed-layout guard");
    const std::pair<const char*,const char*> log_names[]{{"CALLERS.LOG","callers.log"},{"HEYSYSOP.LOG","heysysop.log"},{"SFXRuntime.log","sfxruntime.log"}};
    unsigned log_case{};
    for(const auto& names:log_names){
        const auto logs = root / ("legacy-active-log-" + std::to_string(log_case++)); folder(logs); layout::initialize_or_verify(logs); folder(logs / "logs"); put(logs / "logs" / names.first,"retain legacy journal bytes\n");
        check(refused([&] { layout::require_current(logs); }) && !fs::exists(logs / "logs" / names.second) && read(logs / "logs" / names.first)=="retain legacy journal bytes\n", "tagged tree with a legacy active journal refuses without a parallel log or modified bytes");
    }
    const auto notes = root / "owner-log-notes"; folder(notes); layout::initialize_or_verify(notes); folder(notes / "logs"); put(notes / "logs/CALLERS-owner-notes.LOG","owner spelling retained\n"); layout::require_current(notes);
    check(read(notes / "logs/CALLERS-owner-notes.LOG")=="owner spelling retained\n", "unrelated owner log name is outside the closed active-journal alias guard");
    const auto legacy = root / "legacy"; folder(legacy); folder(legacy / "Config"); put(legacy / "Config/SFX.conf", "retain old board\n");
    check(refused([&] { layout::preflight(legacy); }) && !fs::exists(legacy / "config") && !fs::exists(legacy / "data") && read(legacy / "Config/SFX.conf") == "retain old board\n", "legacy refusal cannot create parallel directories or modify original state");
    const auto unmarked = root / "unmarked"; folder(unmarked); folder(unmarked / "data"); put(unmarked / "data/sfx.db", "retained existing database\n");
    check(refused([&] { layout::initialize_or_verify(unmarked); }) && !fs::exists(unmarked / "config") && read(unmarked / "data/sfx.db") == "retained existing database\n", "established unmarked lower data is not reclassified fresh");
    const auto bad_marker = root / "bad-marker"; folder(bad_marker); folder(bad_marker / "config"); put(bad_marker / "config/linux-layout.v1", "other-layout\n");
    check(refused([&] { layout::require_current(bad_marker); }) && read(bad_marker / "config/linux-layout.v1") == "other-layout\n", "different marker schema refused without replacement");
    folder(fresh / "Config"); check(refused([&] { layout::require_current(fresh); }), "mixed known owned root aliases refused");
    const auto link = root / "link"; folder(link); fs::create_directory_symlink(copy / "config", link / "config");
    check(refused([&] { layout::initialize_or_verify(link); }) && fs::is_symlink(link / "config"), "symlink guard retained without repair");
    std::cout << "PASS " << checks << " Linux lowercase layout/schema/exception/refusal checks\n"; return 0;
} catch (const std::exception& error) { std::cerr << "FAIL " << checks << ": " << error.what() << '\n'; return 1; } }
