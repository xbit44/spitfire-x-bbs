#pragma once
#include "sfx/application/message_storage.hpp"
#include "sfx/application/local_account_administration.hpp"

namespace sfx::application {
struct LocalMessageConfigurationQuery {
    std::optional<MessageGroupId> group;
    MessageCatalogCursor cursor;
};
struct LocalMessageConfigurationSnapshot {
    LocalAccountResultCode code{LocalAccountResultCode::unavailable};
    MessageConfigurationSnapshot catalog{};
};
struct LocalMessageConfigurationResult {
    LocalAccountResultCode code{LocalAccountResultCode::unavailable};
    MessageConfigurationResult result{};
};
// Authority comes only from the protected local host. No payload may carry it.
class LocalMessageAdministration final {
public:
    LocalMessageAdministration(IMessageStorage& storage,LocalAccountAuthority authority):storage_(storage),authority_(authority) {}
    LocalMessageConfigurationSnapshot snapshot(const LocalMessageConfigurationQuery&);
    LocalMessageConfigurationResult execute(const MessageConfigurationChange&);
private:
    IMessageStorage& storage_;
    LocalAccountAuthority authority_;
};
}
