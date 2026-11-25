#include "sfx/platform/win32/backup_read_access.hpp"
#include <Aclapi.h>
#include <sddl.h>
#include <authz.h>
#include <algorithm>
#include <cstring>
#include <memory>
#include <stdexcept>

namespace sfx::platform::win32 {
namespace {
void need(bool yes, const char* message) { if (!yes) throw std::runtime_error(message); }
struct Free { void operator()(void* p) const { if (p) LocalFree(p); } };
using Local = std::unique_ptr<void, Free>;
struct Close { HANDLE value; ~Close() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); } };
constexpr DWORD read_mask = FILE_GENERIC_READ | FILE_GENERIC_EXECUTE;
GENERIC_MAPPING mapping{FILE_GENERIC_READ, FILE_GENERIC_WRITE, FILE_GENERIC_EXECUTE, FILE_ALL_ACCESS};
bool stable(PSID sid) {
    if (!IsValidSid(sid)) return false;
    const SID_IDENTIFIER_AUTHORITY nt = SECURITY_NT_AUTHORITY;
    if (std::memcmp(GetSidIdentifierAuthority(sid), &nt, sizeof(nt))) return false;
    const auto count = *GetSidSubAuthorityCount(sid);
    if (!count) return false;
    const auto first = *GetSidSubAuthority(sid, 0);
    if (first == SECURITY_NT_NON_UNIQUE && count >= 5) return true;
    if (first == SECURITY_SERVICE_ID_BASE_RID && count >= 2) return true;
    if (first != SECURITY_BUILTIN_DOMAIN_RID || count != 2) return false;
    const auto rid = *GetSidSubAuthority(sid, 1);
    return rid != DOMAIN_ALIAS_RID_USERS && rid != DOMAIN_ALIAS_RID_GUESTS;
}
Local sid_value(const std::wstring& text) {
    PSID sid{}; need(ConvertStringSidToSidW(text.c_str(), &sid) && IsValidSid(sid),
        "backup_reader_owner_sid_invalid"); return Local(sid);
}
std::vector<unsigned char> bytes_of(const void* value, std::size_t size) {
    const auto* first = static_cast<const unsigned char*>(value);
    return {first, first + size};
}
std::vector<unsigned char> acl_entries(PACL acl, const BackupReadGrants& extra) {
    need(acl && IsValidAcl(acl), "backup_reader_dacl_missing");
    BackupReadGrants entries;
    for (DWORD n = 0; n < acl->AceCount; ++n) {
        void* raw{}; need(GetAce(acl, n, &raw), "backup_reader_ace_unavailable");
        entries.push_back(bytes_of(raw, static_cast<ACE_HEADER*>(raw)->AceSize));
    }
    for (const auto& entry : extra)
        if (std::find(entries.begin(), entries.end(), entry) == entries.end()) entries.push_back(entry);
    const auto rank = [](const auto& e) {
            auto* h = reinterpret_cast<const ACE_HEADER*>(e.data());
            return (h->AceFlags & INHERITED_ACE ? 2 : 0) +
                (h->AceType == ACCESS_DENIED_ACE_TYPE || h->AceType == ACCESS_DENIED_OBJECT_ACE_TYPE ? 0 : 1);
    };
    // Never reorder a prior custom ACL. Unsupported noncanonical layouts
    // require review with their original permissions intact.
    for (std::size_t n = 1; n < acl->AceCount; ++n)
        need(rank(entries[n - 1]) <= rank(entries[n]), "backup_reader_noncanonical_dacl_preserved");
    BackupReadGrants ordered(entries.begin(), entries.begin() + acl->AceCount);
    for (std::size_t n = acl->AceCount; n < entries.size(); ++n) {
        const auto position = std::find_if(ordered.begin(), ordered.end(), [&](const auto& e) { return rank(e) > rank(entries[n]); });
        ordered.insert(position, entries[n]);
    }
    std::size_t size = sizeof(ACL); for (const auto& e : entries) size += e.size();
    need(size <= 65535, "backup_reader_dacl_too_large");
    std::vector<unsigned char> result(size);
    auto* merged = reinterpret_cast<PACL>(result.data());
    need(InitializeAcl(merged, static_cast<DWORD>(size), ACL_REVISION_DS), "backup_reader_dacl_init_failed");
    for (const auto& e : ordered) need(AddAce(merged, ACL_REVISION_DS, MAXDWORD,
        const_cast<unsigned char*>(e.data()), static_cast<DWORD>(e.size())), "backup_reader_dacl_merge_failed");
    return result;
}
void set_dacl(HANDLE object, PACL acl, bool protect) {
    need(SetSecurityInfo(object, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION |
        (protect ? PROTECTED_DACL_SECURITY_INFORMATION : 0),
        nullptr, nullptr, acl, nullptr) == ERROR_SUCCESS, "backup_reader_dacl_update_failed");
}
bool protected_dacl(void* descriptor) {
    SECURITY_DESCRIPTOR_CONTROL control{}; DWORD revision{};
    need(GetSecurityDescriptorControl(descriptor, &control, &revision), "backup_reader_control_unavailable");
    return (control & SE_DACL_PROTECTED) != 0;
}
void verify_owner_denies(PACL original, PSID owner, DWORD wanted) {
    // Evaluate all original denials using the stable account's Windows groups,
    // without a transient logon SID or the caller's administrator bypass.
    std::vector<unsigned char> rules(original->AclSize + 64);
    auto* denies = reinterpret_cast<PACL>(rules.data());
    need(InitializeAcl(denies, static_cast<DWORD>(rules.size()), ACL_REVISION_DS), "backup_reader_deny_init_failed");
    for (DWORD n = 0; n < original->AceCount; ++n) {
        void* raw{}; need(GetAce(original, n, &raw), "backup_reader_deny_unavailable");
        auto* header = static_cast<ACE_HEADER*>(raw);
        need(header->AceType == ACCESS_ALLOWED_ACE_TYPE || header->AceType == ACCESS_DENIED_ACE_TYPE,
            "backup_reader_deny_requires_review");
        if (header->AceType != ACCESS_DENIED_ACE_TYPE || (header->AceFlags & INHERIT_ONLY_ACE)) continue;
        auto copy = bytes_of(raw, header->AceSize); auto* ace = reinterpret_cast<ACCESS_DENIED_ACE*>(copy.data());
        MapGenericMask(&ace->Mask, &mapping); ace->Header.AceFlags = 0;
        need(AddAce(denies, ACL_REVISION_DS, MAXDWORD, copy.data(), static_cast<DWORD>(copy.size())), "backup_reader_deny_copy_failed");
    }
    BYTE everyone[SECURITY_MAX_SID_SIZE]; DWORD size = sizeof(everyone);
    need(CreateWellKnownSid(WinWorldSid, nullptr, everyone, &size) &&
        AddAccessAllowedAce(denies, ACL_REVISION_DS, wanted, everyone), "backup_reader_deny_rule_failed");
    SECURITY_DESCRIPTOR sd{}; need(InitializeSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION) &&
        SetSecurityDescriptorOwner(&sd, owner, FALSE) && SetSecurityDescriptorDacl(&sd, TRUE, denies, FALSE), "backup_reader_deny_descriptor_failed");
    AUTHZ_RESOURCE_MANAGER_HANDLE manager{}; need(AuthzInitializeResourceManager(AUTHZ_RM_FLAG_NO_AUDIT,
        nullptr, nullptr, nullptr, L"SFX stable backup owner", &manager), "backup_reader_authz_failed");
    struct Manager { AUTHZ_RESOURCE_MANAGER_HANDLE value; ~Manager() { AuthzFreeResourceManager(value); } } managed{manager};
    AUTHZ_CLIENT_CONTEXT_HANDLE context{}; LUID id{};
    need(AuthzInitializeContextFromSid(0, owner, manager, nullptr, id, nullptr, &context), "backup_reader_owner_groups_unavailable");
    struct Context { AUTHZ_CLIENT_CONTEXT_HANDLE value; ~Context() { AuthzFreeContext(value); } } held{context};
    AUTHZ_ACCESS_REQUEST request{}; request.DesiredAccess = wanted; DWORD access{}, error{};
    AUTHZ_ACCESS_REPLY reply{1, &access, nullptr, &error};
    need(AuthzAccessCheck(0, context, &request, nullptr, &sd, nullptr, 0, &reply, nullptr) &&
        error == ERROR_SUCCESS && (access & wanted) == wanted, "backup_reader_owner_deny_preserved");
}
}

