// Explicit two-phase owner qualification. Seed is elevated only to reproduce
// Windows 7's Administrators initial owner; verify uses the ordinary token.
#include "sfx/platform/win32/alpha_runtime_platform.hpp"
#include "sfx/platform/win32/storage_dacl_policy.hpp"
#include <aclapi.h>
#include <sddl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace win = sfx::platform::win32;

void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(std::string(message) + " (Win32 " + std::to_string(GetLastError()) + ")");
}

bool owner_is(const std::filesystem::path& path, const std::wstring& expected) {
    PSID sid = nullptr;
    require(ConvertStringSidToSidW(expected.c_str(), &sid) != FALSE, "parse expected test owner");
    PSID actual = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    auto native = path.native();
    const auto result = GetNamedSecurityInfoW(native.data(), SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION,
        &actual, nullptr, nullptr, nullptr, &descriptor);
    const bool matches = result == ERROR_SUCCESS && actual && EqualSid(actual, sid);
    if (descriptor) LocalFree(descriptor);
    LocalFree(sid);
    return matches;
}

bool allowed_root(const std::filesystem::path& root) {
    if (root != root.lexically_normal() || !root.is_absolute()) return false;
    const auto scratch = std::filesystem::path(SFX_BIN_OWNER_TEST_SCRATCH).lexically_normal();
    const bool local = root.parent_path() == scratch && root.filename().wstring().find(L"bin-owner-qa-") == 0;
    const bool owner = root == std::filesystem::path(L"C:\\xtest-sfx-provisioning\\bin-owner-qa");
    if (!local && !owner) return false;
    auto candidate = root;
    while (!candidate.empty()) {
        const auto attributes = GetFileAttributesW(candidate.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
        const auto parent = candidate.parent_path();
        if (parent == candidate) break;
        candidate = parent;
    }
    return true;
}

void seed(const std::filesystem::path& base, const std::wstring& user) {
    // Never reuse or overwrite an existing fixture. No system token defaults,
    // privileges, account policy or installed SFX objects are changed.
    win::WindowsStorageDaclPolicy base_policy(user);
    require(base_policy.apply(base, win::StorageDaclObjectKind::directory).succeeded(),
        "protect dedicated fixture/report root for ordinary verification");
    for (const auto* name : {L"repair", L"denied"}) {
        const auto root = base / name;
        require(!std::filesystem::exists(root), "seed target must be absent");
        require(CreateDirectoryW(root.c_str(), nullptr) != FALSE, "create fixture installation root");
        win::WindowsStorageDaclPolicy policy(user);
        require(policy.apply(root, win::StorageDaclObjectKind::directory).succeeded(), "protect fixture installation root");
        const auto bin = root / L"Bin";
        require(CreateDirectoryW(bin.c_str(), nullptr) != FALSE, "create initially empty fixture Bin");
        PSID administrators = nullptr;
        require(ConvertStringSidToSidW(L"S-1-5-32-544", &administrators) != FALSE, "parse Administrators SID");
        auto native = bin.native();
        const auto result = SetNamedSecurityInfoW(native.data(), SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION,
            administrators, nullptr, nullptr, nullptr);
        LocalFree(administrators);
        require(result == ERROR_SUCCESS, "seed requires administrator-approved process to assign Administrators owner");
        if (std::wstring(name) == L"denied") {
            const auto sddl = L"D:P(D;;WO;;;" + user + L")(A;OICI;GA;;;" + user +
                L")(A;OICI;GA;;;SY)(A;OICI;GA;;;BA)";
            PSECURITY_DESCRIPTOR descriptor = nullptr;
            require(ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1,
                &descriptor, nullptr) != FALSE, "build denied-owner-correction fixture");
            PACL acl = nullptr;
            BOOL present = FALSE, defaulted = FALSE;
            const bool parsed = GetSecurityDescriptorDacl(descriptor, &present, &acl, &defaulted) && present && acl;
            const auto applied = parsed ? SetNamedSecurityInfoW(native.data(), SE_FILE_OBJECT,
                DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, nullptr, nullptr, acl, nullptr) : ERROR_INVALID_ACL;
            LocalFree(descriptor);
            require(applied == ERROR_SUCCESS, "apply denied-owner-correction fixture");
        }
        require(owner_is(bin, L"S-1-5-32-544") && std::filesystem::is_empty(bin),
            "verify real Administrators-owned empty Bin before production code runs");
        require(!std::filesystem::exists(root / L"Config") && !std::filesystem::exists(root / L"Data"),
            "seed must reproduce exact pre-Config/Data provisioning stage");
    }
}

