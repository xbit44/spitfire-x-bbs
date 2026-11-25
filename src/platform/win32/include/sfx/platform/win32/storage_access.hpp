#pragma once
#include "sfx/platform/win32/storage_dacl_policy.hpp"
namespace sfx::platform::win32 {
// Read-only current-token access/private-exposure assessment. No owner-SID,
// inheritance layout or exact ACE-count condition and no permission repair.
StorageDaclAssessment assess_current_storage_access(const std::filesystem::path&,StorageDaclObjectKind);
// Programs/structural directories need current read/execute and no shared
// write/delete/security modification. Harmless shared read is not secret exposure.
StorageDaclAssessment assess_current_program_access(const std::filesystem::path&,StorageDaclObjectKind);
}
