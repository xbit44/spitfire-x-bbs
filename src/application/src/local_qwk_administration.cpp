#include "sfx/application/local_qwk_administration.hpp"
namespace sfx::application {
namespace {
LocalAccountResultCode qwk_code(StorageErrorCode code){
    if(code==StorageErrorCode::conflict)return LocalAccountResultCode::conflict;
    if(code==StorageErrorCode::invalid_argument)return LocalAccountResultCode::invalid_request;
    if(code==StorageErrorCode::permission_denied)return LocalAccountResultCode::policy_rejected;
    return LocalAccountResultCode::storage_failure;
}
}
bool valid_qwk_policy_change(const QwkPolicyChange& c)noexcept{return c.operation.valid()&&c.expected_revision>0&&valid_qwk_board_id(c.board_id)&&valid_qwk_message_count(c.maximum_messages);}
LocalQwkPolicyResult LocalQwkAdministration::snapshot(){
    if(authority_!=LocalAccountAuthority::installation_owner)return {LocalAccountResultCode::unauthorized};
    const auto result=storage_.qwk_policy({true,{}});if(!result||!result.value)return {qwk_code(result.status.code)};return {LocalAccountResultCode::ok,*result.value};
}
LocalQwkPolicyResult LocalQwkAdministration::execute(const QwkPolicyChange& c){
    if(authority_!=LocalAccountAuthority::installation_owner)return {LocalAccountResultCode::unauthorized};
    if(!valid_qwk_policy_change(c))return {LocalAccountResultCode::invalid_request};
    const auto result=storage_.save_qwk_policy({true,{}},c.operation,c.expected_revision,c.board_id,c.maximum_messages);if(!result||!result.value)return {qwk_code(result.status.code)};return {LocalAccountResultCode::ok,*result.value};
}
} // namespace sfx::application
