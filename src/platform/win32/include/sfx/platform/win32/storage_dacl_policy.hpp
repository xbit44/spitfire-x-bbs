#pragma once

#include "sfx/application/storage.hpp"

#include <filesystem>
#include <string>

namespace sfx::platform::win32 {

enum class StorageDaclObjectKind {
    directory,
    file,
};

struct StorageDaclAssessment {
    application::StorageStatus status;
    bool private_access{false};
    bool effective_access{false};
};

// Explicit creation/preparation applies private defaults for the current host.
// Assessment checks actual current access and private exposure, without an old
// owner or exact historical/inherited ACE-layout condition.
class WindowsStorageDaclPolicy final {
public:
    explicit WindowsStorageDaclPolicy(std::wstring runtime_identity_sid);

    application::StorageStatus apply(
        const std::filesystem::path& path,
        StorageDaclObjectKind kind) const;

    application::StorageStatus apply(
        const std::filesystem::path& path, StorageDaclObjectKind kind,
        bool preserve_object_read_grants,
        bool new_object_inherits_read_grants = false,
        bool respect_current_inheritance = true) const;

    // The logical prior object determines exclusions, not the temporary file.
    application::StorageStatus apply_for_publication(
        const std::filesystem::path& destination, StorageDaclObjectKind kind,
        const std::filesystem::path& trusted_previous) const;

    StorageDaclAssessment assess(
        const std::filesystem::path& path,
        StorageDaclObjectKind kind,
        // Compatibility parameter; harmless private inheritance is accepted.
        // Actual current-token access and private protection are still checked.
        bool allow_private_inherited_object = false) const;

private:
    std::wstring runtime_identity_sid_;
};

// Returns the user SID carried by the current process token. This qualifies
// Desktop-mode policy mechanics without selecting a product identity model.
std::wstring current_process_user_sid_string();

}  // namespace sfx::platform::win32
