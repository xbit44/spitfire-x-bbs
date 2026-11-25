#include "sfx/platform/win32/storage_path_policy.hpp"
#include "sfx/platform/win32/storage_dacl_policy.hpp"

#include <windows.h>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <memory>
#include <cwctype>
#include <string>
#include <vector>

namespace sfx::platform::win32 {
namespace {

using application::StorageErrorCode;
using application::StorageStatus;

adapters::storage::StorageLocationAssessment reject(
    const StorageErrorCode code,
    std::string operation) {
    return {{code, std::move(operation)}, {}};
}

std::wstring canonical_windows_path(const std::filesystem::path& path) {
    std::vector<wchar_t> buffer(32768U);
    const DWORD length = GetFullPathNameW(
        path.c_str(),
        static_cast<DWORD>(buffer.size()),
        buffer.data(),
        nullptr);
    if (length == 0 || length >= buffer.size()) {
        return {};
    }
    return std::wstring(buffer.data(), length);
}

std::wstring lower_path(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(), [](const wchar_t character) {
        return static_cast<wchar_t>(std::towlower(character));
    });
    while (value.size() > 3U && (value.back() == L'\\' || value.back() == L'/')) {
        value.pop_back();
    }
    return value;
}

bool path_is_within(const std::wstring& candidate, const std::wstring& root) {
    const std::wstring normalized_candidate = lower_path(candidate);
    const std::wstring normalized_root = lower_path(root);
    if (normalized_candidate == normalized_root) {
        return true;
    }
    return normalized_candidate.size() > normalized_root.size() &&
           normalized_candidate.compare(0, normalized_root.size(), normalized_root) == 0 &&
           (normalized_candidate[normalized_root.size()] == L'\\' ||
            normalized_candidate[normalized_root.size()] == L'/');
}

bool is_known_sync_root(const std::wstring& candidate) {
    constexpr const wchar_t* variables[] = {L"OneDrive", L"OneDriveConsumer", L"OneDriveCommercial"};
    for (const wchar_t* const variable : variables) {
        const DWORD required = GetEnvironmentVariableW(variable, nullptr, 0);
        if (required == 0) {
            continue;
        }
        std::vector<wchar_t> value(static_cast<std::size_t>(required));
        if (GetEnvironmentVariableW(variable, value.data(), required) == 0) {
            continue;
        }
        const std::wstring root = canonical_windows_path(std::filesystem::path(value.data()));
        if (!root.empty() && path_is_within(candidate, root)) {
            return true;
        }
    }
    return false;
}

bool has_reparse_component(const std::filesystem::path& directory) {
    std::filesystem::path current = directory;
    for (;;) {
        const DWORD attributes = GetFileAttributesW(current.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES ||
            (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
            return true;
        }
        const std::filesystem::path parent = current.parent_path();
        if (parent.empty() || parent == current) {
            break;
        }
        current = parent;
    }
    return false;
}

using UniqueHandle = std::unique_ptr<void, decltype(&CloseHandle)>;

UniqueHandle owned_handle(const HANDLE handle) {
    return UniqueHandle(handle == INVALID_HANDLE_VALUE ? nullptr : handle, &CloseHandle);
}

StorageStatus io_failure(std::string operation) {
    return {StorageErrorCode::io, std::move(operation)};
}

}  // namespace

WindowsLocalStoragePathPolicy::WindowsLocalStoragePathPolicy(
    std::wstring installation_owner_sid)
    : installation_owner_sid_(std::move(installation_owner_sid)) {}

adapters::storage::StorageLocationAssessment WindowsLocalStoragePathPolicy::assess(
    const std::filesystem::path& directory) const {
    if (directory.empty() || !directory.is_absolute()) {
        return reject(StorageErrorCode::invalid_location, "require absolute local storage directory");
    }
    const std::wstring input = directory.native();
    if (input.rfind(L"\\\\", 0) == 0 || input.rfind(L"\\\\?\\", 0) == 0 ||
        input.rfind(L"\\\\.\\", 0) == 0) {
        return reject(StorageErrorCode::invalid_location, "reject UNC or device storage path");
    }

    const std::wstring canonical = canonical_windows_path(directory);
    if (canonical.empty() || canonical.rfind(L"\\\\", 0) == 0) {
        return reject(StorageErrorCode::invalid_location, "canonicalize local storage directory");
    }
    const std::filesystem::path canonical_path(canonical);
    const DWORD attributes = GetFileAttributesW(canonical_path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        return reject(StorageErrorCode::invalid_location, "require existing storage directory");
    }
    if (has_reparse_component(canonical_path)) {
        return reject(StorageErrorCode::invalid_location, "reject reparse-point storage path");
    }
    if (is_known_sync_root(canonical)) {
        return reject(StorageErrorCode::invalid_location, "reject synchronized storage directory");
    }

    std::vector<wchar_t> volume_root(MAX_PATH + 1U);
    if (!GetVolumePathNameW(canonical_path.c_str(), volume_root.data(), static_cast<DWORD>(volume_root.size()))) {
        return reject(StorageErrorCode::invalid_location, "resolve storage volume");
    }
    if (GetDriveTypeW(volume_root.data()) != DRIVE_FIXED) {
        return reject(StorageErrorCode::invalid_location, "require fixed local storage volume");
    }

    std::vector<wchar_t> filesystem_name(MAX_PATH + 1U);
    if (!GetVolumeInformationW(
            volume_root.data(),
            nullptr,
            0,
            nullptr,
            nullptr,
            nullptr,
            filesystem_name.data(),
            static_cast<DWORD>(filesystem_name.size()))) {
        return reject(StorageErrorCode::invalid_location, "inspect storage filesystem");
    }
    if (_wcsicmp(filesystem_name.data(), L"NTFS") != 0) {
        return reject(StorageErrorCode::invalid_location, "require NTFS storage volume");
    }

    return {StorageStatus::success(), canonical_path};
}

StorageStatus WindowsLocalStoragePathPolicy::create_database_file_exclusive(
    const std::filesystem::path& path) const {
    const UniqueHandle file = owned_handle(CreateFileW(
        path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!file) {
        const DWORD error = GetLastError();
            return {error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS
                ? StorageErrorCode::conflict : StorageErrorCode::io,
            "create exclusive SFX database file"};
    }
    if (!FlushFileBuffers(file.get())) return io_failure("flush exclusive SFX database file");
    return StorageStatus::success();
}

StorageStatus WindowsLocalStoragePathPolicy::protect_and_verify_database_file(
    const std::filesystem::path& path) const {
    if (installation_owner_sid_.empty()) {
        return {StorageErrorCode::permission_denied,
            "require configured installation-owner SID for SFX database"};
    }
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES ||
        (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        return {StorageErrorCode::invalid_location,
            "reject missing or reparse-point SFX database"};
    }
    const WindowsStorageDaclPolicy policy(installation_owner_sid_);
    const StorageStatus applied = policy.apply(path, StorageDaclObjectKind::file);
    if (!applied) return applied;
    const StorageDaclAssessment assessment = policy.assess(path, StorageDaclObjectKind::file);
    if (!assessment.status || !assessment.private_access || !assessment.effective_access) {
        return {StorageErrorCode::permission_denied,
            "verify SFX database owner and protected DACL"};
    }
    return StorageStatus::success();
}

}  // namespace sfx::platform::win32
