#include "sfx/application/local_message_administration.hpp"
#include <algorithm>
#include <set>

namespace sfx::application {
namespace {
bool name_valid(std::string_view s){return !s.empty() && s.size()<=64 && s.front()!=' ' && s.back()!=' ' &&
    std::all_of(s.begin(),s.end(),[](unsigned char c){return c>=32 && c<=126;});}
bool minimum(std::uint16_t n){return n>=1 && n<=999;}
LocalAccountResultCode result_code(StorageErrorCode code){
    if(code==StorageErrorCode::conflict)return LocalAccountResultCode::conflict;
    if(code==StorageErrorCode::invalid_argument)return LocalAccountResultCode::invalid_request;
    if(code==StorageErrorCode::permission_denied)return LocalAccountResultCode::policy_rejected;
    return LocalAccountResultCode::storage_failure;
}
}
bool valid_message_catalog_cursor(const MessageCatalogCursor& c){return c.configuration_revision>=0 && c.after_order>=-1 && c.after_id>=0 && c.page_size>=1 && c.page_size<=kMessagePageMaximum;}
bool valid_message_configuration_change(const MessageConfigurationChange& c){
    if(!c.operation.valid() || c.expected_configuration_revision<=0)return false;
    if(c.presentation_mask>1 || (c.presentation_mask && c.action!=MessageConfigurationAction::echo && c.action!=MessageConfigurationAction::insert_echo))return false;
    if(c.action!=MessageConfigurationAction::private_presentation && (c.expected_private_presentation_revision || c.private_pipe_colors))return false;
    if(c.retention_mask>3 || (c.retention_mask && c.action!=MessageConfigurationAction::echo &&
        c.action!=MessageConfigurationAction::insert_echo && c.action!=MessageConfigurationAction::clone_permissions))return false;
    if(c.action!=MessageConfigurationAction::unread_mail_retention && c.expected_unread_retention_revision)return false;
    switch(c.action){
    case MessageConfigurationAction::unread_mail_retention:
        return c.expected_unread_retention_revision>0 && (!c.unread_mail_age_days ||
            (*c.unread_mail_age_days>=1 && *c.unread_mail_age_days<=kMaximumMessageAgeDays));
    case MessageConfigurationAction::private_presentation:return c.expected_private_presentation_revision>0;
    case MessageConfigurationAction::delete_group:return c.group.id.value>0 && c.group.revision>0;
    case MessageConfigurationAction::group:
        return c.group.id.value>=0 && c.group.revision>=0 && (c.group.id.value==0 || c.group.revision>0) && c.group.order>=0 && name_valid(c.group.name) && minimum(c.group.access);
    case MessageConfigurationAction::echo:
        return c.echo.id.value>=0 && c.echo.revision>=0 && (c.echo.id.value==0 || c.echo.revision>0) && c.echo.group.value>0 && c.echo.order>=0 && name_valid(c.echo.name) &&
            (c.echo.id.value==0 ? c.echo.conference_number==0 : minimum(c.echo.conference_number)) && minimum(c.echo.permissions.access) && minimum(c.echo.permissions.reading) && minimum(c.echo.permissions.posting) && valid_message_retention_policy(c.echo.retention);
    case MessageConfigurationAction::insert_echo: {
        auto normal=c;normal.action=MessageConfigurationAction::echo;
        return c.echo.id.value==0 && c.insert_before.value>0 && c.insert_before_revision>0 && valid_message_configuration_change(normal);
    }
    case MessageConfigurationAction::delete_echo:
    case MessageConfigurationAction::move_echo_up:
    case MessageConfigurationAction::move_echo_down:return c.echo.id.value>0 && c.echo.revision>0;
    case MessageConfigurationAction::clone_permissions: {
        if(c.permission_mask>7 || c.clone_source.value<=0 || c.source_revision<=0 || c.clone_targets.size()>999)return false;
        std::set<std::int64_t> unique;
        for(const auto& t:c.clone_targets)if(t.echo.value<=0 || t.echo==c.clone_source || t.revision<=0 || !unique.insert(t.echo.value).second)return false;
        return true;
    }
    case MessageConfigurationAction::system_sysop:return c.sysop>0 && c.expected_assignment_revision>0 && c.expected_account_revision>0;
    case MessageConfigurationAction::line_limit:return minimum(c.security_level) && c.maximum_lines>=1 && c.maximum_lines<=kMaximumMessageLines && c.expected_limit_revision>0;
    }
    return false;
}
LocalMessageConfigurationSnapshot LocalMessageAdministration::snapshot(const LocalMessageConfigurationQuery& q){
    if(authority_!=LocalAccountAuthority::installation_owner)return {LocalAccountResultCode::unauthorized};
    if((q.group && q.group->value<=0) || !valid_message_catalog_cursor(q.cursor))return {LocalAccountResultCode::invalid_request};
    const auto r=storage_.message_configuration({true,{}},q.group,q.cursor);
    if(!r || !r.value)return {result_code(r.status.code)};
    return {LocalAccountResultCode::ok,*r.value};
}
LocalMessageConfigurationResult LocalMessageAdministration::execute(const MessageConfigurationChange& c){
    if(authority_!=LocalAccountAuthority::installation_owner)return {LocalAccountResultCode::unauthorized};
    if(!valid_message_configuration_change(c))return {LocalAccountResultCode::invalid_request};
    const auto r=storage_.change_message_configuration({true,{}},c);
    if(!r || !r.value)return {result_code(r.status.code)};
    return {LocalAccountResultCode::ok,*r.value};
}
}
