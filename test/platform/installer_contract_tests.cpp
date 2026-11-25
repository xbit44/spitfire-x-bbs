// I01/I03/I06: the production codec and root selector differ from the accepted
// synthetic receipt. Small read-only cases cover each newly exposed boundary.
// No key, directory, program, permission or installed SFX object is changed.
#include "sfx/platform/win32/installation_security.hpp"
#include <iostream>
#include <functional>
#include <conio.h>
#include <sddl.h>

namespace install = sfx::platform::win32::installation;
namespace {
unsigned checks = 0;
void check(bool value, const char* label) { ++checks; if (!value) throw std::runtime_error(label); }
void rejects(const std::function<void()>& action, const char* label) {
    bool refused = false; try { action(); } catch (const std::exception&) { refused = true; } check(refused, label);
}
void mutable_descriptor_contract() {
    const auto who = install::operator_sid();
    const auto operator_ace = L"(A;OICI;0x1301ff;;;" + who + L")";
    auto matches = [&](const std::wstring& text) {
        void* raw = nullptr;
        check(ConvertStringSecurityDescriptorToSecurityDescriptorW(text.c_str(),
            SDDL_REVISION_1, &raw, nullptr) != FALSE, "parse fixture descriptor");
        install::Descriptor descriptor(raw);
        return install::exact_mutable_descriptor(descriptor.get(), who);
    };
    const auto prefix = L"O:" + who + L"D:P";
    const std::wstring system = L"(A;OICI;FA;;;SY)", admins = L"(A;OICI;FA;;;BA)";
    check(matches(prefix + operator_ace + system + admins), "original mutable ACE order");
    check(matches(prefix + system + admins + operator_ace), "equivalent reordered allow ACEs");
    check(!matches(prefix + system + admins + operator_ace + L"(A;OICI;FR;;;WD)"), "extra principal refused");
    check(!matches(prefix + system + operator_ace + operator_ace), "duplicate principal refused");
    check(!matches(prefix + system + admins + L"(A;OICI;FA;;;" + who + L")"), "widened operator rights refused");
    check(!matches(prefix + system + admins + L"(A;OI;0x1301ff;;;" + who + L")"), "changed inheritance refused");
    check(!matches(prefix + system + admins + L"(D;OICI;0x1301ff;;;" + who + L")"), "deny substitution refused");
    check(!matches(L"O:SYD:P" + operator_ace + system + admins), "wrong owner refused");
    check(!matches(L"O:" + who + L"D:" + operator_ace + system + admins), "unprotected DACL refused");
}
void hold_disposable_runtime(const wchar_t* argument) {
    // I04's smallest real mid-replacement failure: an ordinary reader refuses
    // delete sharing for ONLY this disposable installation's Runtime program.
    // Setup, not this helper, owns every mutation. No permissions are changed.
    const auto root = install::canonical_root(argument);
    check(root.parent_path() == L"C:\\" &&
        root.filename().native().rfind(L"xtest-sfx-v002-installer-", 0) == 0,
        "read hold restricted to disposable installer targets");
    install::verify_runtime_installation(root, install::operator_sid());
    DWORD console_mode = 0;
    check(GetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), &console_mode) != FALSE,
        "bounded read holder needs its engineering console input");
    auto held = install::open_object(root / L"Bin" / L"SFXRuntime.exe", false,
        GENERIC_READ, FILE_SHARE_READ);
    std::cout << "READ HOLD READY: disposable Runtime only; no writes or permission changes; "
        "Enter releases; automatic expiry 300 seconds\n" << std::flush;
    const auto started = GetTickCount64();
    bool released = false;
    while (GetTickCount64() - started < 300000) {
        if (_kbhit() && _getch() == '\r') { released = true; break; }
        Sleep(50);
    }
    held.reset();
    std::cout << (released ? "READ HOLD RELEASED" : "READ HOLD EXPIRED")
        << ": handle closed; this is fault instrumentation, not a Setup test verdict\n";
}
void inspect_disposable_installation(const wchar_t* argument) {
    // I02: prior synthetic ACL tests cannot prove what actual Setup created.
    // Inspect only the explicitly designated disposable production-test family.
    // Access-mask requests are OPEN_EXISTING and are never followed by mutation.
    const auto root = install::canonical_root(argument);
    wchar_t label[64]{}, fs[32]{}; DWORD serial = 0;
    const bool alternate = root == L"R:\\SFX" &&
        GetVolumeInformationW(L"R:\\",label,64,&serial,nullptr,nullptr,fs,32) &&
        std::wstring(label)==L"SFX-ALT-0927" && std::wstring(fs)==L"NTFS" && serial==0x205e0bb2;
    check(root == L"C:\\xtest-sfx-section8b-followup-fault-r2-20261009" || root == L"C:\\xtest-sfx-section8b-followup-stock-r1-20261009" || root == L"C:\\xtest-sfx-section8b-followup-qualified-r2-20261009" || root == L"C:\\xtest-sfx-section8b-followup-probe-r1-20261009" || root == L"C:\\xtest-sfx-section8b-simple-qualified-r1-20261008" || root == L"C:\\xtest-sfx-section8b-simple-fault-r1-20261008" || root == L"C:\\xtest-sfx-section8b-qualified-r5-20261008" || alternate || (root.parent_path() == L"C:\\" &&
        root.filename().native().rfind(L"xtest-sfx-v002-installer-", 0) == 0),
        "inspection restricted to disposable installer targets");
    const auto who = install::operator_sid();
    install::verify_runtime_installation(root, who);
    check(true, "exact protected structure, programs, receipt and eight mutable roles");
    const auto receipt = install::read_receipt(root, who);
    check(receipt.committed, "authoritative generation is committed");
    for (const auto& path : {root, root / L"Bin"}) {
        for (DWORD access : {DWORD(WRITE_DAC), DWORD(DELETE), DWORD(FILE_DELETE_CHILD), DWORD(FILE_ADD_FILE)}) {
            install::Handle denied(CreateFileW(path.c_str(), access,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
            const auto error = GetLastError();
            check(denied.get() == INVALID_HANDLE_VALUE && error == ERROR_ACCESS_DENIED,
                "ordinary structural modification authority is denied");
        }
    }
    for (std::size_t index = 0; index != receipt.program_count; ++index) {
        const auto* name = install::programs[index];
        for (DWORD access : {DWORD(GENERIC_WRITE), DWORD(WRITE_DAC), DWORD(DELETE)}) {
            install::Handle denied(CreateFileW((root / L"Bin" / name).c_str(), access,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
            const auto error = GetLastError();
            check(denied.get() == INVALID_HANDLE_VALUE && error == ERROR_ACCESS_DENIED,
                "ordinary shipping-program modification authority is denied");
        }
    }
    for (REGSAM access : {REGSAM(KEY_SET_VALUE), REGSAM(KEY_CREATE_SUB_KEY), REGSAM(DELETE)}) {
        HKEY key = nullptr;
        const auto result = RegOpenKeyExW(HKEY_LOCAL_MACHINE,
            (L"SOFTWARE\\Spitfire-X BBS.Installation." + receipt.id).c_str(), REG_OPTION_OPEN_LINK,
            access | KEY_WOW64_64KEY, &key);
        if (key) RegCloseKey(key);
        check(result == ERROR_ACCESS_DENIED, "ordinary receipt modification authority is denied");
    }
    std::cout << "INSTALLER INSTALLED-STRUCTURE PASS checks=" << checks
        << " failed=0 skipped=0 generation=" << receipt.generation
        << "; read-only, no UAC, no mutations\n";
}
}
int wmain(int argc, wchar_t** argv) {
    try {
        mutable_descriptor_contract();
        check(!install::elevated(), "read-only suite must run without elevation");
        if (argc == 3 && std::wstring(argv[1]) == L"--inspect-disposable-installation") {
            inspect_disposable_installation(argv[2]); return 0;
        }
        if (argc == 3 && std::wstring(argv[1]) == L"--hold-disposable-runtime") {
            hold_disposable_runtime(argv[2]); return 0;
        }
        check(argc == 1, "unexpected contract-test arguments");
        check(install::canonical_root(L"C:\\SFX") == L"C:\\SFX", "default canonical root");
        check(install::canonical_root(L"C:/SFX Custom") == L"C:\\SFX Custom", "custom-root separators");
        rejects([] { (void)install::canonical_root(L"C:\\"); }, "volume root refuses");
        rejects([] { (void)install::canonical_root(L"C:\\SFX\\.."); }, "parent traversal refuses");
        rejects([] { (void)install::canonical_root(L"C:\\SFX."); }, "trailing-dot alias refuses");
        rejects([] { (void)install::canonical_root(L"C:\\SFX:stream"); }, "ADS refuses");
        rejects([] { (void)install::canonical_root(L"C:\\AUX.txt"); }, "DOS device refuses");
        rejects([] { (void)install::canonical_root(L"\\\\server\\share\\SFX"); }, "UNC refuses");
        rejects([] { (void)install::canonical_root(L"\\\\?\\C:\\SFX"); }, "device namespace refuses");
        rejects([] { (void)install::canonical_root(L"C:\\Windows\\SFX"); }, "system subtree refuses");
        install::Receipt sample;
        sample.id = L"0123456789ABCDEF0123456789ABCDEF"; sample.root = L"C:\\SFX Custom";
        sample.owner = install::operator_sid(); sample.generation = 1; sample.committed = true;
        sample.volume = 42; sample.root_id = 17; sample.bin_id = 18; sample.program_ids = {19, 20};
        sample.program_hashes = {std::string(64, 'A'), std::string(64, 'B')};
        const auto bytes = install::encode_receipt(sample);
        check(bytes.size() < install::maximum_receipt_bytes, "bounded actual record");
        check(install::encode_receipt(install::decode_receipt(bytes)) == bytes, "committed exact round trip");
        auto current = sample;
        current.program_count = install::current_program_count;
        for(std::size_t n=0;n<install::current_program_count;++n){current.program_ids[n]=19+n;current.program_hashes[n]=std::string(64,static_cast<char>('A'+n%6));}
        const auto current_bytes = install::encode_receipt(current);
        const auto current_round_trip = install::decode_receipt(current_bytes);
        check(current_round_trip.program_count == install::current_program_count &&
                  current_round_trip.program_ids[5] == 24 &&
                  install::encode_receipt(current_round_trip) == current_bytes,
              "current ten-program receipt exact round trip");
        auto configuration_generation = current;
        configuration_generation.program_count = install::configuration_program_count;
        const auto configuration_bytes = install::encode_receipt(configuration_generation);
        check(install::decode_receipt(configuration_bytes).program_count ==
                  install::configuration_program_count &&
                  install::encode_receipt(install::decode_receipt(configuration_bytes)) ==
                  configuration_bytes,
              "accepted five-program receipt remains exact for matched update");
        auto archive_generation = current;
        archive_generation.program_count = install::archive_program_count;
        const auto archive_bytes = install::encode_receipt(archive_generation);
        check(install::decode_receipt(archive_bytes).program_count ==
                install::archive_program_count,
              "accepted four-program receipt remains readable for matched update");
        check(install::decode_receipt(bytes).program_count == install::legacy_program_count,
              "accepted two-program receipt remains readable for one upgrade");
        auto incomplete = sample; incomplete.committed = false;
        check(!install::decode_receipt(install::encode_receipt(incomplete)).committed, "incomplete never becomes committed");
        auto corrupt = bytes; corrupt.push_back('X');
        rejects([&] { (void)install::decode_receipt(corrupt); }, "trailing junk refuses");
        corrupt = bytes; corrupt[0] = 'X';
        rejects([&] { (void)install::decode_receipt(corrupt); }, "foreign policy refuses");
        corrupt = bytes; corrupt[4] = 0;
        rejects([&] { (void)install::decode_receipt(corrupt); }, "embedded NUL refuses");
        corrupt.assign(install::maximum_receipt_bytes + 1, '\n');
        rejects([&] { (void)install::decode_receipt(corrupt); }, "oversized refuses");
        auto invalid = sample; invalid.generation = 0;
        rejects([&] { (void)install::encode_receipt(invalid); }, "zero generation refuses");
        invalid = sample; invalid.program_hashes[0] = std::string(64, 'g');
        rejects([&] { (void)install::encode_receipt(invalid); }, "malformed hash refuses");
        invalid = sample; invalid.root_id = invalid.bin_id;
        rejects([&] { (void)install::encode_receipt(invalid); }, "inconsistent identity refuses");
        invalid = sample; invalid.owner = L"S-1-5-32-544";
        rejects([&] { (void)install::encode_receipt(invalid); }, "shared administrator identity refuses");
        check(install::sha256({'a','b','c'}) ==
            "BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD", "production SHA256 known answer");
        auto token = install::process_token();
        const auto anchors = install::protect_ancestors(L"C:\\SFX", token.get());
        check(anchors.size() == 1, "default volume ancestor read-only qualification");
        std::cout << "INSTALLER CONTRACT PASS checks=" << checks << " failed=0 skipped=0; read-only, no UAC\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "INSTALLER CONTRACT FAIL checks=" << checks << " diagnostic=" << e.what() << '\n'; return 1;
    }
}
