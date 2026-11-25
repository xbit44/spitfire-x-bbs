#include "sfx/application/local_file_administration.hpp"
#include <algorithm>
#include <limits>
#include <set>

namespace sfx::application {
namespace {
LocalAccountResultCode file_admin_code(StorageErrorCode code){
    if(code==StorageErrorCode::conflict)return LocalAccountResultCode::conflict;
    if(code==StorageErrorCode::invalid_argument)return LocalAccountResultCode::invalid_request;
    if(code==StorageErrorCode::permission_denied || code==StorageErrorCode::not_found)return LocalAccountResultCode::policy_rejected;
    return LocalAccountResultCode::storage_failure;
}
template<class T,class Id,class Predicate>
void configuration_page(const std::vector<T>& source,std::vector<T>& output,std::int64_t after,
    std::int64_t& next,Id id,Predicate included){
    std::vector<const T*> ordered;
    for(const auto& item:source)if(id(item)>after&&included(item))ordered.push_back(&item);
    std::sort(ordered.begin(),ordered.end(),[&](const T* a,const T* b){return id(*a)<id(*b);});
    for(const auto* item:ordered){
        if(output.size()==kLocalFileConfigurationPageSize){next=id(output.back());break;}
        output.push_back(*item);
    }
}
}
bool valid_file_configuration_path(std::string_view path) noexcept {
#if defined(__linux__)
    if(path.size()>2048)return false;
    if(!path.empty()&&path.front()=='/'){
        if(path.size()==1||path[1]=='/'||path.find_first_of("\\:")!=path.npos)return false;
        path.remove_prefix(1);
    }
#endif
    if(path.empty()||path.size()>2048||!valid_file_utf8(path)||path.front()=='/'||path.front()=='\\')return false;
    const bool absolute=path.size()>3&&path[1]==':'&&(path[2]=='/'||path[2]=='\\')&&
        ((path[0]>='A'&&path[0]<='Z')||(path[0]>='a'&&path[0]<='z'));
    if(path.find(':')!=std::string_view::npos&&(!absolute||path.find(':',2)!=std::string_view::npos))return false;
    if(absolute)path.remove_prefix(3);
    while(!path.empty()){
        const auto end=path.find_first_of("/\\");const auto component=path.substr(0,end);
        if(!valid_file_name(component))return false;
        if(end==std::string_view::npos)return true;
        path.remove_prefix(end+1);if(path.empty())return false;
    }
    return false;
}
bool valid_file_configuration_change(const FileConfigurationChange& c) noexcept {
    constexpr auto maximum=std::numeric_limits<std::int64_t>::max();
    if(!c.operation.valid()||c.expected_catalog_revision<1||c.expected_catalog_revision==maximum)return false;
    switch(c.action){
    case FileConfigurationAction::policy:return valid_file_policy(c.policy)&&c.policy.revision<maximum&&c.policy.catalog_revision==c.expected_catalog_revision;
    case FileConfigurationAction::group:return valid_file_group(c.group)&&c.group.order>=0&&c.group.revision<maximum;
    case FileConfigurationAction::area:return valid_file_area(c.area.area)&&c.area.area.order>=0&&c.area.area.revision<maximum&&valid_file_configuration_path(c.area.storage_path)&&
        (c.area.area.id.value?c.area.area.number>0:(c.area.area.number==0&&c.area.area.kind==FileAreaKind::ordinary));
    case FileConfigurationAction::security_rule:return c.security.level>=1&&c.security.level<=999&&c.security.revision>=1&&c.security.revision<maximum&&valid_file_count_rule(c.security.counts);
    case FileConfigurationAction::caller_exemption:return c.exemption.account>0&&c.exemption.revision>=1&&c.exemption.revision<maximum;
    case FileConfigurationAction::insert_area_before:case FileConfigurationAction::insert_area_after:
        return !c.area.area.id.value&&!c.area.area.number&&c.anchor.value>0&&c.anchor_revision>0&&c.anchor_revision<maximum&&valid_file_area(c.area.area)&&c.area.area.kind==FileAreaKind::ordinary&&valid_file_configuration_path(c.area.storage_path);
    case FileConfigurationAction::move_area_up:case FileConfigurationAction::move_area_down:
        return c.area.area.id.value>0&&valid_file_area(c.area.area)&&c.area.area.revision<maximum;
    case FileConfigurationAction::preview_group_delete:case FileConfigurationAction::begin_group_delete:case FileConfigurationAction::finish_group_delete:
        return c.group.id.value>0&&valid_file_group(c.group)&&c.group.revision<maximum;
    }
    return false;
}
bool valid_file_configuration_query(const LocalFileConfigurationQuery& q) noexcept {
    if(q.part>FileConfigurationPart::caller_exemption||q.catalog_revision<0||q.after_id<0||q.group.value<0||q.account<0||(!q.catalog_revision&&q.after_id))return false;
    if(q.group.value&&q.part!=FileConfigurationPart::areas)return false;
    if(q.part==FileConfigurationPart::caller_exemption)return q.account>0&&q.after_id==0;
    return !q.account&&(q.part!=FileConfigurationPart::policy||q.after_id==0);
}
bool valid_file_configuration_snapshot(const LocalFileConfigurationSnapshot& s) noexcept {
    if(s.code>LocalAccountResultCode::system_sysop_protected)return false;
    if(s.code!=LocalAccountResultCode::ok)return s.groups.empty()&&s.areas.empty()&&s.security_rules.empty()&&!s.exemption&&!s.next_after;
    if(s.part>FileConfigurationPart::caller_exemption||!valid_file_policy(s.policy)||s.next_after<0||
        s.groups.size()>kLocalFileConfigurationPageSize||s.areas.size()>kLocalFileConfigurationPageSize||s.security_rules.size()>kLocalFileConfigurationPageSize)return false;
    if((s.part!=FileConfigurationPart::groups&&!s.groups.empty())||(s.part!=FileConfigurationPart::areas&&!s.areas.empty())||
        (s.part!=FileConfigurationPart::security_rules&&!s.security_rules.empty())||(s.part==FileConfigurationPart::caller_exemption)!=s.exemption.has_value())return false;
    std::int64_t previous{};
    for(const auto& g:s.groups){if(!valid_file_group(g)||g.order<0||g.id.value<=previous)return false;previous=g.id.value;}
    for(const auto& a:s.areas){if(!valid_file_area(a.area)||a.area.order<0||!a.area.number||!valid_file_configuration_path(a.storage_path)||a.area.id.value<=previous)return false;previous=a.area.id.value;}
    for(const auto& r:s.security_rules){if(r.level<=previous||r.level>999||r.revision<1||r.inherit||!valid_file_count_rule(r.counts))return false;previous=r.level;}
    if(s.exemption&&(s.exemption->account<=0||s.exemption->revision<1))return false;
    return !s.next_after||(s.next_after==previous&&
        s.groups.size()+s.areas.size()+s.security_rules.size()==kLocalFileConfigurationPageSize);
}
LocalFileConfigurationSnapshot LocalFileAdministration::snapshot(const LocalFileConfigurationQuery& q){
    if(authority_!=LocalAccountAuthority::installation_owner)return {LocalAccountResultCode::unauthorized};
    if(!valid_file_configuration_query(q))return {LocalAccountResultCode::invalid_request};
    const auto full=storage_.file_configuration({true,{}});
    if(!full||!full.value)return {file_admin_code(full.status.code)};
    if(q.catalog_revision&&q.catalog_revision!=full.value->policy.catalog_revision)return {LocalAccountResultCode::conflict};
    LocalFileConfigurationSnapshot s;s.code=LocalAccountResultCode::ok;s.part=q.part;s.policy=full.value->policy;
    const auto all=[](const auto&){return true;};
    switch(q.part){
    case FileConfigurationPart::policy:break;
    case FileConfigurationPart::groups:configuration_page(full.value->groups,s.groups,q.after_id,s.next_after,[](const auto& g){return g.id.value;},all);break;
    case FileConfigurationPart::areas:configuration_page(full.value->areas,s.areas,q.after_id,s.next_after,[](const auto& a){return a.area.id.value;},[&](const auto& a){return !q.group.value||a.area.group==q.group;});break;
    case FileConfigurationPart::security_rules:configuration_page(full.value->security_rules,s.security_rules,q.after_id,s.next_after,[](const auto& r){return r.level;},all);break;
    case FileConfigurationPart::caller_exemption:{
        const auto exemption=storage_.file_caller_exemption({true,{}},q.account);
        if(!exemption||!exemption.value)return {file_admin_code(exemption.status.code)};
        // This second read must not silently mix revisions with the catalog.
        const auto current=storage_.file_configuration({true,{}});
        if(!current||!current.value)return {LocalAccountResultCode::storage_failure};
        if(current.value->policy.catalog_revision!=s.policy.catalog_revision)return {LocalAccountResultCode::conflict};
        s.exemption=*exemption.value;break;
    }
    }
    return valid_file_configuration_snapshot(s)?s:LocalFileConfigurationSnapshot{LocalAccountResultCode::storage_failure};
}
LocalFileConfigurationResult LocalFileAdministration::execute(const FileConfigurationChange& c){
    if(authority_!=LocalAccountAuthority::installation_owner)return {LocalAccountResultCode::unauthorized};
    if(!valid_file_configuration_change(c)||c.action==FileConfigurationAction::finish_group_delete)return {LocalAccountResultCode::invalid_request};
    if(c.action==FileConfigurationAction::preview_group_delete||c.action==FileConfigurationAction::begin_group_delete){
        if(!paths_)return {LocalAccountResultCode::unavailable};
        return paths_->group_deletion(storage_,c);
    }
    std::unique_ptr<IFileConfigurationLease> lease;
    if(c.action==FileConfigurationAction::area||c.action==FileConfigurationAction::insert_area_before||c.action==FileConfigurationAction::insert_area_after){
        if(!paths_)return {LocalAccountResultCode::unavailable};
        const auto current=storage_.file_configuration({true,{}});
        if(!current||!current.value)return {file_admin_code(current.status.code)};
        if(c.expected_catalog_revision<current.value->policy.catalog_revision){
            // Revisions only advance. The storage transaction can return an
            // exact existing receipt or a conflict here, but cannot create a
            // new area/change a path under this stale expected revision. This
            // preserves lost-response replay without re-adopting its directory.
            const auto replay=storage_.change_file_configuration({true,{}},c);
            if(!replay||!replay.value)return {file_admin_code(replay.status.code)};
            return replay.value->replayed?LocalFileConfigurationResult{LocalAccountResultCode::ok,*replay.value}:
                LocalFileConfigurationResult{LocalAccountResultCode::storage_failure};
        }
        if(c.expected_catalog_revision!=current.value->policy.catalog_revision)return {LocalAccountResultCode::conflict,{},"File settings changed since this page was loaded. Reload and review the draft."};
        try{lease=paths_->acquire(*current.value,c);}catch(const std::exception& e){
            const std::string why=e.what();std::string detail="The selected folder could not be safely opened. Check its existence and permissions.";
            if(why.find("owner")!=why.npos||why.find("SID")!=why.npos)detail="The folder must belong to the SFX installation owner.";
            else if(why.find("DACL")!=why.npos||why.find("private")!=why.npos){
#if defined(__linux__)
                detail="The folder must be private to the Linux account running SFX, with owner read, write and search permissions.";
#else
                detail="Folder permissions must allow only the SFX owner, SYSTEM and Administrators with the required rights.";
#endif
            }
            else if(why.find("overlap")!=why.npos)detail="This folder overlaps another File Area or a protected SFX folder. Choose a separate folder.";
            else if(why.find("ntfs")!=why.npos)detail="The folder must be on a local fixed NTFS volume.";
            else if(why.find("alias")!=why.npos||why.find("link")!=why.npos||why.find("path")!=why.npos||why.find("role")!=why.npos)detail="Choose a normal existing folder in the SFX Files tree or a permitted separate location; aliases and links are refused.";
            return {LocalAccountResultCode::policy_rejected,{},detail};
        }catch(...){return {LocalAccountResultCode::policy_rejected,{},"The selected folder could not be validated. Draft retained."};}
        if(!lease)return {LocalAccountResultCode::policy_rejected};
    }
    try{if(lease)lease->verify();}catch(...){return {LocalAccountResultCode::policy_rejected,{},"The selected folder changed before saving. Reload and review the draft."};}
    const auto saved=storage_.change_file_configuration({true,{}},c);
    if(!saved||!saved.value)return {file_admin_code(saved.status.code),{},saved.status.code==StorageErrorCode::conflict?"File settings changed. Reload and review before saving again.":"The file settings could not be saved. Check duplicate folders, current group and pending operations."};
    return {LocalAccountResultCode::ok,*saved.value};
}
} // namespace sfx::application
