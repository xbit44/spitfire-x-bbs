#pragma once
#include "sfx/application/classic_information_storage.hpp"
#include "sfx/application/local_account_administration.hpp"

namespace sfx::application {
struct LocalClassicInformationSnapshot {
    LocalAccountResultCode code{LocalAccountResultCode::unavailable};
    OtherBoardPage boards{};
    OpeningDateRecord opening{};
};
class LocalClassicInformationAdministration final {
public:
    LocalClassicInformationAdministration(IClassicInformationStorage& storage,IIdentifierGenerator& identifiers,IClock& clock,LocalAccountAuthority authority)
        :storage_(storage),identifiers_(identifiers),clock_(clock),authority_(authority) {}
    LocalClassicInformationSnapshot snapshot(std::int64_t after);
    LocalAccountResult execute(const ClassicInformationChange&);
private:
    IClassicInformationStorage& storage_;
    IIdentifierGenerator& identifiers_;
    IClock& clock_;
    LocalAccountAuthority authority_;
};
} // namespace sfx::application
