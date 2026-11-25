#include "sfx/platform/win32/storage_dacl_policy.hpp"
#include "sfx/platform/win32/service_file_owner.hpp"
#include "sfx/platform/win32/storage_access.hpp"
#include "sfx/platform/win32/backup_read_access.hpp"

#include <windows.h>
#include <sddl.h>
#include <Aclapi.h>

#include <algorithm>
#include <cstddef>
#include <memory>
#include <utility>
#include <vector>

namespace sfx::platform::win32 {
namespace {

using application::StorageErrorCode;
using application::StorageStatus;

struct LocalFreeDeleter {
    void operator()(void* const value) const noexcept {
        if (value != nullptr) LocalFree(value);
    }
};

using LocalPointer = std::unique_ptr<void, LocalFreeDeleter>;

StorageStatus permission_failure(std::string operation) {
    return {StorageErrorCode::permission_denied, std::move(operation)};
}

std::vector<std::byte> sid_from_string(const std::wstring& value) {
    PSID converted = nullptr;
    if (value.empty() || !ConvertStringSidToSidW(value.c_str(), &converted)) {
        return {};
    }
    LocalPointer holder(converted);
    if (!IsValidSid(converted)) {
        return {};
    }
    const DWORD length = GetLengthSid(converted);
    std::vector<std::byte> result(length);
    if (!CopySid(length, result.data(), converted)) {
        return {};
    }
    return result;
}

std::vector<std::byte> well_known_sid(const WELL_KNOWN_SID_TYPE type) {
    DWORD length = SECURITY_MAX_SID_SIZE;
    std::vector<std::byte> result(length);
    if (!CreateWellKnownSid(type, nullptr, result.data(), &length)) {
        return {};
    }
    result.resize(length);
    return result;
}

bool same_sid(const std::vector<std::byte>& left, const std::vector<std::byte>& right) {
    return !left.empty() && !right.empty() &&
           EqualSid(
               const_cast<std::byte*>(left.data()),
               const_cast<std::byte*>(right.data())) != FALSE;
}

bool is_forbidden_broad_runtime_sid(const std::vector<std::byte>& runtime) {
    // The locked Desktop policy explicitly rejects the shared LocalService SID;
    // it is never an allowed runtime or owner principal.
    if (same_sid(runtime, well_known_sid(WinLocalServiceSid))) {
        return true;
    }
    constexpr WELL_KNOWN_SID_TYPE forbidden[] = {
        WinWorldSid,
        WinAnonymousSid,
        WinAuthenticatedUserSid,
        WinBuiltinUsersSid,
        WinBuiltinGuestsSid,
        WinBuiltinAdministratorsSid,
    };
    for (const WELL_KNOWN_SID_TYPE type : forbidden) {
        const std::vector<std::byte> candidate = well_known_sid(type);
        if (same_sid(runtime, candidate)) return true;
    }
    return false;
}

std::vector<std::vector<std::byte>> required_sids(const std::wstring& runtime_sid_string) {
    std::vector<std::vector<std::byte>> result;
    result.push_back(sid_from_string(runtime_sid_string));
    if (result.front().empty() || is_forbidden_broad_runtime_sid(result.front())) {
        return {};
    }

    for (const WELL_KNOWN_SID_TYPE type : {WinLocalSystemSid, WinBuiltinAdministratorsSid}) {
        std::vector<std::byte> sid = well_known_sid(type);
        if (sid.empty()) return {};
        const bool duplicate = std::any_of(result.begin(), result.end(), [&](const auto& existing) {
            return same_sid(existing, sid);
        });
        if (!duplicate) result.push_back(std::move(sid));
    }
    return result;
}

BYTE inheritance_flags(const StorageDaclObjectKind kind) {
    return kind == StorageDaclObjectKind::directory
        ? static_cast<BYTE>(CONTAINER_INHERIT_ACE | OBJECT_INHERIT_ACE)
        : static_cast<BYTE>(0);
}

DWORD runtime_access_mask(const StorageDaclObjectKind kind) {
    constexpr DWORD common = FILE_GENERIC_READ | FILE_GENERIC_WRITE | DELETE;
    return kind == StorageDaclObjectKind::directory
        ? common | FILE_GENERIC_EXECUTE | FILE_DELETE_CHILD
        : common;
}

bool is_system_or_builtin_administrators(const std::vector<std::byte>& sid) {
    for (const WELL_KNOWN_SID_TYPE type : {WinLocalSystemSid, WinBuiltinAdministratorsSid}) {
        if (same_sid(sid, well_known_sid(type))) return true;
    }
    return false;
}

DWORD required_access_mask(
    const std::vector<std::byte>& sid,
    const StorageDaclObjectKind kind) {
    return is_system_or_builtin_administrators(sid)
        ? FILE_ALL_ACCESS
        : runtime_access_mask(kind);
}

bool path_matches_kind(const std::filesystem::path& path, const StorageDaclObjectKind kind) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) return false;
    const bool is_directory = (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    return is_directory == (kind == StorageDaclObjectKind::directory);
}

}  // namespace

