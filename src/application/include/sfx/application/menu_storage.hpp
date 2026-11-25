#pragma once
#include "sfx/application/account_storage.hpp"
#include "sfx/application/menu.hpp"

namespace sfx::application {
struct MenuPreferenceRecord {
    AccountId account_id{};
    MenuPreferences preferences;
    std::int64_t revision{};
    std::int64_t modified_utc_seconds{};
};
struct UpdateMenuPreferenceRequest {
    AccountId account_id{};
    std::int64_t expected_revision{};
    MenuPreferences preferences;
    std::int64_t modified_utc_seconds{};
};
class IMenuPreferenceStorage {
public:
    virtual ~IMenuPreferenceStorage() = default;
    virtual StorageResult<MenuPreferenceRecord> menu_preferences(AccountId account_id) = 0;
    virtual MutationResult update_menu_preferences(const UpdateMenuPreferenceRequest& request) = 0;
};
} // namespace sfx::application
