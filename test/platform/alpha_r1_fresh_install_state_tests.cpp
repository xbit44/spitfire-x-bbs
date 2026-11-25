// Focused R1 correction fixture. Read-only inspection of a freshly installed
// disposable root proves that Setup's protected empty JOKER is metadata, not an
// initialized-BBS identity. It never creates, repairs or removes an object.
#include "sfx/platform/win32/alpha_runtime_platform.hpp"
#include "sfx/platform/win32/installation_security.hpp"
#include "sfx/platform/win32/storage_dacl_policy.hpp"
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string_view>

namespace win = sfx::platform::win32;
namespace install = sfx::platform::win32::installation;
namespace {
unsigned checks{};
void check(const bool value, const char* label) {
    ++checks;
    if (!value) throw std::runtime_error(label);
}
}

int wmain(int argc, wchar_t** argv) {
    try {
        check(argc == 2 || (argc == 3 && std::wstring_view(argv[2]) == L"--expect-persistent"),
            "fresh-state fixture requires one disposable root and optional --expect-persistent");
        const bool expect_persistent = argc == 3;
        check(!install::elevated(), "fresh-state fixture must remain ordinary");
        const auto root = install::canonical_root(argv[1]);
        check(root.parent_path() == L"C:\\" &&
            root.filename().native().rfind(L"xtest-sfx-v002-r1-fresh-", 0) == 0,
            "fresh-state fixture is restricted to its disposable family");
        const auto owner = install::operator_sid();
        install::verify_runtime_installation(root, owner);
        check(true, "fresh protected installation and receipt verify exactly");
        check(!std::filesystem::exists(root / L"Config" / L"SFX.conf"),
            "fresh Setup has not fabricated operational configuration");
        const auto joker = root / L"Config" / L"JOKER.DAT";
        check(std::filesystem::is_regular_file(joker) && std::filesystem::file_size(joker) == 0,
            "fresh Setup has one empty JOKER metadata file");
        win::WindowsStorageDaclPolicy policy(owner);
        check(policy.assess(joker, win::StorageDaclObjectKind::file).status.succeeded(),
            "fresh JOKER has the exact protected content policy");
        const auto state = win::alpha_installation_has_persistent_state(root, owner);
        check(state && state.value && *state.value == expect_persistent,
            expect_persistent
                ? "first-run storage still prevents a later fresh-setup classification"
                : "protected empty JOKER does not suppress genuine first-run setup");
        std::cout << (expect_persistent ? "R1 INTERRUPTED SETUP STATE PASS checks="
                                        : "R1 FRESH INSTALL STATE PASS checks=") << checks
            << " failed=0 skipped=0; read-only; no UAC or mutation\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "R1 FRESH INSTALL STATE FAIL checks=" << checks
            << " diagnostic=" << error.what() << '\n';
        return 1;
    }
}