WindowsStorageDaclPolicy::WindowsStorageDaclPolicy(std::wstring runtime_identity_sid)
    : runtime_identity_sid_(std::move(runtime_identity_sid)) {}

StorageStatus WindowsStorageDaclPolicy::apply(const std::filesystem::path& path,
    const StorageDaclObjectKind kind) const {
    return apply(path, kind, true, false, true);
}

StorageStatus WindowsStorageDaclPolicy::apply(
    const std::filesystem::path& path,
    const StorageDaclObjectKind kind,
    const bool preserve_object_read_grants,
    const bool new_object_inherits_read_grants,
    const bool respect_current_inheritance) const {
    if (!path.is_absolute() || !path_matches_kind(path, kind)) {
        return {StorageErrorCode::invalid_location, "apply private storage DACL to existing object"};
    }
    const std::vector<std::vector<std::byte>> sids = required_sids(runtime_identity_sid_);
    if (sids.empty()) {
        return permission_failure("validate private storage runtime SID");
    }
    // Native key writers may leave only the recorded owner in their DACL.
    // Attested service metadata authority must be scoped BEFORE any ACL read.
    // Ordinary Desktop/caller tokens gain no privilege through this scope.
    ServiceFileOwnerScope owner_scope(runtime_identity_sid_,true);
    if(!owner_scope.valid())return permission_failure("private storage owner scope");

    DWORD acl_bytes = sizeof(ACL);
    for (const auto& sid : sids) {
        acl_bytes += static_cast<DWORD>(
            sizeof(ACCESS_ALLOWED_ACE) - sizeof(DWORD) +
            GetLengthSid(const_cast<std::byte*>(sid.data())));
    }
    std::vector<std::byte> acl_storage(acl_bytes);
    auto* acl = reinterpret_cast<PACL>(acl_storage.data());
    if (!InitializeAcl(acl, acl_bytes, ACL_REVISION)) {
        return permission_failure("initialize private storage DACL");
    }
    for (const auto& sid : sids) {
        if (!AddAccessAllowedAceEx(
                acl,
                ACL_REVISION,
                inheritance_flags(kind),
                required_access_mask(sid, kind),
                const_cast<std::byte*>(sid.data()))) {
            return permission_failure("build private storage DACL");
        }
    }

    // Parent grants are a Sysop-selected policy for descendants. Existing
    // object grants are carried only at owner-controlled managed boundaries;
    // external imports explicitly opt out of trusting their source ACLs.
    std::vector<unsigned char> reader_acl;
    try {
        BackupReadGrants readers;
        // An existing protected target may intentionally exclude a parent
        // reader. Only an actual new-object publication may opt into it.
        const auto held = CreateFileW(path.c_str(), READ_CONTROL, FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        if (held == INVALID_HANDLE_VALUE) return permission_failure("read backup-reader inheritance boundary");
        const std::unique_ptr<void, decltype(&CloseHandle)> boundary_pin(held, CloseHandle);
        BackupDaclSnapshot boundary(held);
        SECURITY_DESCRIPTOR_CONTROL control{}; DWORD revision{};
        if (!GetSecurityDescriptorControl(boundary.descriptor.data(), &control, &revision))
            return permission_failure("read backup-reader inheritance state");
        if (new_object_inherits_read_grants || (respect_current_inheritance && !(control & SE_DACL_PROTECTED)))
            readers = capture_backup_read_grants(path.parent_path(), runtime_identity_sid_,
                kind == StorageDaclObjectKind::directory, true);
        if (preserve_object_read_grants) {
            auto existing = capture_backup_read_grants(path, runtime_identity_sid_,
                kind == StorageDaclObjectKind::directory);
            readers.insert(readers.end(), existing.begin(), existing.end());
        }
        reader_acl = merge_backup_read_grants(acl, readers);
        acl = reinterpret_cast<PACL>(reader_acl.data());
    } catch (const std::exception&) {
        return permission_failure("preserve trusted Windows backup-reader access");
    }

    std::wstring native_path = path.native();
    // Some native key writers replace their file ACL with owner-only access.
    // The attested System service restores the exact accepted owner+ACL with
    // its temporary metadata privilege, without first requiring that removed
    // System read grant. Ordinary Desktop retains its existing owner check.
    if(owner_scope.active()) {
        const auto restored=SetNamedSecurityInfoW(native_path.data(),SE_FILE_OBJECT,
            OWNER_SECURITY_INFORMATION|DACL_SECURITY_INFORMATION|PROTECTED_DACL_SECURITY_INFORMATION,
            const_cast<std::byte*>(sids.front().data()),nullptr,acl,nullptr);
        if(restored!=ERROR_SUCCESS)return permission_failure("restore private storage owner and DACL");
        return assess(path,kind).status;
    }
    PSID existing_owner = nullptr;
    PSECURITY_DESCRIPTOR existing_descriptor = nullptr;
    const DWORD owner_result = GetNamedSecurityInfoW(
        native_path.data(),
        SE_FILE_OBJECT,
        OWNER_SECURITY_INFORMATION,
        &existing_owner,
        nullptr,
        nullptr,
        nullptr,
        &existing_descriptor);
    LocalPointer existing_descriptor_holder(existing_descriptor);
    if (owner_result != ERROR_SUCCESS || existing_descriptor == nullptr ||
        existing_owner == nullptr) {
        return permission_failure("read private storage owner before DACL replacement");
    }
    const bool owner_already_matches =
        EqualSid(existing_owner, const_cast<std::byte*>(sids.front().data())) != FALSE;
    SECURITY_INFORMATION security_information =
        DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION;
    if (!owner_already_matches) security_information |= OWNER_SECURITY_INFORMATION;
    const DWORD result = SetNamedSecurityInfoW(
        native_path.data(),
        SE_FILE_OBJECT,
        security_information,
        owner_already_matches ? nullptr : const_cast<std::byte*>(sids.front().data()),
        nullptr,
        acl,
        nullptr);
    if (result != ERROR_SUCCESS) {
        return permission_failure("apply private storage DACL");
    }
    return assess(path, kind).status;
}

StorageStatus WindowsStorageDaclPolicy::apply_for_publication(
    const std::filesystem::path& destination, const StorageDaclObjectKind kind,
    const std::filesystem::path& trusted_previous) const {
    try {
        ServiceFileOwnerScope metadata_scope(runtime_identity_sid_,true);
        if(!metadata_scope.valid())return permission_failure("private publication metadata scope");
        const auto access = capture_backup_publication_access(trusted_previous,
            runtime_identity_sid_, kind == StorageDaclObjectKind::directory);
        const auto applied = apply(destination, kind, false, access.inherit_parent, false);
        if (!applied) return applied;
        apply_backup_read_grants(destination, access.grants, kind == StorageDaclObjectKind::directory);
        return assess(destination, kind).status;
    } catch (const std::exception&) {
        return permission_failure("preserve prior Windows backup-reader publication boundary");
    }
}

StorageDaclAssessment WindowsStorageDaclPolicy::assess(
    const std::filesystem::path& path, const StorageDaclObjectKind kind,
    const bool allow_private_inherited_object) const {
    (void)allow_private_inherited_object;
    if (runtime_identity_sid_.empty()) return {permission_failure("storage identity unavailable"),false,false};
    return assess_current_storage_access(path,kind);
}

std::wstring current_process_user_sid_string() {
    HANDLE raw_token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw_token)) return {};
    const auto close_token = [](void* value) {
        if (value != nullptr) CloseHandle(value);
    };
    const std::unique_ptr<void, decltype(close_token)> token(raw_token, close_token);

    DWORD bytes = 0;
    (void)GetTokenInformation(raw_token, TokenUser, nullptr, 0, &bytes);
    if (bytes == 0 || GetLastError() != ERROR_INSUFFICIENT_BUFFER) return {};
    std::vector<std::byte> buffer(bytes);
    if (!GetTokenInformation(raw_token, TokenUser, buffer.data(), bytes, &bytes)) return {};
    const auto* const token_user = reinterpret_cast<const TOKEN_USER*>(buffer.data());
    wchar_t* converted = nullptr;
    if (!ConvertSidToStringSidW(token_user->User.Sid, &converted)) return {};
    LocalPointer holder(converted);
    return converted;
}

}  // namespace sfx::platform::win32