BackupReadGrants capture_backup_read_grants(HANDLE object, const std::wstring& owner,
    bool directory, bool inherit_to_child) {
    BY_HANDLE_FILE_INFORMATION identity{};
    need(GetFileInformationByHandle(object, &identity) && !(identity.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
        ((identity.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) == (inherit_to_child || directory) &&
        ((identity.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || identity.nNumberOfLinks == 1),
        "backup_reader_handle_identity_refused");
    PSID actual{}; PACL acl{}; void* raw{};
    need(GetSecurityInfo(object, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
        &actual, nullptr, &acl, nullptr, &raw) == ERROR_SUCCESS, "backup_reader_source_security_unavailable");
    Local descriptor(raw); auto selected = sid_value(owner);
    // A foreign owner/staging object cannot nominate backup readers.
    if (!actual || !EqualSid(actual, selected.get())) return {};
    need(acl && IsValidAcl(acl), "backup_reader_source_dacl_missing");
    (void)acl_entries(acl, {}); // Reject a noncanonical authorization source.
    BackupReadGrants result;
    for (DWORD n = 0; n < acl->AceCount; ++n) {
        void* entry{}; need(GetAce(acl, n, &entry), "backup_reader_source_ace_unavailable");
        auto* header = static_cast<ACE_HEADER*>(entry);
        if (header->AceType != ACCESS_ALLOWED_ACE_TYPE && header->AceType != ACCESS_DENIED_ACE_TYPE)
            throw std::runtime_error("backup_reader_source_ace_requires_review");
        auto* ace = static_cast<ACCESS_ALLOWED_ACE*>(entry); DWORD mask = ace->Mask; MapGenericMask(&mask, &mapping);
        if (header->AceType == ACCESS_ALLOWED_ACE_TYPE &&
            (!stable(&ace->SidStart) || EqualSid(&ace->SidStart, selected.get()) || !mask || (mask & ~read_mask))) continue;
        auto copy = bytes_of(entry, header->AceSize); auto* copied = reinterpret_cast<ACCESS_ALLOWED_ACE*>(copy.data());
        copied->Mask = mask;
        copied->Header.AceFlags &= ~INHERITED_ACE;
        if (inherit_to_child) {
            auto& flags = copied->Header.AceFlags;
            if (!directory) { if (!(flags & OBJECT_INHERIT_ACE)) continue; flags = 0; }
            else {
                if (!(flags & (OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE))) continue;
                if (flags & CONTAINER_INHERIT_ACE) flags &= ~INHERIT_ONLY_ACE;
                else flags |= INHERIT_ONLY_ACE;
                if (flags & NO_PROPAGATE_INHERIT_ACE) flags &= ~(OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE | NO_PROPAGATE_INHERIT_ACE);
            }
        }
        result.push_back(std::move(copy));
    }
    return result;
}
BackupReadGrants capture_backup_read_grants(const std::filesystem::path& path,
    const std::wstring& owner, bool directory, bool inherit) {
    const auto attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        need(GetLastError() == ERROR_FILE_NOT_FOUND || GetLastError() == ERROR_PATH_NOT_FOUND,
            "backup_reader_source_path_unavailable"); return {};
    }
    need(!(attributes & FILE_ATTRIBUTE_REPARSE_POINT), "backup_reader_reparse_source_refused");
    Close held{CreateFileW(path.c_str(), READ_CONTROL | FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_OVERLAPPED | FILE_FLAG_BACKUP_SEMANTICS, nullptr)};
    need(held.value != INVALID_HANDLE_VALUE, "backup_reader_source_open_failed");
    BY_HANDLE_FILE_INFORMATION identity{};
    need(GetFileInformationByHandle(held.value, &identity) &&
        !(identity.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
        ((identity.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) == (inherit || directory) &&
        ((identity.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || identity.nNumberOfLinks == 1),
        "backup_reader_source_identity_refused");
    return capture_backup_read_grants(held.value, owner, directory, inherit);
}
std::vector<unsigned char> merge_backup_read_grants(PACL base, const BackupReadGrants& grants) {
    return acl_entries(base, grants);
}
BackupDaclSnapshot::BackupDaclSnapshot(HANDLE object) {
    void* raw{}; need(GetSecurityInfo(object, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
        nullptr, nullptr, nullptr, nullptr, &raw) == ERROR_SUCCESS, "backup_reader_snapshot_failed");
    Local held(raw); descriptor = bytes_of(raw, GetSecurityDescriptorLength(raw));
}
std::wstring BackupDaclSnapshot::sddl() const {
    wchar_t* value{}; need(ConvertSecurityDescriptorToStringSecurityDescriptorW(
        const_cast<unsigned char*>(descriptor.data()), SDDL_REVISION_1, DACL_SECURITY_INFORMATION,
        &value, nullptr), "backup_reader_snapshot_encoding_failed"); Local held(value); return value;
}
void BackupDaclSnapshot::restore(HANDLE object) const {
    PACL acl{}; BOOL present{}, defaulted{}; auto* sd = const_cast<unsigned char*>(descriptor.data());
    need(GetSecurityDescriptorDacl(sd, &present, &acl, &defaulted) && present && acl,
        "backup_reader_snapshot_invalid"); set_dacl(object, acl, protected_dacl(sd));
}
void ensure_backup_owner_read(HANDLE object, const std::wstring& owner, bool directory, const BackupDaclSnapshot* original) {
    BackupDaclSnapshot before = original ? *original : BackupDaclSnapshot(object);
    auto selected = sid_value(owner); PACL old{}; BOOL present{}, defaulted{};
    need(GetSecurityDescriptorDacl(before.descriptor.data(), &present, &old, &defaulted) && present && old,
        "backup_reader_original_dacl_missing");
    const DWORD mask = directory ? read_mask : FILE_GENERIC_READ;
    verify_owner_denies(old, selected.get(), mask);
    if (!original && backup_owner_has_read(object, owner, directory)) return;
    for (DWORD n = 0; n < old->AceCount; ++n) {
        void* raw{}; need(GetAce(old, n, &raw), "backup_reader_original_ace_unavailable");
        auto* h = static_cast<ACE_HEADER*>(raw);
        if (h->AceType != ACCESS_DENIED_ACE_TYPE) continue;
        auto* denied = static_cast<ACCESS_DENIED_ACE*>(raw); DWORD denied_mask = denied->Mask; MapGenericMask(&denied_mask, &mapping);
        if (!(h->AceFlags & INHERIT_ONLY_ACE) && EqualSid(&denied->SidStart, selected.get()) && (denied_mask & mask))
            throw std::runtime_error("backup_reader_owner_explicit_deny_preserved");
    }
    std::vector<unsigned char> entry(sizeof(ACCESS_ALLOWED_ACE) - sizeof(DWORD) + GetLengthSid(selected.get()));
    auto* ace = reinterpret_cast<ACCESS_ALLOWED_ACE*>(entry.data()); ace->Header.AceType = ACCESS_ALLOWED_ACE_TYPE;
    ace->Header.AceSize = static_cast<WORD>(entry.size()); ace->Mask = mask;
    need(CopySid(GetLengthSid(selected.get()), &ace->SidStart, selected.get()), "backup_reader_owner_copy_failed");
    auto merged = acl_entries(old, {entry}); set_dacl(object, reinterpret_cast<PACL>(merged.data()), protected_dacl(before.descriptor.data()));
}
bool backup_owner_has_read(HANDLE object, const std::wstring& owner, bool directory) {
    BackupDaclSnapshot before(object); auto selected = sid_value(owner); PACL acl{}; BOOL present{}, defaulted{};
    need(GetSecurityDescriptorDacl(before.descriptor.data(), &present, &acl, &defaulted) && present && acl,
        "backup_reader_access_dacl_missing");
    const DWORD wanted = directory ? read_mask : FILE_GENERIC_READ;
    verify_owner_denies(acl, selected.get(), wanted);
    DWORD granted{};
    for (DWORD n = 0; n < acl->AceCount; ++n) {
        void* raw{}; need(GetAce(acl, n, &raw), "backup_reader_access_ace_unavailable");
        auto* ace = static_cast<ACCESS_ALLOWED_ACE*>(raw);
        if (ace->Header.AceType == ACCESS_ALLOWED_ACE_TYPE && !(ace->Header.AceFlags & INHERIT_ONLY_ACE) &&
            EqualSid(&ace->SidStart, selected.get())) { DWORD mask = ace->Mask; MapGenericMask(&mask, &mapping); granted |= mask; }
    }
    return (granted & wanted) == wanted;
}
bool backup_boundary_with_readers(HANDLE object, void* expected, const std::wstring& owner) {
    PSID actual_owner{}; PACL actual{}, base{}; void* raw{}; BOOL present{}, defaulted{};
    need(GetSecurityInfo(object, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
        &actual_owner, nullptr, &actual, nullptr, &raw) == ERROR_SUCCESS, "backup_reader_boundary_unavailable");
    Local held(raw); auto selected = sid_value(owner);
    if (!actual_owner || !EqualSid(actual_owner, selected.get()) || !protected_dacl(raw) ||
        !GetSecurityDescriptorDacl(expected, &present, &base, &defaulted) || !present || !base || !actual) return false;
    const auto readers = capture_backup_read_grants(object, owner, true);
    auto merged = acl_entries(base, readers); auto* allowed = reinterpret_cast<PACL>(merged.data());
    if (actual->AceCount != allowed->AceCount) return false;
    std::vector<bool> consumed(allowed->AceCount,false);
    for (DWORD n = 0; n < actual->AceCount; ++n) {
        void* entry{}; need(GetAce(actual, n, &entry), "backup_reader_boundary_ace_unavailable");
        const auto bytes = bytes_of(entry, static_cast<ACE_HEADER*>(entry)->AceSize);
        bool found{};
        for (DWORD k = 0; k < allowed->AceCount; ++k) {
            if(consumed[k])continue;
            void* candidate{}; need(GetAce(allowed, k, &candidate), "backup_reader_boundary_rule_unavailable");
            if (bytes == bytes_of(candidate, static_cast<ACE_HEADER*>(candidate)->AceSize)) { consumed[k]=true;found = true; break; }
        }
        if (!found) return false;
    }
    verify_owner_denies(actual, selected.get(), FILE_GENERIC_READ | FILE_GENERIC_WRITE | FILE_GENERIC_EXECUTE | DELETE | FILE_DELETE_CHILD);
    return true;
}
BackupPublicationAccess capture_backup_publication_access(const std::filesystem::path& previous,
    const std::wstring& owner, bool directory) {
    const auto attributes = GetFileAttributesW(previous.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        const auto error = GetLastError(); need(error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND,
            "backup_reader_prior_unavailable"); return {};
    }
    Close held{CreateFileW(previous.c_str(), READ_CONTROL | FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_OVERLAPPED | FILE_FLAG_BACKUP_SEMANTICS, nullptr)};
    need(held.value != INVALID_HANDLE_VALUE, "backup_reader_prior_open_failed");
    BY_HANDLE_FILE_INFORMATION identity{};
    need(GetFileInformationByHandle(held.value, &identity) && !(identity.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
        ((identity.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) == directory &&
        (directory || identity.nNumberOfLinks == 1), "backup_reader_prior_identity_refused");
    BackupDaclSnapshot before(held.value);
    return {capture_backup_read_grants(held.value, owner, directory), !protected_dacl(before.descriptor.data())};
}
void apply_backup_read_grants(const std::filesystem::path& destination,
    const BackupReadGrants& grants, bool directory) {
    if (grants.empty()) return;
    Close held{CreateFileW(destination.c_str(), READ_CONTROL | WRITE_DAC, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_OVERLAPPED | FILE_FLAG_BACKUP_SEMANTICS, nullptr)};
    need(held.value != INVALID_HANDLE_VALUE, "backup_reader_destination_open_failed");
    BY_HANDLE_FILE_INFORMATION identity{};
    need(GetFileInformationByHandle(held.value, &identity) && !(identity.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
        ((identity.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) == directory && (directory || identity.nNumberOfLinks == 1),
        "backup_reader_destination_reparse_refused");
    BackupDaclSnapshot before(held.value); PACL acl{}; BOOL present{}, defaulted{};
    need(GetSecurityDescriptorDacl(before.descriptor.data(), &present, &acl, &defaulted) && present && acl,
        "backup_reader_destination_dacl_missing");
    auto merged = acl_entries(acl, grants); set_dacl(held.value, reinterpret_cast<PACL>(merged.data()), protected_dacl(before.descriptor.data()));
}
void carry_backup_read_grants(const std::filesystem::path& previous,
    const std::filesystem::path& destination, const std::wstring& owner) {
    apply_backup_read_grants(destination, capture_backup_read_grants(previous, owner, false));
}
} // namespace sfx::platform::win32
