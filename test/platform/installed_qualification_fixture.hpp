#pragma once

#include "sfx/platform/win32/alpha_runtime_platform.hpp"
#include "sfx/platform/win32/installation_security.hpp"
#include "sfx/platform/win32/storage_dacl_policy.hpp"
#include <iostream>
#include <map>
#include <set>
#include <sstream>

namespace sfx::test {

// One genuine Setup-owned installation, reserved exclusively for this round.
// Only mutable children are rotated into retained per-case evidence. Never
// fabricate receipts, loosen structural permissions, or recursively delete roots.
class InstalledQualificationFixture final {
public:
    explicit InstalledQualificationFixture(std::filesystem::path evidence)
        : evidence_(std::filesystem::absolute(evidence)), owner_(platform::win32::installation::operator_sid()) {
        namespace i = platform::win32::installation;
        // The driver must run normal shipping Setup, verify its payload, and
        // supply the exact resulting receipt/seed manifest and its SHA256.
        // There is deliberately no fallback to a historical installation.
        const auto manifest_path = std::filesystem::path(environment(L"SFX_QUALIFICATION_MANIFEST"));
        const auto manifest_hash = environment(L"SFX_QUALIFICATION_MANIFEST_SHA256");
        const auto build = std::filesystem::weakly_canonical(SFX_QUALIFICATION_BUILD_ROOT);
        require_inside(manifest_path, build);
        reject_redirect(manifest_path);
        const auto bytes = i::read_file(manifest_path, 65536);
        const auto digest = i::sha256(bytes);
        i::require(std::wstring(digest.begin(), digest.end()) == manifest_hash,
            "exact reviewed Setup fixture manifest hash required");
        std::istringstream manifest(std::string(bytes.begin(), bytes.end()));
        std::string line;
        i::require(bool(std::getline(manifest, line)) && line == "SFX-QUALIFICATION-1", "fixture manifest format");
        i::require(bool(std::getline(manifest, line)), "fixture root missing");
        root_ = std::filesystem::path(line);
        i::require(root_ == std::filesystem::path(L"C:/Program Files/SFX-Foundation-Test-20261107-r1") ||
            root_ == std::filesystem::path(L"C:/Program Files/SFX-Linux-Regression-Test-20261110-r1"),
            "only the explicitly owned qualification fixture is authorized");
        root_ = i::canonical_root(root_); // Keep the verified native spelling for final-path guards.
        i::require(bool(std::getline(manifest, line)) && line.size() == 32 &&
            line.find_first_not_of("0123456789ABCDEF") == std::string::npos, "exact fixture receipt ID required");
        receipt_id_ = std::wstring(line.begin(), line.end());
        i::require(bool(std::getline(manifest, receipt_hash_)) && receipt_hash_.size() == 64,
            "exact committed receipt hash required");
        while (std::getline(manifest, line)) {
            const auto split = line.find('|');
            i::require(split != std::string::npos && split > 0, "bounded seed manifest row required");
            const auto name = line.substr(0, split), hash = line.substr(split + 1);
            i::require(name.find_first_of("\\:\r\n") == std::string::npos && name.find("..") == std::string::npos &&
                name.front() != '/' && hash.size() == 64 && hash.find_first_not_of("0123456789ABCDEF") == std::string::npos,
                "relative exact seed identity required");
            const auto path = std::filesystem::path(name).make_preferred();
            i::require(baseline_.emplace(path, hash).second, "duplicate seed manifest path refused");
            for (auto parent = path.parent_path(); !parent.empty(); parent = parent.parent_path())
                baseline_directories_.insert(parent);
        }
        i::require(baseline_.size() == 50 && baseline_.count(L"Config/JOKER.DAT") == 1 &&
            baseline_.count(L"Config/otherbbs.txt") == 1 && baseline_.count(L"Display/bg-prelog.ans") == 1,
            "current Setup requires twenty-five displays, twenty-three starter files and two empty configuration files");
        for (const auto* relative : {L"Files/General", L"Files/Uploads", L"Files/UploadToSysop",
                L"Data/FileIntake", L"Data/FileRemoved", L"Backup/Upgrades", L"Backup/Scheduled"}) {
            baseline_directories_.insert(relative);
            i::require(std::filesystem::is_directory(root_ / relative), "current Setup seed directory is missing");
        }
        const auto lock_name = L"Local\\SFX-Qualification-" + receipt_id_;
        qualification_lock_.reset(CreateMutexW(nullptr, FALSE, lock_name.c_str()));
        i::require(qualification_lock_ && GetLastError() != ERROR_ALREADY_EXISTS,
            "another qualification suite owns this disposable fixture");
        evidence_ = std::filesystem::weakly_canonical(evidence_);
        require_inside(evidence_, build);
        i::require(!std::filesystem::exists(evidence_), "fixture evidence must be new");
        verify();
        i::require(!std::filesystem::exists(root_ / L"Data" / L"SFX.DB"),
            "qualification fixture must begin without a BBS database");
        std::set<std::filesystem::path> observed;
        for (const auto* leaf : i::mutable_directories_for(i::read_receipt(root_, owner_))) {
            for (const auto& entry : std::filesystem::recursive_directory_iterator(root_ / leaf)) {
                reject_redirect(entry.path());
                const auto relative = entry.path().lexically_relative(root_);
                if (entry.is_directory()) {
                    i::require(baseline_directories_.count(relative) == 1, "unexpected fresh fixture directory");
                } else {
                    const auto seed = baseline_.find(relative);
                    i::require(seed != baseline_.end() && i::file_sha256(entry.path()) == seed->second,
                        "unexpected or changed fresh Setup seed; retain and refuse");
                    observed.insert(relative);
                }
            }
        }
        i::require(observed.size() == baseline_.size(), "every exact Setup seed is present");
        std::filesystem::create_directories(evidence_);
    }
    ~InstalledQualificationFixture() {
        if (!finished_) {
            try { finish(); } catch (const std::exception& e) {
                std::cerr << "FIXTURE RECOVERY REQUIRED; retained state at fixed test root: " << e.what() << '\n';
            }
        }
    }
    const std::filesystem::path& next(const std::string& name) {
        rotate(name);
        return root_;
    }
    void finish() {
        rotate("stopped-final");
        finished_ = true;
        std::cout << "PASS preserved Setup receipt/programs and fresh stopped fixture; evidence " << evidence_ << '\n';
    }
private:
    void verify() const {
        namespace i = platform::win32::installation;
        i::verify_runtime_installation(root_, owner_);
        const auto r = i::read_receipt(root_, owner_);
        i::require(r.id == receipt_id_ && i::sha256(i::encode_receipt(r)) == receipt_hash_ &&
            r.generation == 1 && r.committed &&
            r.program_count == i::current_program_count, "exact disposable qualification receipt required");
    }
    static std::wstring environment(const wchar_t* name) {
        wchar_t value[32768]{};
        const DWORD size = GetEnvironmentVariableW(name, value, 32768);
        platform::win32::installation::require(size > 0 && size < 32768, "explicit fixture manifest environment required");
        return {value, size};
    }
    static void require_inside(const std::filesystem::path& path, const std::filesystem::path& build) {
        const auto resolved = std::filesystem::weakly_canonical(path);
        const auto relative = resolved.lexically_relative(build);
        platform::win32::installation::require(path.is_absolute() && !relative.empty() &&
            *relative.begin() != L".." && resolved != build,
            "fixture manifest and evidence must remain inside this configured build");
        auto ancestor = build;
        reject_redirect(ancestor);
        for (const auto& part : relative) {
            ancestor /= part;
            if (std::filesystem::exists(ancestor)) reject_redirect(ancestor);
        }
    }
    static void reject_redirect(const std::filesystem::path& path) {
        const auto attrs = GetFileAttributesW(path.c_str());
        platform::win32::installation::require(attrs != INVALID_FILE_ATTRIBUTES &&
            !(attrs & FILE_ATTRIBUTE_REPARSE_POINT), "fixture evidence must not traverse a reparse point");
    }
    void rotate(const std::string& name) {
        namespace i = platform::win32::installation;
        i::require(!name.empty() && name.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789-") == std::string::npos,
            "bounded fixture case name required");
        verify();
        platform::win32::OfflineInstallationGuard guard;
        i::require(guard.acquire(root_, owner_).succeeded(), "fixture transition requires no Runtime/offline owner");
        const auto destination = evidence_ / (std::to_string(++sequence_) + "-" + name);
        i::require(!std::filesystem::exists(destination), "never overwrite earlier fixture evidence");
        const auto leaves = i::mutable_directories_for(i::read_receipt(root_, owner_));
        // Keep every shipped seed in place, including protected starter binaries.
        // Refuse changed seeds before moving any newly generated test state.
        for (const auto& [relative, hash] : baseline_)
            i::require(i::file_sha256(root_ / relative) == hash, "Setup seed changed; preserve fixture for diagnosis");
        for (const auto* leaf : leaves)
            for (const auto& entry : std::filesystem::recursive_directory_iterator(root_ / leaf))
                reject_redirect(entry.path());
        for (const auto* leaf : leaves) {
            std::filesystem::create_directories(destination / leaf);
            std::vector<std::filesystem::path> children;
            for (auto it = std::filesystem::recursive_directory_iterator(root_ / leaf);
                it != std::filesystem::recursive_directory_iterator{}; ++it) {
                const auto relative = it->path().lexically_relative(root_);
                if (baseline_.count(relative) || baseline_directories_.count(relative)) continue;
                children.push_back(it->path());
                if (it->is_directory()) it.disable_recursion_pending();
            }
            for (const auto& child : children) {
                const auto target = destination / child.lexically_relative(root_);
                std::filesystem::create_directories(target.parent_path());
                std::filesystem::rename(child, target);
            }
        }
        verify();
    }
    std::filesystem::path root_;
    std::filesystem::path evidence_;
    std::wstring owner_;
    platform::win32::installation::Handle qualification_lock_;
    std::wstring receipt_id_;
    std::string receipt_hash_;
    std::map<std::filesystem::path, std::string> baseline_;
    std::set<std::filesystem::path> baseline_directories_;
    unsigned sequence_{0};
    bool finished_{false};
};
} // namespace sfx::test
