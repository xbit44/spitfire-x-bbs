#pragma once

#include "sfx/adapters/storage/sqlite_storage.hpp"

#include <string>

namespace sfx::platform::win32 {

class WindowsLocalStoragePathPolicy final : public adapters::storage::IStoragePathPolicy {
public:
    explicit WindowsLocalStoragePathPolicy(std::wstring installation_owner_sid = {});

    adapters::storage::StorageLocationAssessment assess(
        const std::filesystem::path& directory) const override;
    application::StorageStatus create_database_file_exclusive(
        const std::filesystem::path& path) const override;
    application::StorageStatus protect_and_verify_database_file(
        const std::filesystem::path& path) const override;

private:
    std::wstring installation_owner_sid_;
};

}  // namespace sfx::platform::win32
