#include "sfx/platform/win32/installation_security.hpp"
#include "sfx/platform/win32/portable_installation.hpp"
#include "sfx/platform/win32/storage_dacl_policy.hpp"
#include "sfx/platform/win32/service_file_owner.hpp"
#include "sfx/platform/win32/service_mode.hpp"
#include <aclapi.h>
#include <authz.h>
#include <bcrypt.h>
#include <sddl.h>
#include <algorithm>
#include <string_view>
#include <charconv>
#include <cstring>
#include <cwctype>
#include <chrono>
#include <limits>
#include <stdexcept>

namespace sfx::platform::win32::installation {
namespace {
constexpr DWORD directory_flags = FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT;
constexpr wchar_t registry_prefix[] = L"SOFTWARE\\Spitfire-X BBS.Installation.";
constexpr char policy_v1[] = "SFX-DESKTOP-INSTALL-1";
constexpr char policy_v2[] = "SFX-DESKTOP-INSTALL-2";
constexpr char policy_v3[] = "SFX-DESKTOP-INSTALL-3";
constexpr char policy_v4[] = "SFX-DESKTOP-INSTALL-4";
constexpr char policy_v5[] = "SFX-DESKTOP-INSTALL-5";
constexpr char policy_v6[] = "SFX-DESKTOP-INSTALL-6";
constexpr char policy_v7[] = "SFX-DESKTOP-INSTALL-7";
constexpr char policy_v8[] = "SFX-DESKTOP-INSTALL-8";
constexpr char policy_v9[] = "SFX-DESKTOP-INSTALL-9";
struct Key {
    HKEY value{};
    ~Key() { if (value) RegCloseKey(value); }
    Key() = default;
    Key(const Key&) = delete;
    Key& operator=(const Key&) = delete;
};
bool hex(const std::wstring& value, std::size_t size) {
    return value.size() == size && std::all_of(value.begin(), value.end(), [](wchar_t c) {
        return (c >= L'0' && c <= L'9') || (c >= L'A' && c <= L'F');
    });
}
std::wstring widen(const std::string& text) { return {text.begin(), text.end()}; }
std::string narrow(const std::wstring& text) {
    require(std::all_of(text.begin(), text.end(), [](wchar_t c) { return c >= 32 && c < 127; }),
        "installation_text_not_portable_ascii");
    std::string result;
    for (auto c : text) result += static_cast<char>(c);
    return result;
}
std::wstring sid_text(PSID sid) {
    wchar_t* raw = nullptr;
    require(sid && ConvertSidToStringSidW(sid, &raw), "installation_sid_unreadable");
    Descriptor release(raw); return raw;
}
bool trusted_sid(const std::wstring& sid) {
    return sid == L"S-1-5-18" || sid == L"S-1-5-32-544" ||
        sid == L"S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464";
}
Descriptor from_sddl(const std::wstring& text) {
    void* raw = nullptr;
    require(ConvertStringSecurityDescriptorToSecurityDescriptorW(text.c_str(), SDDL_REVISION_1,
        &raw, nullptr) != FALSE, "installation_descriptor_invalid");
    return Descriptor(raw);
}
bool same_descriptor(void* actual, void* expected, bool unordered_allow = false) {
    PSID owner = nullptr, want = nullptr;
    PACL acl = nullptr, expected_acl = nullptr;
    BOOL def = FALSE, present = FALSE;
    SECURITY_DESCRIPTOR_CONTROL control{}; DWORD revision = 0;
    if (!GetSecurityDescriptorOwner(actual, &owner, &def) ||
        !GetSecurityDescriptorOwner(expected, &want, &def) || !owner || !want || !EqualSid(owner, want) ||
        !GetSecurityDescriptorControl(actual, &control, &revision) || !(control & SE_DACL_PROTECTED) ||
        !GetSecurityDescriptorDacl(actual, &present, &acl, &def) || !present || !acl ||
        !GetSecurityDescriptorDacl(expected, &present, &expected_acl, &def) || !present || !expected_acl ||
        acl->AceCount != expected_acl->AceCount) return false;
    std::vector<bool> matched(expected_acl->AceCount, false);
    for (DWORD n = 0; n < acl->AceCount; ++n) {
        void *a = nullptr, *b = nullptr;
        if (!GetAce(acl, n, &a)) return false;
        const auto size = static_cast<ACE_HEADER*>(a)->AceSize;
        if (!unordered_allow) {
            if (!GetAce(expected_acl, n, &b) ||
                size != static_cast<ACE_HEADER*>(b)->AceSize || std::memcmp(a, b, size)) return false;
        } else {
            // Only the fixed all-allow mutable policy may ignore ordering.
            // Match each complete ACE exactly once: no extra/duplicate SID,
            // widened mask, deny entry or inherited-flag substitution is allowed.
            if (static_cast<ACE_HEADER*>(a)->AceType != ACCESS_ALLOWED_ACE_TYPE) return false;
            bool found = false;
            for (DWORD index = 0; index < expected_acl->AceCount; ++index) {
                if (!matched[index] && GetAce(expected_acl, index, &b) &&
                    size == static_cast<ACE_HEADER*>(b)->AceSize && !std::memcmp(a, b, size)) {
                    matched[index] = true; found = true; break;
                }
            }
            if (!found) return false;
        }
    }
    return true;
}
Descriptor file_security(HANDLE file) {
    void* raw = nullptr;
    require(GetSecurityInfo(file, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
        nullptr, nullptr, nullptr, nullptr, &raw) == ERROR_SUCCESS, "installation_security_read_failed");
    return Descriptor(raw);
}
struct Authorization {
    AUTHZ_RESOURCE_MANAGER_HANDLE manager{};
    AUTHZ_CLIENT_CONTEXT_HANDLE context{};
    explicit Authorization(HANDLE token) {
        require(AuthzInitializeResourceManager(AUTHZ_RM_FLAG_NO_AUDIT, nullptr, nullptr, nullptr,
            L"SFX installation validation", &manager) != FALSE, "installation_access_context_failed");
        LUID id{};
        if (!AuthzInitializeContextFromToken(0, token, manager, nullptr, id, nullptr, &context)) {
            AuthzFreeResourceManager(manager); manager = nullptr;
            require(false, "installation_token_context_failed");
        }
    }
    ~Authorization() { if (context) AuthzFreeContext(context); if (manager) AuthzFreeResourceManager(manager); }
    bool grants(void* sd, DWORD right) {
        AUTHZ_ACCESS_REQUEST request{}; request.DesiredAccess = right;
        DWORD granted = 0, error = 0, sacl = 0;
        AUTHZ_ACCESS_REPLY reply{}; reply.ResultListLength = 1;
        reply.GrantedAccessMask = &granted; reply.Error = &error; reply.SaclEvaluationResults = &sacl;
        require(AuthzAccessCheck(0, context, &request, nullptr, sd, nullptr, 0, &reply, nullptr) != FALSE,
            "installation_access_check_failed");
        return error == ERROR_SUCCESS && (granted & right) == right;
    }
};
enum class AnchorKind { registry, directory, ntfs_volume_root };
void verify_anchor(void* descriptor, Authorization* auth, AnchorKind kind) {
    const bool registry = kind == AnchorKind::registry;
    PSID owner = nullptr; PACL acl = nullptr; BOOL def = FALSE, present = FALSE;
    require(GetSecurityDescriptorOwner(descriptor, &owner, &def) &&
        GetSecurityDescriptorDacl(descriptor, &present, &acl, &def) && present && acl,
        "installation_ancestor_security_missing");
    require(trusted_sid(sid_text(owner)), "installation_ancestor_owner_unsafe");
    // A real NTFS volume root is not a replaceable parent directory. Ordinary
    // Modify there permits unrelated siblings/metadata, not modification of our
    // explicitly protected child. Keep child deletion and ACL/owner takeover
    // forbidden. Never apply this distinction to a nested directory or HKLM.
    const DWORD danger = WRITE_DAC | WRITE_OWNER | (registry
        ? DELETE | KEY_SET_VALUE | KEY_CREATE_SUB_KEY | KEY_CREATE_LINK
        : kind == AnchorKind::ntfs_volume_root ? FILE_DELETE_CHILD
        : DELETE | FILE_DELETE_CHILD | FILE_WRITE_DATA | FILE_WRITE_ATTRIBUTES | FILE_WRITE_EA);
    for (unsigned n = 0; n != 32; ++n) if (danger & (DWORD(1) << n))
        require(!auth||!auth->grants(descriptor, DWORD(1) << n), "installation_ancestor_writable");
    GENERIC_MAPPING mapping = registry ? GENERIC_MAPPING{KEY_READ, KEY_WRITE, KEY_EXECUTE, KEY_ALL_ACCESS}
        : GENERIC_MAPPING{FILE_GENERIC_READ, FILE_GENERIC_WRITE, FILE_GENERIC_EXECUTE, FILE_ALL_ACCESS};
    for (DWORD n = 0; n < acl->AceCount; ++n) {
        void* raw = nullptr; require(GetAce(acl, n, &raw) != FALSE, "installation_ancestor_ace_invalid");
        const auto* header = static_cast<ACE_HEADER*>(raw);
        if (header->AceFlags & INHERIT_ONLY_ACE) continue;
        require(header->AceType == ACCESS_ALLOWED_ACE_TYPE || header->AceType == ACCESS_DENIED_ACE_TYPE,
            "installation_ancestor_ace_unsupported");
        if (header->AceType != ACCESS_ALLOWED_ACE_TYPE) continue;
        auto* ace = static_cast<ACCESS_ALLOWED_ACE*>(raw); DWORD mask = ace->Mask;
        MapGenericMask(&mask, &mapping);
        // CREATOR OWNER in an effective registry parent entry does not name an arbitrary writer.
        require(!(mask & danger) || trusted_sid(sid_text(&ace->SidStart)) ||
            (registry && sid_text(&ace->SidStart) == L"S-1-3-0"), "installation_ancestor_grant_unsafe");
    }
}
Handle ordinary_token() {
    auto token = process_token();
    if (!elevated()) return token;
    TOKEN_LINKED_TOKEN linked{}; DWORD size = 0;
    require(GetTokenInformation(token.get(), TokenLinkedToken, &linked, sizeof(linked), &size) != FALSE,
        "installation_requires_split_token_uac");
    Handle result(linked.LinkedToken);
    require(token_sid(token.get()) == token_sid(result.get()), "installation_operator_mismatch");
    return result;
}
bool system_process() {auto token=process_token();return token_sid(token.get())==L"S-1-5-18";}
Handle ancestry_token() {
    // A headless System service has no linked interactive token. Its read-only
    // validation still enforces trusted ancestor ownership and rejects every
    // dangerous grant to an untrusted SID, followed by exact private receipts.
    if(system_process())return {};
    return ordinary_token();
}
Descriptor registry_security(HKEY key) {
    DWORD size = 0;
    constexpr auto info = OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION;
    require(RegGetKeySecurity(key, info, nullptr, &size) == ERROR_INSUFFICIENT_BUFFER && size <= 65536,
        "installation_receipt_security_size_invalid");
    Descriptor result(LocalAlloc(LMEM_FIXED, size));
    require(result && RegGetKeySecurity(key, info, result.get(), &size) == ERROR_SUCCESS,
        "installation_receipt_security_read_failed");
    return result;
}
void open_registry_parent(Key& parent) {
    require(RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE", REG_OPTION_OPEN_LINK,
        KEY_READ | KEY_WOW64_64KEY, &parent.value) == ERROR_SUCCESS, "installation_receipt_parent_unavailable");
    auto ordinary = ancestry_token();std::unique_ptr<Authorization> auth;
    if(ordinary)auth=std::make_unique<Authorization>(ordinary.get());
    auto sd = registry_security(parent.value); verify_anchor(sd.get(), auth.get(), AnchorKind::registry);
}
std::wstring receipt_name(const std::wstring& id) {
    require(hex(id, 32), "installation_id_invalid"); return std::wstring(registry_prefix) + id;
}
Descriptor receipt_descriptor(const std::wstring& owner) {
    return from_sddl(L"O:BAG:BAD:P(A;;KA;;;SY)(A;;KA;;;BA)(A;;KR;;;" + owner + L")");
}
void check_key(HKEY key, const std::wstring& owner) {
    auto actual = registry_security(key); auto expected = receipt_descriptor(owner);
    require(same_descriptor(actual.get(), expected.get()), "installation_receipt_security_invalid");
    DWORD children = 0, values = 0, type = 0, length = 0;
    require(RegQueryInfoKeyW(key, nullptr, nullptr, nullptr, &children, nullptr, nullptr,
        &values, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS && !children && values == 1,
        "installation_receipt_inventory_invalid");
    require(RegQueryValueExW(key, L"SymbolicLinkValue", nullptr, &type, nullptr, &length) == ERROR_FILE_NOT_FOUND,
        "installation_receipt_link_refused");
}
std::vector<std::uint8_t> key_bytes(HKEY key) {
    DWORD type = 0, size = 0;
    require(RegQueryValueExW(key, L"Receipt", nullptr, &type, nullptr, &size) == ERROR_SUCCESS &&
        type == REG_BINARY && size > 0 && size <= maximum_receipt_bytes, "installation_receipt_invalid");
    std::vector<std::uint8_t> bytes(size);
    require(RegQueryValueExW(key, L"Receipt", nullptr, &type, bytes.data(), &size) == ERROR_SUCCESS &&
        type == REG_BINARY && size == bytes.size(), "installation_receipt_read_failed");
    return bytes;
}
void put_key(HKEY key, const Receipt& receipt) {
    const auto bytes = encode_receipt(receipt);
    require(RegSetValueExW(key, L"Receipt", 0, REG_BINARY, bytes.data(), static_cast<DWORD>(bytes.size())) ==
        ERROR_SUCCESS && RegFlushKey(key) == ERROR_SUCCESS, "installation_receipt_commit_failed");
    check_key(key, receipt.owner);
    require(key_bytes(key) == bytes, "installation_receipt_readback_failed");
}
void open_exact_key(Key& key, const Receipt& expected, REGSAM access) {
    Key parent; open_registry_parent(parent);
    require(RegOpenKeyExW(HKEY_LOCAL_MACHINE, receipt_name(expected.id).c_str(), REG_OPTION_OPEN_LINK,
        access | KEY_WOW64_64KEY, &key.value) == ERROR_SUCCESS, "installation_receipt_missing");
    check_key(key.value, expected.owner);
    require(key_bytes(key.value) == encode_receipt(expected), "installation_receipt_changed");
}
BY_HANDLE_FILE_INFORMATION info(HANDLE handle) {
    BY_HANDLE_FILE_INFORMATION result{};
    require(GetFileInformationByHandle(handle, &result) != FALSE, "installation_object_inspection_failed");
    return result;
}
} // namespace

void CloseHandleDeleter::operator()(void* value) const noexcept {
    if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value);
}
void LocalFreeDeleter::operator()(void* value) const noexcept { if (value) LocalFree(value); }
void require(bool condition, const char* diagnostic) {
    if (!condition) throw std::runtime_error(diagnostic);
}
Handle process_token(DWORD process_id) {
    Handle process;
    if (process_id) process.reset(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process_id));
    require(!process_id || process != nullptr, "installation_process_unavailable");
    HANDLE token = nullptr;
    require(OpenProcessToken(process_id ? process.get() : GetCurrentProcess(), TOKEN_QUERY, &token) != FALSE,
        "installation_token_unavailable");
    return Handle(token);
}
bool elevated() {
    auto token = process_token(); TOKEN_ELEVATION elevation{}; DWORD size = 0;
    require(GetTokenInformation(token.get(), TokenElevation, &elevation, sizeof(elevation), &size) != FALSE,
        "installation_elevation_unknown");
    return elevation.TokenIsElevated != 0;
}
std::wstring token_sid(HANDLE token) {
    DWORD size = 0; GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    require(size > 0 && size < 65536, "installation_user_size_invalid");
    std::vector<BYTE> bytes(size);
    require(GetTokenInformation(token, TokenUser, bytes.data(), size, &size) != FALSE,
        "installation_user_unavailable");
    return sid_text(reinterpret_cast<TOKEN_USER*>(bytes.data())->User.Sid);
}
std::wstring operator_sid() { auto token = process_token(); return token_sid(token.get()); }
std::wstring unique_id() {
    std::array<BYTE, 16> bytes{};
    require(BCryptGenRandom(nullptr, bytes.data(), static_cast<ULONG>(bytes.size()),
        BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0, "installation_random_failed");
    constexpr wchar_t digits[] = L"0123456789ABCDEF"; std::wstring result;
    for (BYTE b : bytes) { result += digits[b >> 4]; result += digits[b & 15]; }
    return result;
}
Path module_path() {
    std::array<wchar_t, 32768> value{};
    auto size = GetModuleFileNameW(nullptr, value.data(), static_cast<DWORD>(value.size()));
    require(size && size < value.size(), "installation_module_path_unavailable");
    return std::wstring(value.data(), size);
}
Path canonical_root(const Path& input) {
    auto text = input.native(); std::replace(text.begin(), text.end(), L'/', L'\\');
    require(text.size() >= 4 && text.size() <= 160 && text[1] == L':' && text[2] == L'\\' &&
        ((text[0] >= L'A' && text[0] <= L'Z') || (text[0] >= L'a' && text[0] <= L'z')),
        "installation_requires_local_absolute_root");
    text[0] = static_cast<wchar_t>(towupper(text[0]));
    narrow(text); // Existing DeuceSSH host-key file API requires an exact narrow path.
    for (std::size_t start = 3; start < text.size();) {
        const auto end = text.find(L'\\', start);
        const auto part = text.substr(start, end == std::wstring::npos ? end : end - start);
        require(!part.empty() && part != L"." && part != L".." && part.back() != L'.' && part.back() != L' ' &&
            part.find_first_of(L":*?\"<>|~") == std::wstring::npos, "installation_path_component_invalid");
        auto base = part.substr(0, part.find(L'.'));
        std::transform(base.begin(), base.end(), base.begin(), [](wchar_t c) { return static_cast<wchar_t>(towupper(c)); });
        require(base != L"CON" && base != L"PRN" && base != L"AUX" && base != L"NUL" &&
            !(base.size() == 4 && (base.rfind(L"COM", 0) == 0 || base.rfind(L"LPT", 0) == 0) &&
                base[3] >= L'0' && base[3] <= L'9'), "installation_device_name_refused");
        if (end == std::wstring::npos) break;
        start = end + 1; require(start < text.size(), "installation_trailing_separator_refused");
    }
    std::array<wchar_t, MAX_PATH> full{};
    const auto size = GetFullPathNameW(text.c_str(), static_cast<DWORD>(full.size()), full.data(), nullptr);
    require(size && size < full.size() && !_wcsicmp(full.data(), text.c_str()), "installation_path_alias_refused");
    const auto drive = text.substr(0, 3); wchar_t filesystem[32]{}; DWORD flags = 0;
    require(GetDriveTypeW(drive.c_str()) == DRIVE_FIXED &&
        GetVolumeInformationW(drive.c_str(), nullptr, 0, nullptr, nullptr, &flags, filesystem, 32) &&
        !_wcsicmp(filesystem, L"NTFS") && (flags & FILE_PERSISTENT_ACLS), "installation_requires_fixed_ntfs");
    std::array<wchar_t, MAX_PATH> windows{};
    require(GetWindowsDirectoryW(windows.data(), static_cast<UINT>(windows.size())) != 0,
        "installation_windows_directory_unknown");
    const std::wstring system_root(windows.data());
    require(_wcsicmp(text.c_str(), system_root.c_str()) &&
        !(text.size() > system_root.size() && text[system_root.size()] == L'\\' &&
            !_wcsnicmp(text.c_str(), system_root.c_str(), system_root.size())), "installation_system_tree_refused");
    return text;
}
Handle open_object(const Path& path, bool directory, DWORD access, DWORD sharing) {
    Handle handle(CreateFileW(path.c_str(), access, sharing, nullptr, OPEN_EXISTING,
        directory ? directory_flags : FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    require(handle.get() != INVALID_HANDLE_VALUE, "installation_object_open_failed");
    auto value = info(handle.get());
    require(!(value.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
        ((value.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) == directory &&
        (directory || value.nNumberOfLinks == 1), "installation_redirected_object_refused");
    std::array<wchar_t, 32768> final{};
    const auto length = GetFinalPathNameByHandleW(handle.get(), final.data(), static_cast<DWORD>(final.size()),
        FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    const auto expected = L"\\\\?\\" + path.native();
    require(length && length < final.size() && !_wcsicmp(final.data(), expected.c_str()),
        "installation_final_path_mismatch");
    return handle;
}
std::vector<Handle> protect_ancestors(const Path& root, HANDLE ordinary) {
    require(canonical_root(root) == root, "installation_root_not_canonical");
    (void)ordinary;
    std::vector<Handle> handles;
    std::vector<Path> ancestors;
    for (auto path = root.parent_path(); !path.empty();) {
        ancestors.push_back(path); const auto next = path.parent_path();
        if (next == path) break; path = next;
    }
    std::reverse(ancestors.begin(), ancestors.end());
    for (const auto& path : ancestors) {
        try {
            auto handle = open_object(path, true, FILE_READ_ATTRIBUTES|FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE);
            // canonical_root already requires local fixed NTFS; open_object
            // rejects reparse/alias components and verifies the final DOS path.
            // Confirm a real volume mount root, not merely a three-letter alias.
            const bool volume_root = path == path.root_path();
            if (volume_root) {
                wchar_t volume_name[MAX_PATH]{};
                require(GetVolumeNameForVolumeMountPointW(path.c_str(),volume_name,MAX_PATH) != FALSE,
                    "installation_volume_root_unverified");
            }
            // Current no-delete pins/final-path checks protect this operation.
            // A user's ordinary writable parent is not permanent board authority
            // and must not make an otherwise valid local copy unrecoverable.
            handles.push_back(std::move(handle));
        } catch (const std::runtime_error& error) {
            throw AncestorSecurityError(error.what(), path);
        }
    }
    return handles;
}
Handle protect_installer_cache_parent(const Path& cache, HANDLE ordinary) {
    auto parent = open_object(cache.parent_path(), true, FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES | READ_CONTROL,
        FILE_SHARE_READ | FILE_SHARE_WRITE);
    Authorization authorization(ordinary);
    const auto descriptor = file_security(parent.get());
    verify_anchor(descriptor.get(), &authorization, AnchorKind::directory);
    return parent;
}
std::uint64_t file_id(HANDLE file) {
    const auto value = info(file); return (std::uint64_t(value.nFileIndexHigh) << 32) | value.nFileIndexLow;
}
DWORD volume_serial(HANDLE file) { return info(file).dwVolumeSerialNumber; }
bool exact_security(HANDLE file, void* expected) {
    const auto actual = file_security(file); return same_descriptor(actual.get(), expected);
}
Descriptor structural_descriptor(const std::wstring& owner) {
    return from_sddl(L"O:" + owner + L"D:P(A;;RC;;;OW)(A;;FA;;;SY)(A;;FA;;;BA)(A;;0x1200a9;;;" + owner + L")");
}
Descriptor mutable_descriptor(const std::wstring& owner) {
    // Same private mutable content policy used by the accepted storage adapter.
    // Directory boundaries are not a defence against trusted same-identity code.
    return from_sddl(L"O:" + owner + L"D:P(A;OICI;0x1301ff;;;" + owner +
        L")(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)");
}
bool exact_mutable_descriptor(void* actual, const std::wstring& owner) {
    auto expected = mutable_descriptor(owner);
    return same_descriptor(actual, expected.get(), true);
}
void create_directory(const Path& path, void* descriptor) {
    SECURITY_ATTRIBUTES sa{sizeof(sa), descriptor, FALSE};
    {
        PSID owner{};BOOL defaulted{};
        require(GetSecurityDescriptorOwner(descriptor,&owner,&defaulted)&&owner,"installation_creation_owner_missing");
        ServiceFileOwnerScope scope(sid_text(owner));require(scope.valid(),"installation_creation_owner_scope_failed");
        require(CreateDirectoryW(path.c_str(), &sa) != FALSE, "installation_fresh_directory_collision_or_failure");
    }
    auto handle = open_object(path, true);
    require(exact_security(handle.get(), descriptor), "installation_creation_security_mismatch");
}
void create_file(const Path& path, const std::vector<std::uint8_t>& bytes, void* descriptor) {
    SECURITY_ATTRIBUTES sa{sizeof(sa), descriptor, FALSE};
    Handle file;
    {
        PSID owner{};BOOL defaulted{};
        require(GetSecurityDescriptorOwner(descriptor,&owner,&defaulted)&&owner,"installation_creation_owner_missing");
        ServiceFileOwnerScope scope(sid_text(owner));require(scope.valid(),"installation_creation_owner_scope_failed");
        file.reset(CreateFileW(path.c_str(), GENERIC_WRITE | READ_CONTROL, 0, &sa, CREATE_NEW,
            FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_WRITE_THROUGH, nullptr));
    }
    require(file.get() != INVALID_HANDLE_VALUE, "installation_fresh_file_collision_or_failure");
    require(bytes.size() <= std::numeric_limits<DWORD>::max(), "installation_payload_too_large");
    DWORD written = 0;
    require(WriteFile(file.get(), bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) &&
        written == bytes.size() && FlushFileBuffers(file.get()), "installation_payload_write_failed");
    require(exact_security(file.get(), descriptor), "installation_program_security_mismatch");
}
std::vector<std::uint8_t> read_file(const Path& path, std::size_t maximum) {
    auto handle = open_object(path, false, GENERIC_READ, FILE_SHARE_READ);
    LARGE_INTEGER size{};
    require(GetFileSizeEx(handle.get(), &size) && size.QuadPart >= 0 &&
        static_cast<unsigned long long>(size.QuadPart) <= maximum && size.QuadPart <= MAXDWORD,
        "installation_file_size_invalid");
    std::vector<std::uint8_t> result(static_cast<std::size_t>(size.QuadPart)); DWORD read = 0;
    require(ReadFile(handle.get(), result.data(), static_cast<DWORD>(result.size()), &read, nullptr) &&
        read == result.size(), "installation_file_read_failed");
    return result;
}
std::string sha256(const std::vector<std::uint8_t>& bytes) {
    BCRYPT_ALG_HANDLE algorithm = nullptr; BCRYPT_HASH_HANDLE hash = nullptr;
    require(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0,
        "installation_hash_provider_failed");
    std::array<BYTE, 32> digest{};
    const bool ok = BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) == 0 &&
        BCryptHashData(hash, const_cast<PUCHAR>(bytes.data()), static_cast<ULONG>(bytes.size()), 0) == 0 &&
        BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) == 0;
    if (hash) BCryptDestroyHash(hash); BCryptCloseAlgorithmProvider(algorithm, 0);
    require(ok, "installation_hash_failed");
    constexpr char digits[] = "0123456789ABCDEF"; std::string result;
    for (auto b : digest) { result += digits[b >> 4]; result += digits[b & 15]; }
    return result;
}
std::string file_sha256(const Path& path) { return sha256(read_file(path, 128U * 1024U * 1024U)); }

std::vector<std::uint8_t> encode_receipt(const Receipt& r) {
    require(hex(r.id, 32) && !r.owner.empty() && r.generation && r.root_id && r.bin_id &&
        r.root_id != r.bin_id &&
        (r.program_count == legacy_program_count ||
         r.program_count == archive_program_count ||
         r.program_count == configuration_program_count ||
         r.program_count == message_program_count ||
         r.program_count == packet_program_count ||
         r.program_count == file_program_count ||
         r.program_count == maintenance_program_count ||
         r.program_count == current_program_count),
        "installation_receipt_fields_invalid");
    PSID raw_owner = nullptr;
    require(ConvertStringSidToSidW(r.owner.c_str(), &raw_owner) != FALSE,
        "installation_receipt_owner_invalid");
    Descriptor owner(raw_owner);
    require(sid_text(raw_owner) == r.owner && !trusted_sid(r.owner) &&
        r.owner.rfind(L"S-1-5-21-", 0) == 0, "installation_receipt_owner_not_operator");
    std::string text = std::string(r.program_count == legacy_program_count
                                       ? policy_v1
                                       : r.program_count == archive_program_count
                                           ? policy_v2 : r.program_count == configuration_program_count ? policy_v3 : r.program_count == message_program_count ? policy_v4 : r.program_count == packet_program_count ? policy_v5 : r.program_count == file_program_count ? policy_v6 : r.program_count == maintenance_program_count ? policy_v7 : r.ftn_layout ? policy_v9 : policy_v8) +
        "\n" + narrow(r.id) + "\n" + narrow(r.root.native()) + "\n" +
        narrow(r.owner) + "\n" + std::to_string(r.generation) + "\n" + (r.committed ? "committed" : "incomplete") +
        "\n" + std::to_string(r.volume) + "\n" + std::to_string(r.root_id) + "\n" + std::to_string(r.bin_id) + "\n";
    for (std::size_t n = 0; n != r.program_count; ++n) {
        require(r.program_ids[n] && hex(widen(r.program_hashes[n]), 64), "installation_receipt_program_invalid");
        text += std::to_string(r.program_ids[n]) + "\n" + r.program_hashes[n] + "\n";
    }
    const auto& component=r.door_component;
    if(r.program_count==current_program_count){
        if(component.registered){
            require(component.path.is_absolute()&&!_wcsicmp(component.path.filename().c_str(),L"SFXEXEC.DLL")&&component.volume&&component.file_id&&hex(widen(component.sha256),64)&&component.compatibility=="SFXEXEC-PRIVATE20-21-1","installation_dos_component_invalid");
            text+="system32\n"+narrow(component.path.native())+"\n"+std::to_string(component.volume)+"\n"+std::to_string(component.file_id)+"\n"+component.sha256+"\n"+(component.introduced?"1":"0")+"\n"+component.compatibility+"\n";
        }else{
            require(!component.introduced&&component.path.empty()&&!component.volume&&!component.file_id&&component.sha256.empty()&&component.compatibility.empty(),"installation_absent_component_invalid");
            text+="unavailable\n-\n0\n0\n-\n0\n-\n";
        }
    }else require(!component.registered,"installation_legacy_component_invalid");
    if(r.ftn_layout){
        require(r.program_count==current_program_count&&r.ftn_volume==r.volume&&r.ftn_directory_id&&r.ftn_directory_id!=r.root_id&&r.ftn_directory_id!=r.bin_id&&std::find(r.program_ids.begin(),r.program_ids.end(),r.ftn_directory_id)==r.program_ids.end(),"installation_ftn_capability_invalid");
        text+="ftn-private-v1\n"+std::to_string(r.ftn_volume)+"\n"+std::to_string(r.ftn_directory_id)+"\n";
    }else require(r.ftn_volume==0&&r.ftn_directory_id==0,"installation_absent_ftn_capability_invalid");
    require(text.size() <= maximum_receipt_bytes, "installation_receipt_oversized");
    return {text.begin(), text.end()};
}
Receipt decode_receipt(const std::vector<std::uint8_t>& bytes) {
    require(!bytes.empty() && bytes.size() <= maximum_receipt_bytes && bytes.back() == '\n',
        "installation_receipt_invalid_size");
    std::vector<std::string> fields; std::string field;
    for (auto b : bytes) {
        if (b == '\n') { fields.push_back(field); field.clear(); }
        else { require(b >= 32 && b < 127, "installation_receipt_invalid_character"); field += static_cast<char>(b); }
    }
    const bool version1 = fields.size() == 13 && fields[0] == policy_v1;
    const bool version2 = fields.size() == 17 && fields[0] == policy_v2;
    const bool version3 = fields.size() == 19 && fields[0] == policy_v3;
    const bool version4 = fields.size() == 21 && fields[0] == policy_v4;
    const bool version5 = fields.size() == 23 && fields[0] == policy_v5;
    const bool version6 = fields.size() == 27 && fields[0] == policy_v6;
    const bool version7 = fields.size() == 29 && fields[0] == policy_v7;
    const bool version8 = fields.size() == 40 && fields[0] == policy_v8;
    const bool version9 = fields.size() == 43 && fields[0] == policy_v9;
    require(version1 || version2 || version3 || version4 || version5 || version6 || version7 || version8 || version9, "installation_receipt_version_or_fields_invalid");
    const auto number = [&](std::size_t n) {
        std::uint64_t value = 0; const auto& f = fields[n];
        const auto parsed = std::from_chars(f.data(), f.data() + f.size(), value);
        require(parsed.ec == std::errc{} && parsed.ptr == f.data() + f.size() && std::to_string(value) == f,
            "installation_receipt_number_invalid"); return value;
    };
    Receipt r; r.id = widen(fields[1]); r.root = widen(fields[2]); r.owner = widen(fields[3]);
    r.program_count = version1 ? legacy_program_count :
        version2 ? archive_program_count : version3 ? configuration_program_count : version4 ? message_program_count : version5 ? packet_program_count : version6 ? file_program_count : version7 ? maintenance_program_count : current_program_count;
    r.generation = number(4);
    require(fields[5] == "committed" || fields[5] == "incomplete", "installation_receipt_state_invalid");
    r.committed = fields[5] == "committed";
    require(number(6) <= MAXDWORD, "installation_receipt_volume_invalid"); r.volume = static_cast<DWORD>(number(6));
    r.root_id = number(7); r.bin_id = number(8);
    for (std::size_t n = 0; n != r.program_count; ++n) {
        r.program_ids[n] = number(9 + n * 2);
        r.program_hashes[n] = fields[10 + n * 2];
    }
    if((version8||version9)&&fields[33]=="system32"){
        auto& component=r.door_component;component.registered=true;component.path=widen(fields[34]);require(number(35)<=MAXDWORD,"installation_component_volume_invalid");component.volume=static_cast<DWORD>(number(35));component.file_id=number(36);component.sha256=fields[37];require(fields[38]=="0"||fields[38]=="1","installation_component_ownership_invalid");component.introduced=fields[38]=="1";component.compatibility=fields[39];
    }
    if(version9){require(fields[40]=="ftn-private-v1"&&number(41)<=MAXDWORD,"installation_ftn_capability_invalid");r.ftn_layout=true;r.ftn_volume=static_cast<DWORD>(number(41));r.ftn_directory_id=number(42);}
    require(encode_receipt(r) == bytes, "installation_receipt_noncanonical"); return r;
}
std::vector<const wchar_t*> mutable_directories_for(const Receipt& receipt){
    std::vector<const wchar_t*> result;for(const auto* leaf:mutable_directories){if(receipt.program_count<current_program_count&&!_wcsicmp(leaf,L"Node"))continue;result.push_back(leaf);}if(receipt.ftn_layout)result.push_back(L"FTN");return result;
}
void verify_ftn_directory(const Receipt& receipt){
    require(receipt.ftn_layout&&receipt.program_count==current_program_count&&receipt.ftn_directory_id&&receipt.ftn_volume==receipt.volume,"installation_ftn_capability_missing");
    auto directory=open_object(receipt.root/L"FTN",true,READ_CONTROL|FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE);require(volume_serial(directory.get())==receipt.ftn_volume&&file_id(directory.get())==receipt.ftn_directory_id&&WindowsStorageDaclPolicy(receipt.owner).assess(receipt.root/L"FTN",StorageDaclObjectKind::directory).status.succeeded(),"installation_ftn_boundary_invalid");
}
Receipt read_receipt(const Path& root, const std::wstring& owner) {
    auto ordinary = ancestry_token(); const auto ancestors = protect_ancestors(root, ordinary.get());
    auto root_handle = open_object(root, true); auto bin_handle = open_object(root / L"Bin", true);
    auto id_bytes = read_file(root / L"Bin" / L"installation.id", 32);
    const std::wstring id(id_bytes.begin(), id_bytes.end()); receipt_name(id);
    Key parent; open_registry_parent(parent); Key key;
    require(RegOpenKeyExW(HKEY_LOCAL_MACHINE, receipt_name(id).c_str(), REG_OPTION_OPEN_LINK,
        KEY_READ | KEY_WOW64_64KEY, &key.value) == ERROR_SUCCESS, "installation_receipt_missing");
    check_key(key.value, owner); auto result = decode_receipt(key_bytes(key.value));
    require(result.id == id && result.owner == owner && !_wcsicmp(result.root.c_str(), root.c_str()),
        "installation_receipt_binding_mismatch"); return result;
}
namespace {
std::wstring recorded_root_key(const Path& root) {
    auto key=root.lexically_normal().native();std::replace(key.begin(),key.end(),L'/',L'\\');
    while(key.size()>3 && key.back()==L'\\') key.pop_back();
    std::transform(key.begin(),key.end(),key.begin(),[](wchar_t value){return static_cast<wchar_t>(towlower(value));});
    return key;
}
}
std::vector<RegisteredInstallation> unique_installation_targets(std::vector<RegisteredInstallation> records) {
    std::vector<RegisteredInstallation> found;
    for(auto& candidate:records) {
        const auto key=recorded_root_key(candidate.receipt.root);
        auto duplicate=std::find_if(found.begin(),found.end(),[&](const auto& entry){return recorded_root_key(entry.receipt.root)==key;});
        if(duplicate==found.end()) found.push_back(std::move(candidate));
        else if(candidate.available) *duplicate=std::move(candidate);
    }
    std::sort(found.begin(),found.end(),[](const auto& a,const auto& b){return recorded_root_key(a.receipt.root)<recorded_root_key(b.receipt.root);});
    return found;
}
Path suggested_installation_root(const std::vector<RegisteredInstallation>& installations) {
    if(installations.empty()) return L"C:\\SFX";
    const RegisteredInstallation* selected=nullptr;
    for(const auto& entry:installations) if(entry.available) {
        if(selected && recorded_root_key(selected->receipt.root)!=recorded_root_key(entry.receipt.root)) return {};
        selected=&entry;
    }
    if(selected) return selected->receipt.root;
    if(installations.size()==1) return installations.front().receipt.root;
    return {};
}
bool recorded_installation_root(const std::vector<RegisteredInstallation>& installations,const Path& root) {
    return std::any_of(installations.begin(),installations.end(),[&](const auto& entry){
        return recorded_root_key(entry.receipt.root)==recorded_root_key(root);});
}
std::vector<RegisteredInstallation> registered_installations(const std::vector<std::wstring>& ids,
    bool include_history, const Path& exact_recorded_root) {
    const auto owner=operator_sid();
    Key parent;open_registry_parent(parent);
    std::vector<std::wstring> names;
    if(!ids.empty()) {
        for(const auto& id:ids) names.push_back(receipt_name(id).substr(9)); // strip the SOFTWARE parent
    } else {
        constexpr std::wstring_view prefix=L"Spitfire-X BBS.Installation.";
        for(DWORD index=0;;++index) {
            std::array<wchar_t,256> name{};DWORD length=static_cast<DWORD>(name.size());
            const auto result=RegEnumKeyExW(parent.value,index,name.data(),&length,nullptr,nullptr,nullptr,nullptr);
            if(result==ERROR_NO_MORE_ITEMS) break;
            require(result==ERROR_SUCCESS || result==ERROR_MORE_DATA,"installation_discovery_enumeration_failed");
            if(result!=ERROR_SUCCESS) continue;
            const std::wstring candidate(name.data(),length);
            if(candidate.compare(0,prefix.size(),prefix)==0) {
                require(names.size()<256,"installation_discovery_too_many_records");
                names.push_back(candidate);
            }
        }
    }
    std::vector<RegisteredInstallation> found;
    for(const auto& name:names) {
        Key key;
        require(RegOpenKeyExW(parent.value,name.c_str(),REG_OPTION_OPEN_LINK,
            KEY_READ|KEY_WOW64_64KEY,&key.value)==ERROR_SUCCESS,"installation_discovery_record_unreadable");
        // Decode only a bounded value, then authenticate the record's complete
        // ACL and owner binding before trusting its path as a UI suggestion.
        auto record=decode_receipt(key_bytes(key.value));
        if(record.owner!=owner) continue;
        if(!exact_recorded_root.empty() &&
            recorded_root_key(record.root)!=recorded_root_key(exact_recorded_root)) continue;
        check_key(key.value,owner);
        require(receipt_name(record.id)==L"SOFTWARE\\"+name && (record.committed||include_history),
            "installation_discovery_record_invalid");
        RegisteredInstallation candidate{record,false};
        if(!record.committed){found.push_back(std::move(candidate));continue;}
        try {
            const auto root=canonical_root(record.root);
            const auto bound=read_receipt(root,owner);
            require(encode_receipt(bound)==encode_receipt(record),"installation_discovery_record_changed");
            verify_structure(bound,false);candidate.available=true;
        } catch(const std::exception&) {
            // Keep the protected recorded path visible even when the volume is
            // absent or folder protection has changed. Review must refuse it.
        }
        // Old protected receipts can name the same BBS after reinstallation.
        // They are history, not separate targets. Only the receipt bound by the
        // current installation.id and verified structure may be available.
        found.push_back(std::move(candidate));
    }
    return include_history ? std::move(found) : unique_installation_targets(std::move(found));
}
Receipt verify_configuration_recovery_entry(const Path& root) {
    require(elevated(), "configuration_recovery_elevation_required");
    auto ordinary = ordinary_token(); const auto ancestors = protect_ancestors(root, ordinary.get());
    auto root_handle = open_object(root, true); auto bin_handle = open_object(root / L"Bin", true);
    const auto id_bytes = read_file(root / L"Bin" / L"installation.id", 32);
    const std::wstring id(id_bytes.begin(), id_bytes.end()); receipt_name(id);
    Key parent; open_registry_parent(parent); Key key;
    require(RegOpenKeyExW(HKEY_LOCAL_MACHINE, receipt_name(id).c_str(), REG_OPTION_OPEN_LINK,
        KEY_READ | KEY_WOW64_64KEY, &key.value) == ERROR_SUCCESS, "installation_receipt_missing");
    const auto receipt = decode_receipt(key_bytes(key.value));
    check_key(key.value, receipt.owner);
    require(receipt.id == id && receipt.committed && !_wcsicmp(receipt.root.c_str(),root.c_str()),
        "configuration_recovery_receipt_binding_mismatch");
    verify_structure(receipt);
    require(receipt.program_count > configuration_program_index &&
        !_wcsicmp(module_path().c_str(), (receipt.root / L"Bin" / programs[configuration_program_index]).c_str()),
        "configuration_recovery_image_mismatch");
    for (const auto* leaf : mutable_directories_for(receipt)) {
        auto boundary = open_object(root / leaf, true);
        require(WindowsStorageDaclPolicy(receipt.owner).assess(root/leaf,StorageDaclObjectKind::directory).status.succeeded(), "installation_mutable_boundary_invalid");
    }
    return receipt;
}

void verify_structure(const Receipt& receipt, bool verify_programs) {
    verify_structure(receipt, verify_programs, {});
}
void verify_structure(const Receipt& receipt, bool verify_programs,
                      const StructureVerificationCallbacks& callbacks) {
    struct Clock {
        const StructureVerificationCallbacks& callbacks;
        bool program; std::size_t role;
        StructureVerificationTiming::Detail detail{StructureVerificationTiming::Detail::aggregate};
        bool active{true};
        std::chrono::steady_clock::time_point start{std::chrono::steady_clock::now()};
        void emit(bool completed) noexcept {
            if (!callbacks.timing) return;
            try { callbacks.timing({program,role,static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now()-start).count()),completed,detail}); } catch (...) {}
        }
        void complete() noexcept { emit(true); active=false; }
        ~Clock(){if(active)emit(false);}
    };
    if(callbacks.checkpoint)callbacks.checkpoint();
    Clock boundaries{callbacks,false,0};
    require(receipt.program_count == legacy_program_count ||
                receipt.program_count == archive_program_count ||
                receipt.program_count == configuration_program_count ||
                receipt.program_count == message_program_count ||
                receipt.program_count == packet_program_count ||
                receipt.program_count == file_program_count ||
                receipt.program_count == maintenance_program_count ||
                receipt.program_count == current_program_count,
            "installation_program_count_invalid");
    auto ordinary = ancestry_token(); const auto ancestors = protect_ancestors(receipt.root, ordinary.get());
    auto root = open_object(receipt.root, true); auto bin = open_object(receipt.root / L"Bin", true);
    require(volume_serial(root.get()) == receipt.volume && file_id(root.get()) == receipt.root_id &&
        volume_serial(bin.get()) == receipt.volume && file_id(bin.get()) == receipt.bin_id,
        "installation_structure_mismatch");
    auto identity = open_object(receipt.root / L"Bin" / L"installation.id", false);
    const auto identity_bytes=read_file(receipt.root/L"Bin"/L"installation.id",32);
    require(std::wstring(identity_bytes.begin(),identity_bytes.end())==receipt.id,"installation_identity_changed");
    if(receipt.ftn_layout)verify_ftn_directory(receipt);
    if (!verify_programs) { boundaries.complete(); return; }
    if (receipt.program_count >= archive_program_count) {
        auto tools = open_object(receipt.root / L"Bin" / L"Tools", true);
        require(volume_serial(tools.get()) == receipt.volume,
                "installation_tools_directory_mismatch");
    }
    boundaries.complete();
    for (std::size_t n = 0; n < receipt.program_count; ++n) {
        if(callbacks.checkpoint)callbacks.checkpoint();
        Clock role_time{callbacks,true,n};
        const auto path = receipt.root / L"Bin" / programs[n];
        using Detail=StructureVerificationTiming::Detail;
        auto program=[&]{Clock time{callbacks,true,n,Detail::open_security};auto value=open_object(path,false);time.complete();return value;}();
        const auto native_matches=[&]{Clock time{callbacks,true,n,Detail::native_identity};const bool matches=volume_serial(program.get())==receipt.volume&&file_id(program.get())==receipt.program_ids[n];time.complete();return matches;};
        const auto digest=[&]{
            if(!callbacks.timing)return file_sha256(path);
            // Keep read_file's second guarded open, size checks and ReadFile
            // intact; this span is not a pure physical-I/O measurement.
            Clock read_time{callbacks,true,n,Detail::file_read};auto bytes=read_file(path,128U*1024U*1024U);read_time.complete();
            Clock hash_time{callbacks,true,n,Detail::sha};auto hash=sha256(bytes);hash_time.complete();return hash;
        };
        require(native_matches() && digest() == receipt.program_hashes[n],
            "installation_program_mismatch");
        role_time.complete();
    }
}
namespace {
void verify_process_identity(const Receipt& receipt,const DWORD process_id,const std::size_t program_index){
    require(process_id != 0 && program_index < receipt.program_count,
        "installation_program_process_invalid");
    auto token = process_token(process_id);
    require(token_sid(token.get()) == receipt.owner,
        "installation_program_process_owner_mismatch");
    Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process_id));
    require(process != nullptr, "installation_program_process_unavailable");
    std::vector<wchar_t> path(32768U, L'\0');
    DWORD length = static_cast<DWORD>(path.size());
    require(QueryFullProcessImageNameW(process.get(), 0, path.data(), &length) != FALSE &&
        length > 0 && length < path.size(), "installation_program_process_path_unavailable");
    const auto expected = receipt.root / L"Bin" / programs[program_index];
    require(_wcsicmp(std::wstring(path.data(), length).c_str(), expected.c_str()) == 0,
        "installation_program_process_path_mismatch");
}
}
void verify_installed_program_process(const Receipt& receipt,const DWORD process_id,const std::size_t program_index){
    verify_structure(receipt);verify_process_identity(receipt,process_id,program_index);
}
struct InstallationVerificationLease::Impl {
    Receipt receipt;
    std::vector<Handle> pins;
    Handle root,bin;
    explicit Impl(const Receipt& value):receipt(value){
        require(receipt.program_count>0&&receipt.program_count<=programs.size(),"installation_verification_lease_count_invalid");
        root=open_object(receipt.root,true,FILE_READ_ATTRIBUTES|READ_CONTROL,FILE_SHARE_READ);
        bin=open_object(receipt.root/L"Bin",true,FILE_READ_ATTRIBUTES|READ_CONTROL,FILE_SHARE_READ);
        for(std::size_t n=0;n<receipt.program_count;++n)pins.push_back(open_object(receipt.root/L"Bin"/programs[n],false,GENERIC_READ|READ_CONTROL,FILE_SHARE_READ));
        // Hash only AFTER the non-writable/non-replaceable objects are pinned.
        verify_structure(receipt);
    }
};
InstallationVerificationLease::InstallationVerificationLease(const Receipt& receipt):impl_(std::make_unique<Impl>(receipt)){}
InstallationVerificationLease::~InstallationVerificationLease()=default;
void InstallationVerificationLease::verify_process(const Receipt& current,DWORD pid,std::size_t role)const{
    require(encode_receipt(current)==encode_receipt(impl_->receipt),"installation_verification_lease_receipt_changed");
    verify_structure(current,false);
    for(std::size_t n=0;n<current.program_count;++n){
        const auto file=impl_->pins[n].get();
        require(volume_serial(file)==current.volume&&file_id(file)==current.program_ids[n],"installation_verification_lease_object_changed");
    }
    verify_process_identity(current,pid,role);
}
void create_receipt(const Receipt& receipt,bool resume_preparation) {
    require(elevated() && receipt.committed, "installation_receipt_creation_not_authorized");
    verify_structure(receipt);
    Key parent; open_registry_parent(parent); Key key; auto sd = receipt_descriptor(receipt.owner);
    SECURITY_ATTRIBUTES attributes{sizeof(attributes), sd.get(), FALSE}; DWORD disposition = 0;
    require(RegCreateKeyExW(HKEY_LOCAL_MACHINE, receipt_name(receipt.id).c_str(), 0, nullptr,
        REG_OPTION_NON_VOLATILE, KEY_ALL_ACCESS | KEY_WOW64_64KEY, &attributes, &key.value, &disposition) ==
        ERROR_SUCCESS, "installation_receipt_creation_failed");
    auto actual = registry_security(key.value);
    require(same_descriptor(actual.get(), sd.get()), "installation_receipt_creation_security_invalid");
    if(disposition!=REG_CREATED_NEW_KEY){
        require(resume_preparation,"installation_receipt_collision_refused");
        const auto pending=read_file(receipt.root/L"Bin"/L"preparation.pending",maximum_receipt_bytes+128);
        const std::string text(pending.begin(),pending.end());std::size_t offset=0;
        require(text.rfind("SFX-PREPARATION-1\n",0)==0,"installation_preparation_proof_invalid");
        for(unsigned line=0;line<3;++line){const auto end=text.find('\n',offset);require(end!=text.npos,"installation_preparation_proof_invalid");offset=end+1;}
        auto expected=decode_receipt({pending.begin()+offset,pending.end()});auto comparison=receipt;
        expected.door_component={};comparison.door_component={};
        require(encode_receipt(expected)==encode_receipt(comparison),"installation_preparation_proof_mismatch");
        DWORD children{},values{};require(RegQueryInfoKeyW(key.value,nullptr,nullptr,nullptr,&children,nullptr,nullptr,&values,nullptr,nullptr,nullptr,nullptr)==ERROR_SUCCESS&&!children&&values<=1,"installation_preparation_key_inventory_changed");
        // RegSetValueEx is atomic: only an empty key from this exact pending
        // operation, or its exact complete receipt, may be resumed.
        if(values)require(key_bytes(key.value)==encode_receipt(receipt),"installation_preparation_key_conflict_preserved");
    }
    put_key(key.value, receipt);
    write_portable_installation_input(receipt);
}
void replace_receipt(const Receipt& expected, const Receipt& replacement) {
    require(elevated() && expected.id == replacement.id && expected.root == replacement.root &&
        expected.owner == replacement.owner && expected.root_id == replacement.root_id &&
        expected.bin_id == replacement.bin_id, "installation_receipt_replacement_invalid");
    Key key; open_exact_key(key, expected, KEY_READ | KEY_SET_VALUE); put_key(key.value, replacement);
    write_portable_installation_input(replacement);
}
void remove_receipt(const Receipt& expected) {
    require(elevated(), "installation_receipt_removal_not_authorized");
    Key key; open_exact_key(key, expected, KEY_READ);
    require(RegDeleteKeyExW(HKEY_LOCAL_MACHINE, receipt_name(expected.id).c_str(), KEY_WOW64_64KEY, 0) ==
        ERROR_SUCCESS, "installation_receipt_removal_failed");
}
Receipt inspect_generation(const Path& root_path, const std::wstring& owner, const std::wstring& id,
    std::uint64_t generation, bool committed, const std::size_t program_count,bool ftn_layout) {
    Receipt r; r.id = id; r.root = root_path; r.owner = owner; r.generation = generation; r.committed = committed;
    require(program_count == legacy_program_count ||
                program_count == archive_program_count ||
                program_count == configuration_program_count ||
                program_count == message_program_count ||
                program_count == packet_program_count ||
                program_count == file_program_count ||
                program_count == maintenance_program_count ||
                program_count == current_program_count,
            "installation_program_count_invalid");
    r.program_count = program_count;
    auto root = open_object(root_path, true); auto bin = open_object(root_path / L"Bin", true);
    r.volume = volume_serial(root.get()); r.root_id = file_id(root.get()); r.bin_id = file_id(bin.get());
    for (std::size_t n = 0; n < r.program_count; ++n) {
        const auto path = root_path / L"Bin" / programs[n]; auto file = open_object(path, false);
        r.program_ids[n] = file_id(file.get()); r.program_hashes[n] = file_sha256(path);
    }
    if(ftn_layout){require(program_count==current_program_count,"installation_ftn_legacy_payload_refused");auto directory=open_object(root_path/L"FTN",true);r.ftn_layout=true;r.ftn_volume=volume_serial(directory.get());r.ftn_directory_id=file_id(directory.get());}
    verify_structure(r); return r;
}
void verify_runtime_installation(const Path& root, const std::wstring& owner) {
    require(GetFileAttributesW((root/L"Bin"/L"preparation.pending").c_str())==INVALID_FILE_ATTRIBUTES&&GetLastError()==ERROR_FILE_NOT_FOUND,
        "preparation_incomplete_open_control_panel_or_config_to_retry");
    require(!elevated() || is_owned_service_runtime(root,owner), "installation_launch_unelevated_required");
    const auto receipt = read_receipt(root, owner);
    require(receipt.committed, "installation_incomplete_run_setup_repair");
    verify_structure(receipt);
    for (const auto* leaf : mutable_directories_for(receipt)) {
        auto boundary = open_object(root / leaf, true);
        require(WindowsStorageDaclPolicy(owner).assess(root/leaf,StorageDaclObjectKind::directory).status.succeeded(), "installation_mutable_boundary_invalid");
    }
}
} // namespace sfx::platform::win32::installation
