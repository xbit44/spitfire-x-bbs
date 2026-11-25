#pragma once

#include <filesystem>
#include <fstream>
#include <map>

#include "sfx/adapters/storage/sqlite_storage.hpp"
#include "sfx/application/storage.hpp"

namespace sfx::test {

class FakeStorageRuntime final : public application::IStorageRuntime {
public:
    application::StorageStatus start_result;
    application::StorageStatus checkpoint_result;
    application::StorageStatus shutdown_result;
    application::StorageHealth current_health;
    std::uint32_t start_calls{0};
    std::uint32_t checkpoint_calls{0};
    std::uint32_t shutdown_calls{0};

    application::StorageStatus start() override {
        ++start_calls;
        if (start_result) current_health.state = application::StorageRuntimeState::ready;
        return start_result;
    }

    application::StorageStatus checkpoint() override {
        ++checkpoint_calls;
        return checkpoint_result;
    }

    application::StorageStatus shutdown() override {
        ++shutdown_calls;
        if (shutdown_result) current_health.state = application::StorageRuntimeState::stopped;
        return shutdown_result;
    }

    application::StorageHealth health() const override { return current_health; }
};

class FakeStoragePathPolicy final : public adapters::storage::IStoragePathPolicy {
public:
    bool accept{true};
    mutable std::string denied_protection_filename;
    mutable std::filesystem::path assessed_directory;
    std::filesystem::path canonical_directory;

    adapters::storage::StorageLocationAssessment assess(
        const std::filesystem::path& directory) const override {
        assessed_directory = directory;
        if (!accept) {
            return {{application::StorageErrorCode::invalid_location, "fake rejected storage location"}, {}};
        }
        return {
            application::StorageStatus::success(),
            canonical_directory.empty() ? directory : canonical_directory};
    }

    application::StorageStatus create_database_file_exclusive(
        const std::filesystem::path& path) const override {
        if (std::filesystem::exists(path)) {
            return {application::StorageErrorCode::conflict,
                "fake create exclusive SFX database"};
        }
        std::ofstream output(path, std::ios::binary | std::ios::out);
        if (!output.good()) {
            return {application::StorageErrorCode::io,
                "fake create exclusive SFX database"};
        }
        return application::StorageStatus::success();
    }

    application::StorageStatus protect_and_verify_database_file(
        const std::filesystem::path& path) const override {
        if (!denied_protection_filename.empty() &&
            path.filename().u8string() == denied_protection_filename) {
            return {application::StorageErrorCode::permission_denied,
                "fake injected SFX database permission failure"};
        }
        if (!std::filesystem::exists(path)) {
            return {application::StorageErrorCode::permission_denied,
                "fake verify protected SFX database"};
        }
        return application::StorageStatus::success();
    }
};

}  // namespace sfx::test