void verify(const std::filesystem::path& base, const std::wstring& user) {
    for (const auto* name : {L"repair", L"denied"}) {
        const auto root = base / name;
        require(owner_is(root, user) && owner_is(root / L"Bin", L"S-1-5-32-544"),
            "qualification requires designated-user root and real Administrators-owned Bin");
        const auto result = win::provision_alpha_installation_tree(root, user);
        if (std::wstring(name) == L"denied") {
            require(!result && result.operation == "apply Alpha Bin owner and read-execute DACL",
                "failed owner correction preserves its exact diagnostic");
            require(!std::filesystem::exists(root / L"Config") && !std::filesystem::exists(root / L"Data"),
                "failed correction stops before Config/Data creation");
            continue;
        }
        require(result.succeeded(), "production provisioner repairs Windows 7 initial owner");
        require(owner_is(root / L"Bin", user) && win::assess_alpha_bin_directory(root, user).succeeded(),
            "final Bin owner and protected read-execute DACL match exactly");
        win::WindowsStorageDaclPolicy policy(user);
        for (const auto* leaf : {L"Config", L"Data", L"Display", L"Files", L"Doors",
                                 L"Backup", L"Logs", L"Work"})
            require(policy.assess(root / leaf, win::StorageDaclObjectKind::directory).status.succeeded(),
                "remaining directories exist with exact designated-user protected policy");
        require(win::provision_alpha_installation_tree(root, user).succeeded(), "already-correct tree remains restart-safe");
        // Verification is not permissive: reject a broader but still protected
        // storage DACL in Bin, then prove provisioning restores special RX policy.
        require(policy.apply(root / L"Bin", win::StorageDaclObjectKind::directory).succeeded(), "seed broader Bin DACL");
        require(!win::assess_alpha_bin_directory(root, user).succeeded(), "exact Bin verification rejects mutable-directory permissions");
        require(win::provision_alpha_installation_tree(root, user).succeeded() &&
            win::assess_alpha_bin_directory(root, user).succeeded(), "restore and verify special Bin RX policy");
    }
}

int main(int argc, char** argv) {
    std::filesystem::path base;
    std::string mode;
    try {
        require(argc == 3, "usage: seed|verify approved-fixture-root");
        mode = argv[1];
        require(mode == "seed" || mode == "verify", "explicit qualification mode required");
        base = std::filesystem::absolute(argv[2]).lexically_normal();
        require(allowed_root(base) && std::filesystem::is_directory(base), "fixture root outside approved scratch scope");
        const auto user = win::current_process_user_sid_string();
        require(!user.empty(), "resolve designated installation user");
        if (mode == "seed") seed(base, user); else verify(base, user);
        const std::string message = "SFX Windows 7 initial Administrators Bin owner " + mode + " PASS\n";
        std::ofstream(base / (mode + "-result.txt")) << message;
        std::cout << message;
        return 0;
    } catch (const std::exception& error) {
        const std::string message = std::string("SFX Bin owner qualification FAIL: ") + error.what() + "\n";
        if (!base.empty() && allowed_root(base) && (mode == "seed" || mode == "verify"))
            std::ofstream(base / (mode + "-result.txt")) << message;
        std::cerr << message;
        return 1;
    }
}
