#pragma once

#include "sfx/archive/managed_journal_snapshot.hpp"

#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace sfx::archive {

enum class PhysicalLayout { canonical, linux_lowercase_v1 };

inline bool valid_physical_layout(PhysicalLayout layout) noexcept {
    return layout == PhysicalLayout::canonical || layout == PhysicalLayout::linux_lowercase_v1;
}

namespace layout_detail {
inline std::string lower_owned(std::string_view value) {
    std::string result(value);
    for (auto& c : result) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
    return result;
}

// Exact first-party paths only. Never casefold arbitrary contents, game files,
// owner-selected paths or a vendor subtree. Prefix mappings end at a directory.
inline std::string map(std::string_view input, bool native) {
    constexpr std::pair<std::string_view, std::string_view> files[]{
        {"Config/SFX.conf", "config/sfx.conf"},
        {"Config/SFXSecurity.conf", "config/sfxsecurity.conf"},
        {"Config/SFXScheduler.conf", "config/sfxscheduler.conf"},
        {"Config/SFXMessageRetention.conf", "config/sfxmessageretention.conf"},
        {"Config/SFXMessageIntegrity.conf", "config/sfxmessageintegrity.conf"},
        {"Config/SFXNodes.state", "config/sfxnodes.state"},
        {"Config/JOKER.DAT", "config/joker.dat"},
        {"Data/SFX.DB", "data/sfx.db"},
        {"Data/SFXRuntime.lock", "data/sfxruntime.lock"},
        {"Data/SFXScheduler.state", "data/sfxscheduler.state"},
        {"Data/SFXMessageRetention.state", "data/sfxmessageretention.state"},
        {"Data/SFXMessageIntegrity.state", "data/sfxmessageintegrity.state"},
        {"Display/SFNWSLTR.ASC", "display/sfnwsltr.asc"}
    };
    for (const auto& pair : files) {
        const auto from = native ? pair.first : pair.second;
        const auto to = native ? pair.second : pair.first;
        if (input == from) return std::string(to);
        if (pair.first == "Data/SFX.DB" && input.substr(0, from.size()) == from) {
            const auto suffix = input.substr(from.size());
            if (suffix == "-wal" || suffix == "-shm" || suffix == "-journal" ||
                suffix == ".restore-staging" || suffix == ".pre-restore" || suffix == ".failed-restore")
                return std::string(to) + std::string(suffix);
        }
    }
    const auto node_prefix = native ? std::string_view("Config/SFXNode") : std::string_view("config/sfxnode");
    if (input.substr(0, node_prefix.size()) == node_prefix && input.size() > node_prefix.size() + 6 &&
        input.substr(input.size() - 6) == ".state") {
        const auto number = input.substr(node_prefix.size(), input.size() - node_prefix.size() - 6);
        if (number.find_first_not_of("0123456789") == std::string_view::npos)
            return std::string(native ? "config/sfxnode" : "Config/SFXNode") + std::string(number) + ".state";
    }
    if (native && is_linux_journal_snapshot_member(input)) return lower_owned(input);
    if (!native && input.substr(0, 5) == "logs/") {
        const auto leaf = input.substr(5);
        constexpr std::pair<std::string_view, std::string_view> journals[]{
            {"callers", "CALLERS"}, {"heysysop", "HEYSYSOP"}, {"sfxruntime", "SFXRuntime"}
        };
        for (const auto& pair : journals) {
            std::string candidate;
            if (leaf == std::string(pair.first) + ".log") {
                candidate = "Logs/" + std::string(pair.second) + (pair.first == "sfxruntime" ? ".log" : ".LOG");
            } else if (leaf.substr(0, pair.first.size() + 1) == std::string(pair.first) + "-") {
                std::string suffix(leaf.substr(pair.first.size()));
                for (auto& c : suffix) if (c >= 'a' && c <= 'z') c = static_cast<char>(c - ('a' - 'A'));
                candidate = "Logs/" + std::string(pair.second) + suffix;
            }
            if (!candidate.empty() && is_linux_journal_snapshot_member(candidate)) return candidate;
        }
    }
    // The stock external mailer has global inbound/work folders and positional
    // code-selected network slots. Preserve that code and payload suffixes.
    const auto external_prefix = native ? std::string_view("FTN/External/") : std::string_view("ftn/external/");
    if (input.substr(0, external_prefix.size()) == external_prefix) {
        const auto tail = input.substr(external_prefix.size());
        const auto slash = tail.find('/');
        if (slash != std::string_view::npos && slash != 0) {
            const auto role = tail.substr(slash + 1);
            constexpr std::pair<std::string_view, std::string_view> slots[]{
                {"Inbound/Secure", "inbound/secure"}, {"Inbound/Nonsecure", "inbound/nonsecure"},
                {"Inbound/Links", "inbound/links"}, {"Inbound", "inbound"},
                {"Outbound", "outbound"}, {"Copies", "copies"}, {"Journal", "journal"},
                {"Records", "records"}, {"Consumed", "consumed"}, {"Temp", "temp"}
            };
            for (const auto& slot : slots) {
                const auto from = native ? slot.first : slot.second;
                const auto to = native ? slot.second : slot.first;
                if (role == from || (role.size() > from.size() &&
                    role.substr(0, from.size()) == from && role[from.size()] == '/'))
                    return std::string(native ? "ftn/external/" : "FTN/External/") +
                        std::string(tail.substr(0, slash)) + "/" + std::string(to) + std::string(role.substr(from.size()));
            }
            const auto zoned = native ? std::string_view("Outbound.") : std::string_view("outbound.");
            if (role.substr(0, zoned.size()) == zoned) {
                const auto zone_end = role.find('/', zoned.size());
                const auto zone = role.substr(zoned.size(),
                    zone_end == std::string_view::npos ? zone_end : zone_end - zoned.size());
                if ((zone.size() == 3 || zone.size() == 4) &&
                    zone.find_first_not_of("0123456789abcdef") == std::string_view::npos)
                    return std::string(native ? "ftn/external/" : "FTN/External/") +
                        std::string(tail.substr(0, slash)) + (native ? "/outbound." : "/Outbound.") +
                        std::string(zone) + (zone_end == std::string_view::npos ? "" : std::string(role.substr(zone_end)));
            }
            if (role == (native ? "Profile" : "profile") || role == (native ? "Profile.tmp" : "profile.tmp"))
                return std::string(native ? "ftn/external/" : "FTN/External/") +
                    std::string(tail.substr(0, slash)) + (native ? "/profile" : "/Profile") +
                    (role.size() > 7 ? ".tmp" : "");
        }
    }
    const auto runs = native ? std::string_view("FTN/External/Runs/") : std::string_view("ftn/external/runs/");
    if (input.substr(0, runs.size()) == runs) {
        const auto rest = input.substr(runs.size());
        const auto slash = rest.find('/');
        const auto leaf = slash == std::string_view::npos ? std::string_view{} : rest.substr(slash + 1);
        if (slash != std::string_view::npos && slash != 0 &&
            (leaf == (native ? "Manifest" : "manifest") || leaf == (native ? "Manifest.tmp" : "manifest.tmp")))
            return std::string(native ? "ftn/external/runs/" : "FTN/External/Runs/") +
                std::string(rest.substr(0, slash)) + (native ? "/manifest" : "/Manifest") +
                (leaf.size() > 8 ? ".tmp" : "");
    }
    constexpr std::pair<std::string_view, std::string_view> directories[]{
        {"Data/FileIntake", "data/fileintake"}, {"Data/FileRemoved", "data/fileremoved"},
        {"Data/ClamAV", "data/clamav"},
        {"Files/General", "files/general"}, {"Files/Uploads", "files/uploads"},
        {"Files/UploadToSysop", "files/uploadtosysop"},
        {"Backup/Upgrades", "backup/upgrades"}, {"Backup/Scheduled", "backup/scheduled"},
        {"Backup/Recovery", "backup/recovery"}, {"Backup/RestoreFiles", "backup/restorefiles"},
        {"FTN/External/Inbound/Secure", "ftn/external/inbound/secure"},
        {"FTN/External/Inbound/Nonsecure", "ftn/external/inbound/nonsecure"},
        {"FTN/External/Inbound/Links", "ftn/external/inbound/links"},
        {"FTN/External/Inbound", "ftn/external/inbound"},
        {"FTN/External/Temp", "ftn/external/temp"}, {"FTN/External/Runs", "ftn/external/runs"},
        {"FTN/External/Records", "ftn/external/records"},
        {"FTN/External/Consumed", "ftn/external/consumed"}, {"FTN/External/Tics", "ftn/external/tics"},
        {"FTN/External", "ftn/external"},
        {"FTN/Inbound", "ftn/inbound"}, {"FTN/Outbound", "ftn/outbound"},
        {"FTN/Partial", "ftn/partial"}, {"FTN/Nonsecure", "ftn/nonsecure"},
        {"FTN/Bad", "ftn/bad"}, {"FTN/Processed", "ftn/processed"}, {"FTN/Conflicts", "ftn/conflicts"},
        {"Config", "config"}, {"Data", "data"}, {"Display", "display"},
        {"Doors", "doors"}, {"Logs", "logs"}, {"BotGate", "botgate"},
        {"FTN", "ftn"}, {"Files", "files"}, {"FileContents", "filecontents"},
        {"Backup", "backup"}, {"Work", "work"}, {"Node", "node"},
        {"Management", "management"}, {"Games", "games"},
        {"Bin", "bin"}, {"Web", "web"}, {"Docs", "docs"}, {"Displays", "displays"}
    };
    for (const auto& pair : directories) {
        const auto from = native ? pair.first : pair.second;
        const auto to = native ? pair.second : pair.first;
        if (input == from || (input.size() > from.size() && input.substr(0, from.size()) == from && input[from.size()] == '/'))
            return std::string(to) + std::string(input.substr(from.size()));
    }
    return std::string(input);
}
} // namespace layout_detail

inline std::string physical_relative(std::string_view canonical, PhysicalLayout layout) {
    if (!valid_physical_layout(layout)) throw std::invalid_argument("invalid archive physical layout");
    return layout == PhysicalLayout::canonical ? std::string(canonical) : layout_detail::map(canonical, true);
}

inline std::string archive_relative(std::string_view native, PhysicalLayout layout) {
    if (!valid_physical_layout(layout)) throw std::invalid_argument("invalid archive physical layout");
    return layout == PhysicalLayout::canonical ? std::string(native) : layout_detail::map(native, false);
}

} // namespace sfx::archive
