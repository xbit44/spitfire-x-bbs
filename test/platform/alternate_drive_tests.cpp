// Explicit disposable-volume test. Never shipped or run on an owner drive.
// Including the implementation permits descriptor-level tests without adding a
// production API that accepts a caller-supplied "this is a volume root" flag.
#include "../../src/platform/win32/src/installation_security.cpp"
#include <winioctl.h>
#include <iostream>
#include <functional>
namespace install = sfx::platform::win32::installation;
namespace {
unsigned checks = 0;
void check(bool ok, const char* text) { ++checks; if (!ok) throw std::runtime_error(text); }
bool refused(const std::function<void()>& operation) {
    try { operation(); return false; } catch (const std::exception& e) {
        std::cout << "REFUSED " << e.what() << '\n'; return true;
    }
}
std::vector<BYTE> junction(const std::wstring& target) {
    const std::wstring substitute = L"\\??\\" + target;
    const WORD sub_bytes = static_cast<WORD>(substitute.size() * sizeof(wchar_t));
    const WORD print_bytes = static_cast<WORD>(target.size() * sizeof(wchar_t));
    std::vector<BYTE> bytes(16 + sub_bytes + 2 + print_bytes + 2, 0);
    const DWORD tag = IO_REPARSE_TAG_MOUNT_POINT;
    const WORD data_bytes = static_cast<WORD>(bytes.size() - 8), print_offset = sub_bytes + 2;
    std::memcpy(bytes.data(), &tag, 4); std::memcpy(bytes.data()+4, &data_bytes, 2);
    std::memcpy(bytes.data()+10, &sub_bytes, 2); std::memcpy(bytes.data()+12, &print_offset, 2);
    std::memcpy(bytes.data()+14, &print_bytes, 2);
    std::memcpy(bytes.data()+16, substitute.data(), sub_bytes);
    std::memcpy(bytes.data()+16+print_offset, target.data(), print_bytes);
    return bytes;
}
bool set_junction(const std::wstring& path, std::vector<BYTE>& data) {
    install::Handle handle(CreateFileW(path.c_str(), FILE_WRITE_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (handle.get() == INVALID_HANDLE_VALUE) { std::cout << "OPEN refusal " << GetLastError() << '\n'; return false; }
    DWORD returned = 0;
    const bool ok = DeviceIoControl(handle.get(), FSCTL_SET_REPARSE_POINT, data.data(),
        static_cast<DWORD>(data.size()), nullptr, 0, &returned, nullptr) != FALSE;
    std::cout << "REPARSE " << (ok ? 0 : GetLastError()) << '\n';
    if (ok) {
        // Restore only the reparse metadata on this same verified test handle.
        std::array<BYTE,8> remove{}; const DWORD tag = IO_REPARSE_TAG_MOUNT_POINT;
        std::memcpy(remove.data(), &tag, 4);
        check(DeviceIoControl(handle.get(), FSCTL_DELETE_REPARSE_POINT, remove.data(),
            static_cast<DWORD>(remove.size()), nullptr, 0, &returned, nullptr) != FALSE, "remove test junction metadata");
    }
    return ok;
}
void volume_probe(bool expect_allowed) {
    check(!install::elevated(), "test must use ordinary operator token");
    wchar_t label[64]{}, fs[32]{}; DWORD serial = 0;
    check(GetVolumeInformationW(L"R:\\", label,64,&serial,nullptr,nullptr,fs,32) &&
        std::wstring(label)==L"SFX-ALT-0927" && std::wstring(fs)==L"NTFS" && serial==0x205e0bb2,
        "exact disposable VHD label/filesystem/serial required");
    auto token = install::process_token();
    auto root = install::open_object(L"R:\\",true);
    auto descriptor = install::file_security(root.get()); install::Authorization auth(token.get());
    check(auth.grants(descriptor.get(),FILE_ADD_FILE) && auth.grants(descriptor.get(),DELETE) &&
        auth.grants(descriptor.get(),FILE_WRITE_ATTRIBUTES), "owner Modify layout reproduced");
    check(!auth.grants(descriptor.get(),FILE_DELETE_CHILD) && !auth.grants(descriptor.get(),WRITE_DAC) &&
        !auth.grants(descriptor.get(),WRITE_OWNER), "Modify does not grant child deletion or ACL ownership control");
    const bool blocked = refused([&] { (void)install::protect_ancestors(L"R:\\SFX",token.get()); });
    check(blocked != expect_allowed, "production ancestor decision matches expected baseline/correction");
    // No recursive deletion, real-drive mutation, or elevated operation. The
    // root handle and serial bind these attempts to this disposable VHD only.
    root.reset();
    const auto fixture = std::wstring(L"R:\\probe-") + install::unique_id();
    check(CreateDirectoryW(fixture.c_str(),nullptr)!=FALSE,"create unique harmless sibling");
    auto data = junction(fixture);
    const auto empty = fixture + L"-empty";
    check(CreateDirectoryW(empty.c_str(),nullptr)!=FALSE,"create empty junction positive control");
    check(set_junction(empty,data),"junction buffer succeeds on ordinary empty directory");
    check(!set_junction(L"R:\\",data),"real NTFS volume root refuses redirection");
    install::Handle deletion(CreateFileW(L"R:\\", DELETE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
    if (deletion.get()!=INVALID_HANDLE_VALUE) {
        FILE_DISPOSITION_INFO info{TRUE};
        const bool deleted = SetFileInformationByHandle(deletion.get(),FileDispositionInfo,&info,sizeof(info))!=FALSE;
        if (deleted) { info.DeleteFile=FALSE; (void)SetFileInformationByHandle(deletion.get(),FileDispositionInfo,&info,sizeof(info)); }
        check(!deleted,"volume root cannot be marked for deletion");
    } else check(GetLastError()==ERROR_ACCESS_DENIED,"NTFS refuses DELETE open on volume root despite ACL bit");
    check(GetVolumeInformationW(L"R:\\",label,64,&serial,nullptr,nullptr,fs,32) && serial==0x205e0bb2,
        "volume identity unchanged after negative probes");
    check(RemoveDirectoryW(empty.c_str()) && RemoveDirectoryW(fixture.c_str()),"remove only two unique empty probe directories");
}
void descriptor_tests() {
    auto token = install::process_token(); install::Authorization auth(token.get());
    const std::wstring base = L"O:BAG:BAD:P(A;;FA;;;SY)(A;;FA;;;BA)";
    const auto accepts = [&](const std::wstring& text, install::AnchorKind kind) {
        auto descriptor = install::from_sddl(text);
        return !refused([&] { install::verify_anchor(descriptor.get(),auth,kind); });
    };
    using Kind = install::AnchorKind;
    check(accepts(base+L"(A;;0x1301bf;;;AU)",Kind::ntfs_volume_root),"reported root Modify allowed");
    check(!accepts(base+L"(A;;0x1301bf;;;AU)",Kind::directory),"nested Modify parent still refused");
    check(!accepts(base+L"(A;;0x1301bf;;;AU)",Kind::registry),"HKLM guard not relaxed");
    check(accepts(base+L"(A;;FRFX;;;BU)(A;;0x4;;;AU)(A;OICIIO;FA;;;AU)",Kind::ntfs_volume_root),
        "read/add-folder and inherit-only grants do not govern protected child");
    for (const auto sid : {L"AU",L"BU",L"WD",L"S-1-5-21-111-222-333-1234"}) {
        for (const auto right : {L"0x40",L"WD",L"WO",L"FA"}) {
            check(!accepts(base+L"(A;;"+right+L";;;"+sid+L")",Kind::ntfs_volume_root),
                "child deletion/ACL/owner/full control refused for every untrusted principal");
        }
    }
    check(!accepts(L"O:"+install::operator_sid()+L"G:BAD:P(A;;FRFX;;;AU)",Kind::ntfs_volume_root),
        "untrusted root owner refused");
    check(!accepts(L"O:BAG:BAD:(D;;0x40;;;AU)(A;;FA;;;AU)",Kind::ntfs_volume_root),
        "mixed deny/unsafe allow is not guessed safe");
    check(accepts(L"O:BAG:BAD:(D;;0x2;;;AU)(A;;0x1301bf;;;AU)",Kind::ntfs_volume_root),
        "deny plus otherwise safe Modify accepted without treating every deny as unsafe");
    check(!accepts(L"O:BAG:BAD:NO_ACCESS_CONTROL",Kind::ntfs_volume_root),"null DACL refuses");
}
}
int wmain(int argc, wchar_t** argv) {
    try {
        check(argc==2,"--before or --after required");
        const std::wstring mode(argv[1]); check(mode==L"--before" || mode==L"--after","bounded probe mode");
        volume_probe(mode==L"--after");
        if (mode==L"--after") descriptor_tests();
        std::cout << "ALTERNATE DRIVE PASS checks=" << checks << " failed=0 skipped=0\n"; return 0;
    } catch(const std::exception& error) {
        std::cerr << "ALTERNATE DRIVE FAIL checks=" << checks << " " << error.what() << '\n'; return 1;
    }
}
