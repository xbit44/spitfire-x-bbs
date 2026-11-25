#include "sfx/application/local_classic_information.hpp"
namespace sfx::application {
LocalClassicInformationSnapshot LocalClassicInformationAdministration::snapshot(std::int64_t after) {
    if(authority_!=LocalAccountAuthority::installation_owner)return {LocalAccountResultCode::unauthorized};
    if(after<0 || after>99999)return {LocalAccountResultCode::invalid_request};
    const auto page=storage_.other_boards(after);const auto date=storage_.opening_date();
    if(!page || !page.value || !date || !date.value)return {};
    return {LocalAccountResultCode::ok,*page.value,*date.value};
}
LocalAccountResult LocalClassicInformationAdministration::execute(const ClassicInformationChange& change) {
    if(authority_!=LocalAccountAuthority::installation_owner)return {LocalAccountResultCode::unauthorized};
    if(!valid_classic_information_change(change))return {LocalAccountResultCode::invalid_request};
    const auto result=storage_.change_classic_information(change,{true,0,0,clock_.unix_seconds(),identifiers_.generate()});
    if(result)return {LocalAccountResultCode::ok,true,false,change.expected_revision+1};
    return {result.code==StorageErrorCode::conflict?LocalAccountResultCode::conflict:LocalAccountResultCode::storage_failure};
}
} // namespace sfx::application
